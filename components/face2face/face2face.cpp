#include "face2face.h"
#include "esphome/core/log.h"

#include <cstring>
#include <cstdlib>

// lwIP / POSIX sockets (ESP-IDF)
#include <lwip/sockets.h>
#include <lwip/netdb.h>

// Peer ESPHome components
#include "esphome/components/esp_cam_sensor/esp_cam_sensor_camera.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/audio/audio.h"

// ESP32-P4 hardware JPEG codec (built-in IDF component esp_driver_jpeg).
// NOTE: a few enum/field names below may differ slightly between IDF versions.
// If the build complains, check driver/jpeg_encode.h and driver/jpeg_decode.h
// in *your* installed ESP-IDF and adjust the names.
#include "driver/jpeg_encode.h"
#include "driver/jpeg_decode.h"

// Acoustic echo cancellation (Espressif ESP-SR). Only compiled when enabled in
// YAML (enable_aec), which also pulls the esp-sr managed component.
#ifdef FACE2FACE_USE_AEC
#include "esp_aec.h"
#include "esp_heap_caps.h"
#endif

namespace esphome {
namespace face2face {

static const char *const TAG = "face2face";

float Face2Face::get_setup_priority() const { return setup_priority::AFTER_CONNECTION; }

void FrameAssembler::reset(uint16_t id, uint32_t size, uint16_t count) {
  frame_id = id;
  frame_size = size;
  frag_count = count;
  frags_seen = 0;
  active = true;
  data.assign(size, 0);
  got.assign(count, false);
}

// ===========================================================================
// Lifecycle
// ===========================================================================
void Face2Face::setup() {
  ESP_LOGCONFIG(TAG, "Setting up face2face...");
  if (!open_sockets_()) {
    this->mark_failed();
    return;
  }
  // Heavy media buffers (hardware JPEG codec, RGB framebuffer, AEC) are NOT
  // allocated here. They are created lazily in ensure_media_() when a call
  // starts and freed in release_media_() on hangup, so almost no RAM/PSRAM is
  // used while idle on the LVGL UI.
  if (audio_enabled_ && mic_ != nullptr) {
    mic_->add_data_callback([this](const std::vector<uint8_t> &data) { this->on_mic_data_(data); });
  }
  if (audio_enabled_ && spk_ != nullptr) {
    spk_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, audio_sample_rate_));
  }
  ESP_LOGCONFIG(TAG, "face2face ready (peer=%s v:%u a:%u) - media allocated per-call",
                peer_ip_.c_str(), video_port_, audio_port_);
}

void Face2Face::loop() {
  if (!sockets_ready_)
    return;

  poll_receive_();

  uint32_t now_ms = millis();

  // Heartbeat (presence), even when idle.
  if (now_ms - last_ping_tx_ms_ >= 1000) {
    last_ping_tx_ms_ = now_ms;
    send_ping_();
  }

  // Ring/dial timeout: give up if the peer never answers.
  if ((state_ == STATE_OUTGOING || state_ == STATE_RINGING) &&
      (now_ms - state_since_ms_) > ring_timeout_ms_) {
    ESP_LOGI(TAG, "Call setup timed out");
    if (state_ == STATE_OUTGOING)
      send_ctrl_(CTRL_HANGUP);
    else
      send_ctrl_(CTRL_DECLINE);
    go_idle_();
  }

  // Send our video while streaming, rate-limited to framerate_.
  if (state_ == STATE_STREAMING && jpeg_ready_ && camera_ != nullptr) {
    uint32_t now = micros();
    uint32_t period = 1000000UL / framerate_;
    if (now - last_tx_us_ >= period) {
      last_tx_us_ = now;
      pump_video_tx_();
    }
  }
}

