#include "face2face.h"
#include "esphome/core/log.h"
#include "ring_aac.h"

#include <cstring>
#include <cmath>
#include <cstdlib>

// AAC ringtone decoder (Espressif esp_audio_codec; pulled in __init__.py).
#include "esp_aac_dec.h"

// lwIP / POSIX sockets (ESP-IDF)
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_task_wdt.h"  // subscribe the video task so camera WDT-resets don't warn

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
  // Guards the shared HW JPEG peripheral (TX task encodes, main loop decodes).
  jpeg_mutex_ = xSemaphoreCreateMutex();
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

  // Synthesised ringtone while setting up a call (no audio file needed).
  if (ringtone_enabled_ && (state_ == STATE_OUTGOING || state_ == STATE_RINGING))
    pump_ringtone_();

  // Deferred audio start (after the I2S bus is freed by the wake-word).
  // The ESPHome i2s driver retries its own start internally ("retrying in 1s"),
  // but if voice_assistant/mWW re-grabbed the mic we re-issue start a few times.
  if (audio_due_ms_ != 0 && state_ == STATE_STREAMING && (int32_t) (now_ms - audio_due_ms_) >= 0) {
    if (spk_ != nullptr)
      spk_->start();
    if (mic_ != nullptr && !mic_->is_running()) {
      mic_->start();
      mic_started_ = true;
    }
    // Consider audio up once the mic is actually running; otherwise retry in
    // 500ms (bounded) so a slow wake-word handoff doesn't leave audio dead.
    if (mic_ == nullptr || mic_->is_running()) {
      audio_due_ms_ = 0;
      ESP_LOGI(TAG, "Audio started");
    } else if (++audio_retries_ < 10) {
      audio_due_ms_ = now_ms + 500;
    } else {
      audio_due_ms_ = 0;
      ESP_LOGW(TAG, "Audio start gave up (mic busy)");
    }
  }

  // Throughput meter: report the actual C6 link usage (kbit/s) once a second
  // during a call, and keep it for the LVGL debug page.
  if (state_ == STATE_STREAMING && now_ms - last_thru_ms_ >= 1000) {
    uint32_t dt = now_ms - last_thru_ms_;
    if (dt > 0) {
      dbg_tx_kbps_ = (int) ((uint64_t) thru_tx_bytes_ * 8 / dt);  // bytes*8/ms = kbit/s
      dbg_rx_kbps_ = (int) ((uint64_t) thru_rx_bytes_ * 8 / dt);
      ESP_LOGI(TAG, "C6 link: TX=%d kbps  RX=%d kbps", dbg_tx_kbps_, dbg_rx_kbps_);
    }
    thru_tx_bytes_ = 0;
    thru_rx_bytes_ = 0;
    last_thru_ms_ = now_ms;
  }

  // Send our video while streaming, rate-limited to framerate_ (in the main
  // loop, like before: the loop feeds the WDT itself so it can't be starved).
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

// Resolve "host" (already-numeric IP or a DNS/DDNS hostname) to a numeric IPv4
// string. Returns "" on failure so the caller can keep the previous peer.
std::string Face2Face::resolve_host_(const std::string &host) {
  if (host.empty())
    return "";
  // Already a dotted-quad IP? inet_aton accepts it directly, no DNS needed.
  struct in_addr probe {};
  if (::inet_aton(host.c_str(), &probe) != 0)
    return host;
  // Hostname (e.g. DuckDNS) -> resolve via DNS each call so a changed home IP
  // is picked up.
  struct addrinfo hints {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo *res = nullptr;
  if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
    ESP_LOGW(TAG, "DNS resolve failed for '%s'", host.c_str());
    return "";
  }
  char ip[INET_ADDRSTRLEN] = {};
  auto *sa = reinterpret_cast<struct sockaddr_in *>(res->ai_addr);
  ::inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
  ::freeaddrinfo(res);
  ESP_LOGI(TAG, "resolved '%s' -> %s", host.c_str(), ip);
  return std::string(ip);
}

