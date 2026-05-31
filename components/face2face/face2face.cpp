#include "face2face.h"
#include "esphome/core/log.h"

#include <cstring>

// lwIP / POSIX sockets (ESP-IDF)
#include <lwip/sockets.h>
#include <lwip/netdb.h>

// Peer ESPHome components
#include "esphome/components/esp_cam_sensor/esp_cam_sensor_camera.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/audio/audio.h"

// ESP32-P4 hardware JPEG codec (built-in IDF component esp_driver_jpeg).
// NOTE: a few enum/field names below may differ slightly between IDF versions
// (5.3 / 5.4 / 5.5). If the build complains, check driver/jpeg_encode.h and
// driver/jpeg_decode.h in *your* installed ESP-IDF and adjust the names.
#include "driver/jpeg_encode.h"
#include "driver/jpeg_decode.h"

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
  remote_fb_.assign((size_t) width_ * height_ * 2, 0);

  if (!jpeg_init_()) {
    ESP_LOGE(TAG, "JPEG hardware codec init failed");
    this->mark_failed();
    return;
  }

  // Microphone delivers raw PCM via a callback; we only forward it while a
  // call is active.
  if (audio_enabled_ && mic_ != nullptr) {
    mic_->add_data_callback([this](const std::vector<uint8_t> &data) { this->on_mic_data_(data); });
  }
  if (audio_enabled_ && spk_ != nullptr) {
    spk_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, audio_sample_rate_));
  }
  ESP_LOGCONFIG(TAG, "face2face ready (peer=%s v:%u a:%u)", peer_ip_.c_str(), video_port_, audio_port_);
}

void Face2Face::loop() {
  if (!sockets_ready_)
    return;

  poll_receive_();

  // Heartbeat: advertise our presence to the peer once per second, even when
  // no call is active, so each side can tell whether the other is reachable.
  uint32_t now_ms = millis();
  if (now_ms - last_ping_tx_ms_ >= 1000) {
    last_ping_tx_ms_ = now_ms;
    send_ping_();
  }

  if (in_call_ && jpeg_ready_ && camera_ != nullptr) {
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
}

// ===========================================================================
// Call control
// ===========================================================================
void Face2Face::start_call() {
  if (in_call_)
    return;
  ESP_LOGI(TAG, "Starting call to %s", peer_ip_.c_str());
  if (camera_ != nullptr && !camera_->is_streaming())
    camera_->start_streaming();
  if (audio_enabled_) {
    if (spk_ != nullptr)
      spk_->start();
    if (mic_ != nullptr && !mic_->is_running())
      mic_->start();
  }
  in_call_ = true;
}

void Face2Face::stop_call() {
  if (!in_call_)
    return;
  ESP_LOGI(TAG, "Stopping call");
  in_call_ = false;
  if (audio_enabled_) {
    if (mic_ != nullptr && mic_->is_running())
      mic_->stop();
    if (spk_ != nullptr)
      spk_->stop();
  }
  // Camera is left streaming so the local self-view keeps working; call
  // camera_->stop_streaming() here if you want it off between calls.
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
    return;  // heartbeat only, nothing else to do
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
  // --- Encoder ---
  jpeg_encode_engine_cfg_t enc_eng = {};
  enc_eng.timeout_ms = 70;
  if (jpeg_new_encoder_engine(&enc_eng, reinterpret_cast<jpeg_encoder_handle_t *>(&jpeg_enc_)) != ESP_OK) {
    ESP_LOGE(TAG, "jpeg_new_encoder_engine failed");
    return false;
  }
  // Encoder input: RGB565 frame (DMA-capable, aligned).
  size_t in_size = (size_t) width_ * height_ * 2;
  jpeg_encode_memory_alloc_cfg_t in_cfg = {};
  in_cfg.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
  size_t in_alloc = 0;
  enc_in_ = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(in_size, &in_cfg, &in_alloc));
  // Encoder output: compressed JPEG scratch (worst case ~ raw/2 is plenty).
  jpeg_encode_memory_alloc_cfg_t out_cfg = {};
  out_cfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  enc_out_cap_ = in_size;  // generous
  enc_out_ = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(enc_out_cap_, &out_cfg, &enc_out_cap_));

  // --- Decoder ---
  jpeg_decode_engine_cfg_t dec_eng = {};
  dec_eng.timeout_ms = 40;
  if (jpeg_new_decoder_engine(&dec_eng, reinterpret_cast<jpeg_decoder_handle_t *>(&jpeg_dec_)) != ESP_OK) {
    ESP_LOGE(TAG, "jpeg_new_decoder_engine failed");
    return false;
  }
  // Decoder input: incoming JPEG (DMA-capable). Decoder output: RGB565.
  jpeg_decode_memory_alloc_cfg_t din_cfg = {};
  din_cfg.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
  size_t din_alloc = 0;
  dec_in_cap_ = in_size;  // a JPEG is always smaller than the raw frame
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

void Face2Face::pump_video_tx_() {
  esp_cam_sensor::SimpleBufferElement *el = nullptr;
  uint8_t *rgb = nullptr;
  int w = 0, h = 0;
  if (!camera_->get_current_rgb_frame(&el, &rgb, &w, &h) || rgb == nullptr)
    return;

  // Copy the RGB565 frame into the DMA-capable encoder input buffer.
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

  // Frame must fit the DMA output buffer we allocated (width_/height_ = max).
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

  // Resolution-agnostic: expose whatever size the peer actually sent so the
  // LVGL canvas can size itself via remote_width()/remote_height().
  remote_w_ = info.width;
  remote_h_ = info.height;
  size_t n = out_len < want ? out_len : want;
  if (remote_fb_.size() != want)
    remote_fb_.assign(want, 0);
  std::memcpy(remote_fb_.data(), dec_out_, n);
  return true;
}

// ===========================================================================
// Audio (ESPHome microphone -> UDP -> speaker)
// ===========================================================================
void Face2Face::on_mic_data_(const std::vector<uint8_t> &data) {
  if (!in_call_ || data.empty())
    return;
  send_frame_(F2F_STREAM_AUDIO, data.data(), data.size(), audio_sock_);
}

void Face2Face::play_audio_(const uint8_t *pcm, uint32_t len) {
  if (spk_ == nullptr || len == 0)
    return;
  // Best-effort: drop the chunk if the speaker buffer is momentarily full.
  spk_->play(pcm, len);
}

}  // namespace face2face
}  // namespace esphome