void Face2Face::dump_config() {
  ESP_LOGCONFIG(TAG, "face2face:");
  ESP_LOGCONFIG(TAG, "  Peer: %s  (video:%u audio:%u)", peer_ip_.c_str(), video_port_, audio_port_);
  ESP_LOGCONFIG(TAG, "  Video: %ux%u @ %u fps, JPEG q=%u", width_, height_, framerate_, jpeg_quality_);
  ESP_LOGCONFIG(TAG, "  Audio: %s @ %u Hz  (mic:%s spk:%s)", YESNO(audio_enabled_), audio_sample_rate_,
                mic_ ? "yes" : "no", spk_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  AEC: %s (allocated per-call)", aec_enabled_ ? "enabled" : "off");
  ESP_LOGCONFIG(TAG, "  Ring timeout: %u ms, auto-answer: %s", ring_timeout_ms_, YESNO(auto_answer_));
}

// ===========================================================================
// Call FSM  (native signaling — replaces the external intercom)
// ===========================================================================
void Face2Face::set_state_(CallState s) {
  if (state_ == s)
    return;
  state_ = s;
  state_since_ms_ = millis();
}

void Face2Face::call() {
  if (state_ != STATE_IDLE) {
    ESP_LOGW(TAG, "call() ignored: not idle (state=%d)", state_);
    return;
  }
  ESP_LOGI(TAG, "Calling %s ...", peer_ip_.c_str());
  set_state_(STATE_OUTGOING);
  send_ctrl_(CTRL_CALL);
  if (on_outgoing_ != nullptr)
    on_outgoing_->trigger();
}

void Face2Face::answer() {
  if (state_ != STATE_RINGING) {
    ESP_LOGW(TAG, "answer() ignored: not ringing");
    return;
  }
  ESP_LOGI(TAG, "Answering call");
  send_ctrl_(CTRL_ANSWER);
  start_streaming_();
}

void Face2Face::decline() {
  if (state_ != STATE_RINGING)
    return;
  ESP_LOGI(TAG, "Declining call");
  send_ctrl_(CTRL_DECLINE);
  go_idle_();
}

void Face2Face::hangup() {
  if (state_ == STATE_IDLE)
    return;
  ESP_LOGI(TAG, "Hanging up");
  send_ctrl_(CTRL_HANGUP);
  go_idle_();
}

void Face2Face::start_streaming_() {
  // Allocate the heavy media buffers now (freed again on hangup).
  if (!ensure_media_()) {
    ESP_LOGE(TAG, "Cannot start call: media allocation failed (low memory?)");
    send_ctrl_(CTRL_HANGUP);
    go_idle_();
    return;
  }
  set_state_(STATE_STREAMING);
  // Fire on_streaming first so YAML can stop wake-word / voice_assistant and
  // release the microphone *before* we start capturing it ourselves.
  if (on_streaming_ != nullptr)
    on_streaming_->trigger();
  if (camera_ != nullptr && !camera_->is_streaming()) {
    camera_->start_streaming();
    camera_started_ = true;
  }
  if (audio_enabled_) {
    if (spk_ != nullptr)
      spk_->start();
    if (mic_ != nullptr && !mic_->is_running()) {
      mic_->start();
      mic_started_ = true;
    }
  }
  ESP_LOGI(TAG, "Call established (streaming)");
}

void Face2Face::go_idle_() {
  bool was_active = (state_ != STATE_IDLE);
  set_state_(STATE_IDLE);
  if (audio_enabled_) {
    if (mic_ != nullptr && mic_started_) {
      mic_->stop();
      mic_started_ = false;
    }
    if (spk_ != nullptr)
      spk_->stop();
  }
  // Stop the camera if WE started it (frees its DMA frame buffers).
  if (camera_started_ && camera_ != nullptr) {
    camera_->stop_streaming();
    camera_started_ = false;
  }
  // Free the JPEG codec, framebuffer and AEC buffers (~several MB of PSRAM).
  release_media_();
  if (was_active && on_idle_ != nullptr)
    on_idle_->trigger();
}

void Face2Face::on_ctrl_(uint8_t type) {
  switch (type) {
    case CTRL_CALL:
      if (state_ == STATE_IDLE) {
        set_state_(STATE_RINGING);
        send_ctrl_(CTRL_RING);  // tell caller we are presenting the call
        ESP_LOGI(TAG, "Incoming call");
        if (on_ringing_ != nullptr)
          on_ringing_->trigger();
        if (auto_answer_)
          answer();
      } else if (state_ == STATE_STREAMING) {
        // Already in a call with this peer: re-ack to be safe.
        send_ctrl_(CTRL_ANSWER);
      }
      break;
    case CTRL_RING:
      // Provisional ringback; UI may already show "calling".
      break;
    case CTRL_ANSWER:
      if (state_ == STATE_OUTGOING)
        start_streaming_();
      break;
    case CTRL_HANGUP:
      if (state_ != STATE_IDLE)
        go_idle_();
      break;
    case CTRL_DECLINE:
      if (state_ == STATE_OUTGOING) {
        ESP_LOGI(TAG, "Call declined by peer");
        go_idle_();
      }
      break;
    default:
      break;
  }
}

void Face2Face::send_ctrl_(F2FCtrl type) {
  struct sockaddr_in dst {};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(video_port_);  // control rides the video/control port
  ::inet_aton(peer_ip_.c_str(), &dst.sin_addr);

  uint8_t pkt[F2F_HEADER_SIZE + 1];
  auto *hdr = reinterpret_cast<F2FHeader *>(pkt);
  *hdr = F2FHeader{};
  hdr->magic = F2F_MAGIC;
  hdr->stream = F2F_STREAM_CTRL;
  hdr->flags = F2F_FLAG_LAST;
  hdr->frag_count = 1;
  hdr->frame_size = 1;
  hdr->payload_len = 1;
  pkt[F2F_HEADER_SIZE] = (uint8_t) type;
  // UDP control is best-effort; send a few copies so setup survives loss.
  for (int i = 0; i < 4; i++)
    ::sendto(video_sock_, pkt, sizeof(pkt), 0, (struct sockaddr *) &dst, sizeof(dst));
}

// ===========================================================================
// Networking
// ===========================================================================
bool Face2Face::open_sockets_() {
  for (int i = 0; i < 2; i++) {
    int sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
      ESP_LOGE(TAG, "socket() failed: errno %d", errno);
      return false;
    }
    int flags = ::fcntl(sock, F_GETFL, 0);
    ::fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    int rxbuf = 65536;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rxbuf, sizeof(rxbuf));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(i == 0 ? video_port_ : audio_port_);
    if (::bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
      ESP_LOGE(TAG, "bind() failed: errno %d", errno);
      ::close(sock);
      return false;
    }
    (i == 0 ? video_sock_ : audio_sock_) = sock;
  }
  sockets_ready_ = true;
  return true;
}