bool Face2Face::call_contact(const std::string &name) {
  for (const auto &c : contacts_) {
    if (c.name != name)
      continue;
    std::string ip = resolve_host_(c.host);
    if (ip.empty()) {
      ESP_LOGW(TAG, "call_contact('%s'): could not resolve host '%s'",
               name.c_str(), c.host.c_str());
      return false;
    }
    set_peer_ip(ip);
    ESP_LOGI(TAG, "call_contact('%s') -> %s (%s)", name.c_str(), c.host.c_str(), ip.c_str());
    call();
    return true;
  }
  ESP_LOGW(TAG, "call_contact: unknown contact '%s'", name.c_str());
  return false;
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
  // Stop the ringtone now that the call connects.
  if (ring_spk_started_) {
    if (spk_ != nullptr)
      spk_->stop();
    ring_spk_started_ = false;
    ring_pcm_pos_ = 0;
  }
  // Allocate the heavy media buffers now (freed again on hangup).
  if (!ensure_media_()) {
    ESP_LOGE(TAG, "Cannot start call: media allocation failed (low memory?)");
    send_ctrl_(CTRL_HANGUP);
    go_idle_();
    return;
  }
  set_state_(STATE_STREAMING);
  // Power the speaker amplifier (PA) for the call, like the media_player does
  // for TTS. Without this the call audio is stuck at codec line level (faint).
  if (amplifier_ != nullptr)
    amplifier_->turn_on();
  // Fire on_streaming first so YAML can stop wake-word / voice_assistant and
  // release the microphone *before* we start capturing it ourselves.
  if (on_streaming_ != nullptr)
    on_streaming_->trigger();
  if (camera_ != nullptr && !camera_->is_streaming()) {
    camera_->start_streaming();
    camera_started_ = true;
  }
  // Reset the throughput meter for this call.
  thru_tx_bytes_ = 0;
  thru_rx_bytes_ = 0;
  last_thru_ms_ = millis();
  // Defer mic+speaker start: micro_wake_word/voice_assistant (stopped in
  // on_streaming) need a moment to release the shared I2S bus, otherwise the
  // speaker fails with "Parent bus is busy". The actual start happens in loop()
  // once audio_start_delay_ms_ has elapsed.
  if (audio_enabled_)
    audio_due_ms_ = millis() + audio_start_delay_ms_;
    audio_retries_ = 0;
  ESP_LOGI(TAG, "Call established (streaming)");
}

void Face2Face::go_idle_() {
  bool was_active = (state_ != STATE_IDLE);
  set_state_(STATE_IDLE);
  audio_due_ms_ = 0;  // cancel any pending deferred audio start
  ring_spk_started_ = false;  // ringtone (if any) is stopped via spk_->stop() below
  ring_pcm_pos_ = 0;
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
  // Turn the speaker amplifier back off now the call is over.
  if (amplifier_ != nullptr)
    amplifier_->turn_off();
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
    int txbuf = 65536;  // bigger TX buffer: a JPEG frame is dozens of fragments
    ::setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &txbuf, sizeof(txbuf));

    // WiFi QoS (WMM): mark audio as Voice (AC_VO) and video as Video (AC_VI) so
    // the radio sends audio before the heavy video stream. On a shared
    // WiFi-over-SDIO link the bursty MJPEG otherwise starves the small audio
    // stream -> choppy/jittery sound. IP_TOS upper bits map to the 802.11e AC.
    int tos = (i == 0) ? 0x80 : 0xC0;  // video=CS4/AC_VI, audio=CS6/AC_VO (higher)
    ::setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

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
  if (sock < 0 || data == nullptr || len == 0)
    return;
  struct sockaddr_in dst {};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(stream == F2F_STREAM_VIDEO ? video_port_ : audio_port_);
  // No peer configured yet (e.g. peer_ip still 0.0.0.0): don't send.
  if (::inet_aton(peer_ip_.c_str(), &dst.sin_addr) == 0 || dst.sin_addr.s_addr == 0)
    return;

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
    // The socket is non-blocking (so recv never stalls). A JPEG frame is dozens
    // of fragments sent back-to-back; when lwIP's TX buffer is momentarily full
    // sendto returns EWOULDBLOCK. Previously the fragment was DROPPED, and a
    // frame missing any fragment is discarded whole by the receiver -> "frame
    // by frame" and total loss at higher resolution. Retry with a short yield
    // (taskYIELD, not vTaskDelay which is 10ms at 100Hz tick) so lwIP drains.
    int tries = 0;
    while (true) {
      int sent = ::sendto(sock, pkt, F2F_HEADER_SIZE + plen, 0, (struct sockaddr *) &dst, sizeof(dst));
      if (sent >= 0) {
        thru_tx_bytes_ += (uint32_t) (F2F_HEADER_SIZE + plen);
        break;
      }
      if (errno == EWOULDBLOCK || errno == EAGAIN || errno == ENOMEM || errno == ENOBUFS) {
        if (++tries > 4000)
          break;  // give up on this fragment rather than stall forever
        taskYIELD();
        continue;
      }
      ESP_LOGW(TAG, "sendto failed: errno %d", errno);
      return;
    }
  }
}