void Face2Face::send_ping_() {
  struct sockaddr_in dst {};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(video_port_);
  ::inet_aton(peer_ip_.c_str(), &dst.sin_addr);
  F2FHeader h{};
  h.magic = F2F_MAGIC;
  h.stream = F2F_STREAM_PING;
  h.flags = F2F_FLAG_LAST;
  h.frag_count = 1;
  ::sendto(video_sock_, &h, sizeof(h), 0, (struct sockaddr *) &dst, sizeof(dst));
}

void Face2Face::send_frame_(F2FStream stream, const uint8_t *data, uint32_t len, int sock) {
  struct sockaddr_in dst {};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(stream == F2F_STREAM_VIDEO ? video_port_ : audio_port_);
  ::inet_aton(peer_ip_.c_str(), &dst.sin_addr);

  uint16_t frag_count = (len + F2F_MAX_PAYLOAD - 1) / F2F_MAX_PAYLOAD;
  if (frag_count == 0)
    frag_count = 1;
  uint16_t frame_id = (stream == F2F_STREAM_VIDEO) ? tx_video_frame_id_++ : tx_audio_frame_id_++;

  uint8_t pkt[F2F_HEADER_SIZE + F2F_MAX_PAYLOAD];
  auto *hdr = reinterpret_cast<F2FHeader *>(pkt);
  hdr->magic = F2F_MAGIC;
  hdr->stream = stream;
  hdr->frame_id = frame_id;
  hdr->frag_count = frag_count;
  hdr->frame_size = len;

  for (uint16_t f = 0; f < frag_count; f++) {
    uint32_t off = (uint32_t) f * F2F_MAX_PAYLOAD;
    uint16_t plen = (len - off) > F2F_MAX_PAYLOAD ? F2F_MAX_PAYLOAD : (uint16_t) (len - off);
    hdr->flags = (f == frag_count - 1) ? F2F_FLAG_LAST : 0;
    hdr->frag_index = f;
    hdr->payload_len = plen;
    std::memcpy(pkt + F2F_HEADER_SIZE, data + off, plen);
    int sent = ::sendto(sock, pkt, F2F_HEADER_SIZE + plen, 0, (struct sockaddr *) &dst, sizeof(dst));
    if (sent < 0 && errno != EWOULDBLOCK) {
      ESP_LOGW(TAG, "sendto failed: errno %d", errno);
      break;
    }
  }
}