void Face2Face::poll_receive_() {
  uint8_t buf[F2F_HEADER_SIZE + F2F_MAX_PAYLOAD];
  for (int i = 0; i < 64; i++) {
    int n = ::recv(video_sock_, buf, sizeof(buf), 0);
    if (n > 0) {
      thru_rx_bytes_ += (uint32_t) n;
      handle_packet_(buf, n, F2F_STREAM_VIDEO);
    } else
      break;
  }
  if (audio_enabled_) {
    for (int i = 0; i < 48; i++) {
      int n = ::recv(audio_sock_, buf, sizeof(buf), 0);
      if (n > 0) {
        thru_rx_bytes_ += (uint32_t) n;
        handle_packet_(buf, n, F2F_STREAM_AUDIO);
      } else
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
  jpeg_decode_engine_cfg_t dec_eng = {};
  dec_eng.timeout_ms = 40;
  if (jpeg_new_decoder_engine(&dec_eng, reinterpret_cast<jpeg_decoder_handle_t *>(&jpeg_dec_)) != ESP_OK) {
    ESP_LOGE(TAG, "jpeg_new_decoder_engine failed");
    return false;
  }
  // DMA buffers (enc_in_/enc_out_/dec_in_/dec_out_) are grown lazily to the
  // ACTUAL frame size in pump_video_tx_/decode_jpeg_, so any camera resolution
  // (e.g. 1280x720) works without matching width_/height_ in YAML.
  jpeg_ready_ = true;
  ESP_LOGCONFIG(TAG, "Hardware JPEG codec ready");
  return true;
}

// Ensure a JPEG-encoder DMA buffer is at least `need` bytes (realloc if needed).
static bool ensure_enc_buf(uint8_t **buf, size_t *cap, size_t need, bool input) {
  if (*buf != nullptr && *cap >= need)
    return true;
  if (*buf != nullptr) {
    free(*buf);
    *buf = nullptr;
    *cap = 0;
  }
  jpeg_encode_memory_alloc_cfg_t cfg = {};
  cfg.buffer_direction = input ? JPEG_ENC_ALLOC_INPUT_BUFFER : JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  size_t got = 0;
  *buf = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(need, &cfg, &got));
  *cap = (*buf != nullptr) ? (got ? got : need) : 0;
  return *buf != nullptr;
}

// Ensure a JPEG-decoder DMA buffer is at least `need` bytes (realloc if needed).
static bool ensure_dec_buf(uint8_t **buf, size_t *cap, size_t need, bool input) {
  if (*buf != nullptr && *cap >= need)
    return true;
  if (*buf != nullptr) {
    free(*buf);
    *buf = nullptr;
    *cap = 0;
  }
  jpeg_decode_memory_alloc_cfg_t cfg = {};
  cfg.buffer_direction = input ? JPEG_DEC_ALLOC_INPUT_BUFFER : JPEG_DEC_ALLOC_OUTPUT_BUFFER;
  size_t got = 0;
  *buf = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(need, &cfg, &got));
  *cap = (*buf != nullptr) ? (got ? got : need) : 0;
  return *buf != nullptr;
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
  enc_in_cap_ = 0;
  enc_out_cap_ = 0;
  dec_in_cap_ = 0;
  dec_out_cap_ = 0;
  jpeg_ready_ = false;
}