void Face2Face::poll_receive_() {
  uint8_t buf[F2F_HEADER_SIZE + F2F_MAX_PAYLOAD];
  for (int i = 0; i < 64; i++) {
    int n = ::recv(video_sock_, buf, sizeof(buf), 0);
    if (n > 0)
      handle_packet_(buf, n, F2F_STREAM_VIDEO);
    else
      break;
  }
  if (audio_enabled_) {
    for (int i = 0; i < 48; i++) {
      int n = ::recv(audio_sock_, buf, sizeof(buf), 0);
      if (n > 0)
        handle_packet_(buf, n, F2F_STREAM_AUDIO);
      else
        break;
    }
  }
}

void Face2Face::handle_packet_(const uint8_t *buf, size_t len, F2FStream expected) {
  if (len < F2F_HEADER_SIZE)
    return;
  auto *hdr = reinterpret_cast<const F2FHeader *>(buf);
  if (hdr->magic != F2F_MAGIC)
    return;
  // Any valid packet from the peer counts as presence.
  last_peer_rx_ms_ = millis();

  if (hdr->stream == F2F_STREAM_PING)
    return;
  if (hdr->stream == F2F_STREAM_CTRL) {
    if (hdr->payload_len >= 1 && F2F_HEADER_SIZE + 1 <= len)
      on_ctrl_(buf[F2F_HEADER_SIZE]);
    return;
  }
  if (hdr->stream != expected)
    return;
  if (F2F_HEADER_SIZE + hdr->payload_len > len)
    return;
  if (hdr->frame_size == 0 || hdr->frag_index >= hdr->frag_count)
    return;

  FrameAssembler &asmb = (expected == F2F_STREAM_VIDEO) ? video_asm_ : audio_asm_;
  if (!asmb.active || asmb.frame_id != hdr->frame_id)
    asmb.reset(hdr->frame_id, hdr->frame_size, hdr->frag_count);
  if (hdr->frame_size != asmb.frame_size || hdr->frag_index >= asmb.got.size())
    return;
  if (asmb.got[hdr->frag_index])
    return;

  uint32_t off = (uint32_t) hdr->frag_index * F2F_MAX_PAYLOAD;
  if (off + hdr->payload_len > asmb.data.size())
    return;
  std::memcpy(asmb.data.data() + off, buf + F2F_HEADER_SIZE, hdr->payload_len);
  asmb.got[hdr->frag_index] = true;
  asmb.frags_seen++;

  if (!asmb.complete())
    return;
  asmb.active = false;

  if (expected == F2F_STREAM_VIDEO) {
    if (decode_jpeg_(asmb.data.data(), asmb.frame_size))
      new_remote_frame_ = true;
  } else {
    play_audio_(asmb.data.data(), asmb.frame_size);
  }
}

// ===========================================================================
// Hardware JPEG codec (esp_driver_jpeg)
// ===========================================================================
bool Face2Face::jpeg_init_() {
  jpeg_encode_engine_cfg_t enc_eng = {};
  enc_eng.timeout_ms = 70;
  if (jpeg_new_encoder_engine(&enc_eng, reinterpret_cast<jpeg_encoder_handle_t *>(&jpeg_enc_)) != ESP_OK) {
    ESP_LOGE(TAG, "jpeg_new_encoder_engine failed");
    return false;
  }
  size_t in_size = (size_t) width_ * height_ * 2;
  jpeg_encode_memory_alloc_cfg_t in_cfg = {};
  in_cfg.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
  size_t in_alloc = 0;
  enc_in_ = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(in_size, &in_cfg, &in_alloc));
  jpeg_encode_memory_alloc_cfg_t out_cfg = {};
  out_cfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  enc_out_cap_ = in_size;
  enc_out_ = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(enc_out_cap_, &out_cfg, &enc_out_cap_));

  jpeg_decode_engine_cfg_t dec_eng = {};
  dec_eng.timeout_ms = 40;
  if (jpeg_new_decoder_engine(&dec_eng, reinterpret_cast<jpeg_decoder_handle_t *>(&jpeg_dec_)) != ESP_OK) {
    ESP_LOGE(TAG, "jpeg_new_decoder_engine failed");
    return false;
  }
  jpeg_decode_memory_alloc_cfg_t din_cfg = {};
  din_cfg.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
  size_t din_alloc = 0;
  dec_in_cap_ = in_size;
  dec_in_ = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(dec_in_cap_, &din_cfg, &din_alloc));
  jpeg_decode_memory_alloc_cfg_t dout_cfg = {};
  dout_cfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
  dec_out_cap_ = in_size;
  dec_out_ = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(dec_out_cap_, &dout_cfg, &dec_out_cap_));

  if (enc_in_ == nullptr || enc_out_ == nullptr || dec_in_ == nullptr || dec_out_ == nullptr) {
    ESP_LOGE(TAG, "JPEG DMA buffer allocation failed");
    return false;
  }
  jpeg_ready_ = true;
  ESP_LOGCONFIG(TAG, "Hardware JPEG codec ready");
  return true;
}

void Face2Face::jpeg_deinit_() {
  if (jpeg_enc_ != nullptr) {
    jpeg_del_encoder_engine(reinterpret_cast<jpeg_encoder_handle_t>(jpeg_enc_));
    jpeg_enc_ = nullptr;
  }
  if (jpeg_dec_ != nullptr) {
    jpeg_del_decoder_engine(reinterpret_cast<jpeg_decoder_handle_t>(jpeg_dec_));
    jpeg_dec_ = nullptr;
  }
  if (enc_in_ != nullptr) { free(enc_in_); enc_in_ = nullptr; }
  if (enc_out_ != nullptr) { free(enc_out_); enc_out_ = nullptr; }
  if (dec_in_ != nullptr) { free(dec_in_); dec_in_ = nullptr; }
  if (dec_out_ != nullptr) { free(dec_out_); dec_out_ = nullptr; }
  jpeg_ready_ = false;
}

// Allocate the per-call media buffers (idempotent).
bool Face2Face::ensure_media_() {
  if (jpeg_ready_)
    return true;
  if (!jpeg_init_())
    return false;
  remote_fb_.assign((size_t) width_ * height_ * 2, 0);
  if (audio_enabled_ && aec_enabled_)
    aec_init_();  // best-effort; passes through raw audio if it fails
  return true;
}

// Free everything allocated by ensure_media_ so idle RAM/PSRAM use is minimal.
void Face2Face::release_media_() {
  jpeg_deinit_();
  aec_deinit_();
  std::vector<uint8_t>().swap(remote_fb_);  // release capacity, not just size
  remote_w_ = 0;
  remote_h_ = 0;
  new_remote_frame_ = false;
  video_asm_ = FrameAssembler{};
  audio_asm_ = FrameAssembler{};
}