// Allocate the per-call media buffers (idempotent).
bool Face2Face::ensure_media_() {
  if (jpeg_ready_)
    return true;
  if (!jpeg_init_())
    return false;
  remote_fb_.assign((size_t) width_ * height_ * 2, 0);
  // AEC is allocated once for the component lifetime (never freed on hangup, to
  // avoid racing the mic task). Only init it the first time.
  if (audio_enabled_ && aec_enabled_ && !aec_ready_)
    aec_init_();  // best-effort; passes through raw audio if it fails
  return true;
}

// Free everything allocated by ensure_media_ so idle RAM/PSRAM use is minimal.
void Face2Face::release_media_() {
  jpeg_deinit_();
  // NOTE: we deliberately do NOT free the AEC here. on_mic_data_ runs in the
  // microphone's own FreeRTOS task; freeing aec_in_/aec_handle_ from the main
  // loop on hangup races with it -> use-after-free / Guru Meditation. The AEC
  // buffers are tiny (~16KB) so we keep them for the component lifetime.
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
  // Pump the V4L2 capture pipeline ourselves. lvgl_camera_display normally does
  // this via capture_frame(), but it is disabled during a call, so face2face
  // must dequeue the next frame or current_buffer_index_ stays -1 forever
  // ("get_current_rgb_frame: no buffer available" flood, no video).
  if (!camera_->capture_frame())
    return;
  if (!camera_->get_current_rgb_frame(&el, &rgb, &w, &h) || rgb == nullptr)
    return;

  // Downscale by an integer factor (scale_) while copying into the encoder
  // input buffer. A 1280x720 frame at scale 3 becomes 426x240 -> the JPEG is a
  // few KB (a handful of UDP fragments) instead of ~100KB (dozens), which is
  // what the WiFi-over-SDIO link can actually sustain.
  int s = scale_ < 1 ? 1 : scale_;
  int ow = w / s, oh = h / s;
  // The hardware JPEG codec aligns dimensions to 16px. Round DOWN to a multiple
  // of 16 on both sides so the encoder output and the peer's decoder output have
  // the exact size we allocate (else decode fails: "buffer smaller than actual
  // output size"). 426x240 -> 416x240, 400x400 -> 400x400.
  ow &= ~15;
  oh &= ~15;
  if (ow < 16 || oh < 16) { camera_->release_buffer(el); return; }
  size_t out_bytes = (size_t) ow * oh * 2;
  bool have_input = ensure_enc_buf(&enc_in_, &enc_in_cap_, out_bytes, true) &&
                    ensure_enc_buf(&enc_out_, &enc_out_cap_, out_bytes, false);
  if (have_input) {
    const uint16_t *src = reinterpret_cast<const uint16_t *>(rgb);
    uint16_t *dst = reinterpret_cast<uint16_t *>(enc_in_);
    if (s == 1) {
      std::memcpy(dst, src, out_bytes);
    } else {
      for (int y = 0; y < oh; y++) {
        const uint16_t *srow = src + (size_t) (y * s) * w;
        uint16_t *drow = dst + (size_t) y * ow;
        for (int x = 0; x < ow; x++)
          drow[x] = srow[x * s];
      }
    }
  }
  // Release the camera buffer NOW, before the (slow) JPEG encode + network send.
  // The camera only has 2 buffers; holding one during encode/send starves the
  // sensor -> "get_current_rgb_frame: no buffer available" flood and choppy fps.
  camera_->release_buffer(el);

  if (!have_input) {
    // Throttle: this would otherwise log at the frame rate.
    uint32_t now = millis();
    if (now - last_enc_warn_ms_ > 1000) {
      last_enc_warn_ms_ = now;
      ESP_LOGW(TAG, "enc buffer alloc failed for %dx%d", ow, oh);
    }
    return;
  }
  jpeg_encode_cfg_t cfg = {};
  cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
  cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
  cfg.image_quality = jpeg_quality_;
  cfg.width = ow;
  cfg.height = oh;
  uint32_t out_size = 0;
  // Serialise with the main loop's decode (shared HW JPEG peripheral).
  if (jpeg_mutex_ != nullptr)
    xSemaphoreTake(jpeg_mutex_, portMAX_DELAY);
  esp_err_t err = jpeg_encoder_process(reinterpret_cast<jpeg_encoder_handle_t>(jpeg_enc_), &cfg, enc_in_,
                                       out_bytes, enc_out_, enc_out_cap_, &out_size);
  if (jpeg_mutex_ != nullptr)
    xSemaphoreGive(jpeg_mutex_);
  if (err == ESP_OK && out_size > 0) {
    send_frame_(F2F_STREAM_VIDEO, enc_out_, out_size, video_sock_);
  } else {
    uint32_t now = millis();
    if (now - last_enc_warn_ms_ > 1000) {
      last_enc_warn_ms_ = now;
      ESP_LOGW(TAG, "jpeg encode failed: %d", err);
    }
  }
}