void Face2Face::pump_video_tx_() {
  esp_cam_sensor::SimpleBufferElement *el = nullptr;
  uint8_t *rgb = nullptr;
  int w = 0, h = 0;
  if (!camera_->get_current_rgb_frame(&el, &rgb, &w, &h) || rgb == nullptr)
    return;

  size_t frame_bytes = (size_t) w * h * 2;
  if (frame_bytes <= (size_t) width_ * height_ * 2) {
    std::memcpy(enc_in_, rgb, frame_bytes);
    jpeg_encode_cfg_t cfg = {};
    cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
    cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
    cfg.image_quality = jpeg_quality_;
    cfg.width = w;
    cfg.height = h;
    uint32_t out_size = 0;
    esp_err_t err = jpeg_encoder_process(reinterpret_cast<jpeg_encoder_handle_t>(jpeg_enc_), &cfg, enc_in_,
                                         frame_bytes, enc_out_, enc_out_cap_, &out_size);
    if (err == ESP_OK && out_size > 0)
      send_frame_(F2F_STREAM_VIDEO, enc_out_, out_size, video_sock_);
    else
      ESP_LOGW(TAG, "jpeg encode failed: %d", err);
  }
  camera_->release_buffer(el);
}

bool Face2Face::decode_jpeg_(const uint8_t *jpeg, uint32_t len) {
  if (!jpeg_ready_ || len > dec_in_cap_)
    return false;
  std::memcpy(dec_in_, jpeg, len);

  jpeg_decode_picture_info_t info = {};
  if (jpeg_decoder_get_info(dec_in_, len, &info) != ESP_OK)
    return false;

  size_t want = (size_t) info.width * info.height * 2;
  if (want == 0 || want > dec_out_cap_)
    return false;

  jpeg_decode_cfg_t cfg = {};
  cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;

  uint32_t out_len = 0;
  if (jpeg_decoder_process(reinterpret_cast<jpeg_decoder_handle_t>(jpeg_dec_), &cfg, dec_in_, len, dec_out_,
                           dec_out_cap_, &out_len) != ESP_OK)
    return false;

  remote_w_ = info.width;
  remote_h_ = info.height;
  size_t n = out_len < want ? out_len : want;
  if (remote_fb_.size() != want)
    remote_fb_.assign(want, 0);
  std::memcpy(remote_fb_.data(), dec_out_, n);
  return true;
}

// ===========================================================================
// Audio (ESPHome microphone -> [AEC] -> UDP -> speaker)
// ===========================================================================
bool Face2Face::aec_init_() {
#ifdef FACE2FACE_USE_AEC
  if (mic_ == nullptr || spk_ == nullptr) {
    ESP_LOGW(TAG, "AEC needs both microphone and speaker; disabled");
    return false;
  }
  auto *h = aec_create((int) audio_sample_rate_, aec_filter_length_, 1, (aec_mode_t) aec_mode_);
  if (h == nullptr) {
    ESP_LOGW(TAG, "aec_create failed");
    return false;
  }
  aec_handle_ = h;
  aec_chunk_ = aec_get_chunksize(h);
  if (aec_chunk_ <= 0) {
    aec_destroy(h);
    aec_handle_ = nullptr;
    return false;
  }
  size_t bytes = (size_t) aec_chunk_ * sizeof(int16_t);
  aec_in_ = static_cast<int16_t *>(heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_DEFAULT));
  aec_ref_ = static_cast<int16_t *>(heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_DEFAULT));
  aec_out_ = static_cast<int16_t *>(heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_DEFAULT));
  if (aec_in_ == nullptr || aec_ref_ == nullptr || aec_out_ == nullptr) {
    ESP_LOGW(TAG, "AEC buffer alloc failed");
    return false;
  }
  ref_cap_ = (size_t) aec_chunk_ * 32;  // up to ~0.5-1 s of reference history
  ref_buf_.assign(ref_cap_, 0);
  ref_head_ = 0;
  ref_count_ = 0;
  mic_acc_.clear();
  send_acc_.clear();
  aec_ready_ = true;
  ESP_LOGCONFIG(TAG, "AEC ready (chunk=%d samples, filter=%d, mode=%d)", aec_chunk_, aec_filter_length_, aec_mode_);
  return true;
#else
  ESP_LOGW(TAG, "AEC requested but not compiled in (enable_aec pulls esp-sr)");
  return false;
#endif
}

void Face2Face::aec_deinit_() {
#ifdef FACE2FACE_USE_AEC
  if (aec_handle_ != nullptr) {
    aec_destroy(static_cast<aec_handle_t *>(aec_handle_));
    aec_handle_ = nullptr;
  }
  if (aec_in_ != nullptr) { free(aec_in_); aec_in_ = nullptr; }
  if (aec_ref_ != nullptr) { free(aec_ref_); aec_ref_ = nullptr; }
  if (aec_out_ != nullptr) { free(aec_out_); aec_out_ = nullptr; }
#endif
  std::vector<int16_t>().swap(mic_acc_);
  std::vector<int16_t>().swap(send_acc_);
  std::vector<int16_t>().swap(ref_buf_);
  ref_cap_ = 0;
  ref_head_ = 0;
  ref_count_ = 0;
  aec_chunk_ = 0;
  aec_ready_ = false;
}

void Face2Face::ref_push_(const int16_t *d, size_t n) {
  if (ref_cap_ == 0)
    return;
  for (size_t i = 0; i < n; i++) {
    size_t t = (ref_head_ + ref_count_) % ref_cap_;
    ref_buf_[t] = d[i];
    if (ref_count_ < ref_cap_)
      ref_count_++;
    else
      ref_head_ = (ref_head_ + 1) % ref_cap_;  // overwrite oldest
  }
}

void Face2Face::ref_pop_(int16_t *d, size_t n) {
  size_t k = 0;
  while (k < n && ref_count_ > 0) {
    d[k++] = ref_buf_[ref_head_];
    ref_head_ = (ref_head_ + 1) % ref_cap_;
    ref_count_--;
  }
  while (k < n)  // underflow (speaker silent) -> zero reference = no echo
    d[k++] = 0;
}

void Face2Face::on_mic_data_(const std::vector<uint8_t> &data) {
  if (state_ != STATE_STREAMING || data.empty())
    return;

#ifdef FACE2FACE_USE_AEC
  if (aec_ready_) {
    const int16_t *in = reinterpret_cast<const int16_t *>(data.data());
    mic_acc_.insert(mic_acc_.end(), in, in + data.size() / 2);
    send_acc_.clear();
    size_t off = 0;
    while (mic_acc_.size() - off >= (size_t) aec_chunk_) {
      std::memcpy(aec_in_, mic_acc_.data() + off, (size_t) aec_chunk_ * sizeof(int16_t));
      ref_pop_(aec_ref_, aec_chunk_);  // time-aligned far-end reference
      aec_process(static_cast<const aec_handle_t *>(aec_handle_), aec_in_, aec_ref_, aec_out_);
      send_acc_.insert(send_acc_.end(), aec_out_, aec_out_ + aec_chunk_);
      off += aec_chunk_;
    }
    if (off > 0)
      mic_acc_.erase(mic_acc_.begin(), mic_acc_.begin() + off);
    if (!send_acc_.empty())
      send_frame_(F2F_STREAM_AUDIO, reinterpret_cast<const uint8_t *>(send_acc_.data()),
                  send_acc_.size() * sizeof(int16_t), audio_sock_);
    return;
  }
#endif
  send_frame_(F2F_STREAM_AUDIO, data.data(), data.size(), audio_sock_);
}

void Face2Face::play_audio_(const uint8_t *pcm, uint32_t len) {
  if (spk_ == nullptr || len == 0 || state_ != STATE_STREAMING)
    return;
#ifdef FACE2FACE_USE_AEC
  // The far-end audio we are about to play is the echo reference for the mic.
  if (aec_ready_)
    ref_push_(reinterpret_cast<const int16_t *>(pcm), len / 2);
#endif
  spk_->play(pcm, len);
}

}  // namespace face2face
}  // namespace esphome