bool Face2Face::decode_jpeg_(const uint8_t *jpeg, uint32_t len) {
  if (!jpeg_ready_)
    return false;
  // Grow the decoder input buffer to the incoming JPEG size.
  if (!ensure_dec_buf(&dec_in_, &dec_in_cap_, len, true))
    return false;
  std::memcpy(dec_in_, jpeg, len);

  jpeg_decode_picture_info_t info = {};
  if (jpeg_decoder_get_info(dec_in_, len, &info) != ESP_OK)
    return false;

  // The HW decoder rounds width/height UP to a multiple of 16, so its output can
  // be larger than info.width*info.height. Allocate for the aligned size.
  uint32_t aw = (info.width + 15) & ~15u;
  uint32_t ah = (info.height + 15) & ~15u;
  size_t want = (size_t) aw * ah * 2;
  if (want == 0 || !ensure_dec_buf(&dec_out_, &dec_out_cap_, want, false))
    return false;

  jpeg_decode_cfg_t cfg = {};
  cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;

  uint32_t out_len = 0;
  // Serialise with the video-TX task's encode (shared HW JPEG peripheral).
  if (jpeg_mutex_ != nullptr)
    xSemaphoreTake(jpeg_mutex_, portMAX_DELAY);
  esp_err_t derr = jpeg_decoder_process(reinterpret_cast<jpeg_decoder_handle_t>(jpeg_dec_), &cfg, dec_in_, len,
                                        dec_out_, dec_out_cap_, &out_len);
  if (jpeg_mutex_ != nullptr)
    xSemaphoreGive(jpeg_mutex_);
  if (derr != ESP_OK)
    return false;

  remote_w_ = info.width;
  remote_h_ = info.height;
  size_t n = out_len < want ? out_len : want;
  if (remote_fb_.size() != want)
    remote_fb_.assign(want, 0);
  if (swap_colors_) {
    // The hardware JPEG decoder emits RGB565 with a byte order that LVGL reads
    // swapped (psychedelic magenta/cyan). Swap the two bytes of each pixel so
    // the canvas shows correct colors without touching the LVGL byte_order.
    const uint16_t *src = reinterpret_cast<const uint16_t *>(dec_out_);
    uint16_t *dst = reinterpret_cast<uint16_t *>(remote_fb_.data());
    size_t px = n / 2;
    for (size_t i = 0; i < px; i++)
      dst[i] = (uint16_t) ((src[i] >> 8) | (src[i] << 8));
  } else {
    std::memcpy(remote_fb_.data(), dec_out_, n);
  }
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
  // Diagnostics: level + frame count of what WE send (your voice).
  {
    const int16_t *s = reinterpret_cast<const int16_t *>(data.data());
    size_t n = data.size() / 2;
    int32_t p = 0;
    for (size_t i = 0; i < n; i++) {
      int32_t a = s[i] < 0 ? -s[i] : s[i];
      if (a > p)
        p = a;
    }
    dbg_mic_peak_ = p;
    dbg_tx_audio_++;
  }

#ifdef FACE2FACE_USE_AEC
  // AEC gating (as recommended by Espressif/esp-sr): only run echo cancellation
  // when the speaker actually played within the last 250ms. When the far end is
  // silent there is no echo to cancel, and running the adaptive filter on pure
  // near-end speech makes it drift and can hurt the wake word. So bypass AEC
  // and send the raw mic when the speaker has been idle.
  bool spk_recent = (last_spk_ms_ != 0) && (millis() - last_spk_ms_ < 250);
  if (aec_ready_ && spk_recent) {
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
  // Speaker idle: keep the mic accumulator/reference from going stale.
  if (!mic_acc_.empty())
    mic_acc_.clear();
#endif
  send_frame_(F2F_STREAM_AUDIO, data.data(), data.size(), audio_sock_);
}

// Decode the embedded ring.aac (AAC-LC) into a cached PCM buffer, once. The
// decoder reports the native rate/channels; we keep mono int16 and let the
// speaker resample. ADTS-framed, fed frame by frame.
bool Face2Face::decode_ringtone_() {
  if (ring_decoded_)
    return !ring_pcm_.empty();
  ring_decoded_ = true;  // attempt only once, success or not

  void *dec = nullptr;
  if (esp_aac_dec_open(nullptr, 0, &dec) != ESP_AUDIO_ERR_OK || dec == nullptr) {
    ESP_LOGW(TAG, "ringtone: AAC decoder open failed, using synth beep");
    return false;
  }

  std::vector<uint8_t> out;  // PCM bytes accumulated across frames
  out.reserve(128 * 1024);
  std::vector<uint8_t> frame(8192);  // per-call PCM output scratch

  esp_audio_dec_in_raw_t raw = {};
  raw.buffer = const_cast<uint8_t *>(RING_AAC);
  raw.len = RING_AAC_LEN;
  esp_audio_dec_info_t info = {};
  bool ok = true;

  while (raw.len > 0) {
    esp_audio_dec_out_frame_t of = {};
    of.buffer = frame.data();
    of.len = frame.size();
    esp_audio_err_t r = esp_aac_dec_decode(dec, &raw, &of, &info);
    if (r == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
      frame.resize(of.needed_size);
      continue;  // retry this frame with a bigger buffer
    }
    if (r != ESP_AUDIO_ERR_OK) {
      ESP_LOGW(TAG, "ringtone: AAC decode error %d at %u left", (int) r, raw.len);
      ok = false;
      break;
    }
    if (of.decoded_size > 0)
      out.insert(out.end(), of.buffer, of.buffer + of.decoded_size);
    if (raw.consumed == 0)
      break;  // no progress -> avoid an infinite loop
    raw.buffer += raw.consumed;
    raw.len -= raw.consumed;
  }
  esp_aac_dec_close(dec);

  if (!ok || out.empty() || info.sample_rate == 0) {
    ESP_LOGW(TAG, "ringtone: decode produced no PCM, using synth beep");
    ring_pcm_.clear();
    return false;
  }

  // Source mono samples at the decoder's native rate. For a stereo clip we
  // down-mix in place into the front of `out`; for mono we read it directly.
  int16_t *s = reinterpret_cast<int16_t *>(out.data());
  size_t total = out.size() / sizeof(int16_t);
  uint8_t ch = info.channel ? info.channel : 1;
  size_t src_n = total;
  if (ch >= 2) {
    src_n = total / ch;
    for (size_t i = 0, o = 0; o < src_n; o++, i += ch) {
      int32_t acc = 0;
      for (uint8_t c = 0; c < ch; c++)
        acc += s[i + c];
      s[o] = (int16_t) (acc / ch);
    }
  }

  // Resample (linear) to audio_sample_rate_ so it matches the rest of the audio
  // path and keeps the cached buffer small (44.1 kHz -> 16 kHz ~ 106 KB -> 39 KB).
  uint32_t src_rate = info.sample_rate;
  uint32_t dst_rate = audio_sample_rate_;
  if (src_rate == dst_rate || src_n < 2) {
    ring_pcm_.assign(s, s + src_n);
  } else {
    size_t dst_n = (size_t) ((uint64_t) src_n * dst_rate / src_rate);
    ring_pcm_.resize(dst_n);
    for (size_t i = 0; i < dst_n; i++) {
      double srcpos = (double) i * src_rate / dst_rate;
      size_t i0 = (size_t) srcpos;
      double frac = srcpos - i0;
      int16_t a = s[i0];
      int16_t b = (i0 + 1 < src_n) ? s[i0 + 1] : a;
      ring_pcm_[i] = (int16_t) (a + (b - a) * frac);
    }
  }
  ring_pcm_rate_ = dst_rate;
  ESP_LOGI(TAG, "ringtone: decoded ring.aac -> %u samples @ %u Hz mono (%u KB)",
           (unsigned) ring_pcm_.size(), (unsigned) ring_pcm_rate_,
           (unsigned) (ring_pcm_.size() * sizeof(int16_t) / 1024));
  return true;
}

void Face2Face::pump_ringtone_() {
  if (spk_ == nullptr)
    return;

  // Lazy one-shot decode of ring.aac. If it fails we fall back to the beep.
  bool use_pcm = decode_ringtone_() && !ring_pcm_.empty();
  const uint32_t sr = use_pcm ? ring_pcm_rate_ : audio_sample_rate_;

  // Start the speaker once for the duration of the ringing phase.
  if (!ring_spk_started_) {
    spk_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, sr));
    spk_->start();
    ring_spk_started_ = true;
    ring_phase_ = 0;
    ring_pcm_pos_ = 0;
    last_ring_ms_ = 0;
  }
  // Feed ~40 ms chunks, paced so we don't overflow the speaker buffer.
  uint32_t now = millis();
  if (now - last_ring_ms_ < 30)
    return;
  last_ring_ms_ = now;

  const size_t samples = sr / 25;  // 40 ms
  static thread_local std::vector<int16_t> buf;
  buf.resize(samples);

  if (use_pcm) {
    // Stream the decoded ring.aac, looping back to the start when it ends so it
    // keeps ringing until the call is answered or times out.
    for (size_t i = 0; i < samples; i++) {
      if (ring_pcm_pos_ >= ring_pcm_.size())
        ring_pcm_pos_ = 0;
      buf[i] = ring_pcm_[ring_pcm_pos_++];
    }
  } else {
    // Fallback synthesised ring: caller 425 Hz, callee 1000 Hz, 1 s on / gap.
    uint32_t cycle = (now % 4000);
    bool tone_on;
    double freq;
    if (state_ == STATE_RINGING) {
      tone_on = (cycle < 400) || (cycle >= 600 && cycle < 1000);
      freq = 1000.0;
    } else {  // STATE_OUTGOING
      tone_on = (cycle < 1000);
      freq = 425.0;
    }
    for (size_t i = 0; i < samples; i++) {
      int16_t s = 0;
      if (tone_on) {
        double t = (double) ring_phase_ / (double) sr;
        s = (int16_t) (6000.0 * std::sin(2.0 * M_PI * freq * t));
      }
      buf[i] = s;
      ring_phase_++;
    }
  }
  spk_->play(reinterpret_cast<const uint8_t *>(buf.data()), samples * sizeof(int16_t));
}

void Face2Face::play_audio_(const uint8_t *pcm, uint32_t len) {
  if (spk_ == nullptr || len == 0 || state_ != STATE_STREAMING)
    return;
  // Diagnostics: level + frame count of what we PLAY (audio received from peer).
  {
    const int16_t *s = reinterpret_cast<const int16_t *>(pcm);
    size_t n = len / 2;
    int32_t p = 0;
    for (size_t i = 0; i < n; i++) {
      int32_t a = s[i] < 0 ? -s[i] : s[i];
      if (a > p)
        p = a;
    }
    dbg_spk_peak_ = p;
    dbg_rx_audio_++;
  }
  // Only feed the speaker once its I2S driver is actually RUNNING. On a shared
  // I2S bus the speaker can fail to start ("Parent bus is busy"); pushing PCM
  // into a non-started speaker pipeline corrupts its buffers -> crash on hangup.
  if (!spk_->is_running())
    return;
#ifdef FACE2FACE_USE_AEC
  // The far-end audio we are about to play is the echo reference for the mic.
  if (aec_ready_)
    ref_push_(reinterpret_cast<const int16_t *>(pcm), len / 2);
  last_spk_ms_ = millis();  // mark speaker activity for AEC gating
#endif
  spk_->play(pcm, len);
}

}  // namespace face2face
}  // namespace esphome






