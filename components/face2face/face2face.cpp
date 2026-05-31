#include "face2face.h"
#include "esphome/core/log.h"

#include <cstring>

// lwIP / POSIX sockets (available under ESP-IDF)
#include <lwip/sockets.h>
#include <lwip/netdb.h>

namespace esphome {
namespace face2face {

static const char *const TAG = "face2face";

// ===========================================================================
// FrameAssembler
// ===========================================================================
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
// Component lifecycle
// ===========================================================================
void Face2Face::setup() {
  ESP_LOGCONFIG(TAG, "Setting up face2face...");
  if (!open_sockets_()) {
    ESP_LOGE(TAG, "Failed to open UDP sockets");
    this->mark_failed();
    return;
  }
  // Pre-allocate the remote framebuffer (RGB565).
  remote_fb_.assign((size_t) width_ * height_ * 2, 0);

  // Capture / decode / audio are brought up lazily on start_call() so the
  // camera and codecs are only powered while a call is active.
  ESP_LOGCONFIG(TAG, "face2face ready (peer=%s video:%u audio:%u)",
                peer_ip_.c_str(), video_port_, audio_port_);
}

void Face2Face::loop() {
  if (!sockets_ready_)
    return;

  // 1) Receive + reassemble + decode incoming media.
  poll_receive_();

  // 2) Capture + encode + send outgoing media, rate-limited to framerate_.
  if (in_call_ && capture_ready_) {
    uint32_t now = micros();
    uint32_t period = 1000000UL / framerate_;
    if (now - last_tx_us_ >= period) {
      last_tx_us_ = now;
      pump_tx_();
    }
  }
}

void Face2Face::dump_config() {
  ESP_LOGCONFIG(TAG, "face2face:");
  ESP_LOGCONFIG(TAG, "  Peer IP: %s", peer_ip_.c_str());
  ESP_LOGCONFIG(TAG, "  Video port: %u", video_port_);
  ESP_LOGCONFIG(TAG, "  Audio port: %u", audio_port_);
  ESP_LOGCONFIG(TAG, "  Resolution: %ux%u @ %u fps", width_, height_, framerate_);
  ESP_LOGCONFIG(TAG, "  JPEG quality: %u", jpeg_quality_);
  ESP_LOGCONFIG(TAG, "  Audio: %s @ %u Hz", YESNO(audio_enabled_), audio_sample_rate_);
}

// ===========================================================================
// Call control
// ===========================================================================
void Face2Face::start_call() {
  if (in_call_)
    return;
  ESP_LOGI(TAG, "Starting call to %s", peer_ip_.c_str());
  if (!capture_ready_)
    capture_ready_ = capture_init_();
  if (!decoder_ready_)
    decoder_ready_ = decoder_init_();
  if (audio_enabled_)
    audio_init_();
  in_call_ = capture_ready_;
  if (!in_call_)
    ESP_LOGE(TAG, "Call could not start: capture pipeline not ready");
}

void Face2Face::stop_call() {
  if (!in_call_)
    return;
  ESP_LOGI(TAG, "Stopping call");
  in_call_ = false;
  capture_deinit_();
  audio_deinit_();
  // Decoder kept resident; cheap to keep, expensive to re-init.
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
    // Non-blocking so loop() never stalls.
    int flags = ::fcntl(sock, F_GETFL, 0);
    ::fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    // Bigger RX buffer: a 640x480 JPEG can be ~30-60 fragments.
    int rxbuf = 65536;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rxbuf, sizeof(rxbuf));

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(i == 0 ? video_port_ : audio_port_);
    if (::bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
      ESP_LOGE(TAG, "bind(%u) failed: errno %d", i == 0 ? video_port_ : audio_port_, errno);
      ::close(sock);
      return false;
    }
    if (i == 0)
      video_sock_ = sock;
    else
      audio_sock_ = sock;
  }
  sockets_ready_ = true;
  return true;
}

void Face2Face::close_sockets_() {
  if (video_sock_ >= 0) {
    ::close(video_sock_);
    video_sock_ = -1;
  }
  if (audio_sock_ >= 0) {
    ::close(audio_sock_);
    audio_sock_ = -1;
  }
  sockets_ready_ = false;
}

// Split a frame into UDP fragments and send them to the peer.
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
    uint16_t plen = (len - off) > F2F_MAX_PAYLOAD ? F2F_MAX_PAYLOAD : (uint16_t)(len - off);
    hdr->flags = (f == frag_count - 1) ? F2F_FLAG_LAST : 0;
    hdr->frag_index = f;
    hdr->payload_len = plen;
    std::memcpy(pkt + F2F_HEADER_SIZE, data + off, plen);
    int sent = ::sendto(sock, pkt, F2F_HEADER_SIZE + plen, 0,
                        (struct sockaddr *) &dst, sizeof(dst));
    if (sent < 0 && errno != EWOULDBLOCK) {
      ESP_LOGW(TAG, "sendto failed: errno %d", errno);
      break;
    }
  }
}

void Face2Face::poll_receive_() {
  uint8_t buf[F2F_HEADER_SIZE + F2F_MAX_PAYLOAD];
  // Drain a bounded number of packets per loop to keep latency low without
  // starving the rest of ESPHome.
  for (int i = 0; i < 64; i++) {
    int n = ::recv(video_sock_, buf, sizeof(buf), 0);
    if (n > 0)
      handle_packet_(buf, n, F2F_STREAM_VIDEO);
    else
      break;
  }
  if (audio_enabled_) {
    for (int i = 0; i < 32; i++) {
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
  if (hdr->magic != F2F_MAGIC || hdr->stream != expected)
    return;
  if (F2F_HEADER_SIZE + hdr->payload_len > len)
    return;
  if (hdr->frame_size == 0 || hdr->frag_index >= hdr->frag_count)
    return;

  FrameAssembler &asmb = (expected == F2F_STREAM_VIDEO) ? video_asm_ : audio_asm_;

  // New frame? (re)initialise the assembler. We accept the newest frame_id and
  // drop late fragments of an older one — fine for real-time MJPEG/audio.
  if (!asmb.active || asmb.frame_id != hdr->frame_id) {
    asmb.reset(hdr->frame_id, hdr->frame_size, hdr->frag_count);
  }
  if (hdr->frame_size != asmb.frame_size || hdr->frag_index >= asmb.got.size())
    return;
  if (asmb.got[hdr->frag_index])
    return;  // duplicate

  uint32_t off = (uint32_t) hdr->frag_index * F2F_MAX_PAYLOAD;
  if (off + hdr->payload_len > asmb.data.size())
    return;
  std::memcpy(asmb.data.data() + off, buf + F2F_HEADER_SIZE, hdr->payload_len);
  asmb.got[hdr->frag_index] = true;
  asmb.frags_seen++;

  if (!asmb.complete())
    return;
  asmb.active = false;  // consume

  if (expected == F2F_STREAM_VIDEO) {
    if (decoder_ready_ && decode_jpeg_(asmb.data.data(), asmb.frame_size))
      new_remote_frame_ = true;
  } else {
    // Audio frames are raw/encoded PCM chunks; hand them to the codec.
    audio_play_(asmb.data.data(), asmb.frame_size);
  }
}

// ===========================================================================
// Capture + encode  (esp_capture / GMF, hardware MJPEG encoder)
// ===========================================================================
// NOTE: the calls below target the Espressif `esp_capture` managed component.
// They are gated behind the real headers so the component still *compiles*
// without them while you wire the pipeline on-device. Replace the stub bodies
// with the concrete esp_capture_* calls once building against ESP-IDF + P4.
bool Face2Face::capture_init_() {
  ESP_LOGI(TAG, "capture_init_(): bring up OV5647 (V4L2) + MJPEG encoder + I2S mic");
  // === Integration point (esp_capture) =====================================
  //  esp_capture_video_v4l2_src_cfg_t vcfg = { .dev_name = "/dev/video0",
  //      .buf_count = 2 };
  //  esp_capture_video_src_if_t *vsrc = esp_capture_new_video_v4l2_src(&vcfg);
  //  esp_capture_audio_aud_dev_src_cfg_t acfg = { ... };  // I2S mic
  //  esp_capture_audio_src_if_t *asrc = esp_capture_new_audio_dev_src(&acfg);
  //  esp_capture_cfg_t cap_cfg = { .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO,
  //      .audio_src = asrc, .video_src = vsrc };
  //  esp_capture_open(&cap_cfg, (esp_capture_handle_t*)&capture_handle_);
  //  esp_capture_sink_cfg_t sink = {
  //      .video_info = { .format_id = ESP_CAPTURE_FMT_ID_MJPEG,
  //                      .width = width_, .height = height_,
  //                      .fps = framerate_ },
  //      .audio_info = { .format_id = ESP_CAPTURE_FMT_ID_G711A,  // or AAC/PCM
  //                      .sample_rate = audio_sample_rate_,
  //                      .channel = 1, .bits_per_sample = 16 } };
  //  esp_capture_sink_setup(capture_handle_, 0, &sink,
  //                         (esp_capture_sink_handle_t*)&video_sink_);
  //  esp_capture_sink_enable(video_sink_, ESP_CAPTURE_RUN_MODE_ALWAYS);
  //  esp_capture_start(capture_handle_);
  // =========================================================================
  ESP_LOGW(TAG, "capture pipeline is a stub — wire esp_capture_* on device");
  return true;  // return true so the rest of the chain can be exercised
}

void Face2Face::capture_deinit_() {
  if (!capture_ready_)
    return;
  // esp_capture_stop(capture_handle_); esp_capture_close(capture_handle_);
  capture_handle_ = nullptr;
  video_sink_ = nullptr;
  audio_sink_ = nullptr;
  capture_ready_ = false;
}

void Face2Face::pump_tx_() {
  // === Integration point (esp_capture) =====================================
  //  esp_capture_stream_frame_t frame = { .stream_type = ESP_CAPTURE_STREAM_TYPE_VIDEO };
  //  if (esp_capture_sink_acquire_frame(video_sink_, &frame, true) == ESP_CAPTURE_ERR_OK) {
  //    send_frame_(F2F_STREAM_VIDEO, frame.data, frame.size, video_sock_);
  //    esp_capture_sink_release_frame(video_sink_, &frame);
  //  }
  //  if (audio_enabled_) {
  //    esp_capture_stream_frame_t af = { .stream_type = ESP_CAPTURE_STREAM_TYPE_AUDIO };
  //    if (esp_capture_sink_acquire_frame(video_sink_, &af, true) == ESP_CAPTURE_ERR_OK) {
  //      send_frame_(F2F_STREAM_AUDIO, af.data, af.size, audio_sock_);
  //      esp_capture_sink_release_frame(video_sink_, &af);
  //    }
  //  }
  // =========================================================================
}

// ===========================================================================
// Decode + render  (hardware JPEG decoder)
// ===========================================================================
bool Face2Face::decoder_init_() {
  ESP_LOGI(TAG, "decoder_init_(): hardware JPEG decoder -> RGB565");
  // === Integration point (esp_video_codec / esp_jpeg) ======================
  //  esp_jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
  //  cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
  //  cfg.rotate = JPEG_ROTATE_0D;
  //  jpeg_dec_open(&cfg, (jpeg_dec_handle_t*)&jpeg_decoder_);
  // =========================================================================
  decoder_ready_ = true;
  return true;
}

void Face2Face::decoder_deinit_() {
  // jpeg_dec_close(jpeg_decoder_);
  jpeg_decoder_ = nullptr;
  decoder_ready_ = false;
}

bool Face2Face::decode_jpeg_(const uint8_t *jpeg, uint32_t len) {
  // === Integration point ===================================================
  //  jpeg_dec_io_t io = { .inbuf = (uint8_t*) jpeg, .inbuf_len = len,
  //      .outbuf = remote_fb_.data(), .outbuf_len = remote_fb_.size() };
  //  jpeg_dec_header_info_t hdr;
  //  if (jpeg_dec_parse_header(jpeg_decoder_, &io, &hdr) != JPEG_ERR_OK) return false;
  //  if (jpeg_dec_process(jpeg_decoder_, &io) != JPEG_ERR_OK) return false;
  //  return true;
  // =========================================================================
  (void) jpeg;
  (void) len;
  return false;  // until the hardware decoder is wired in
}

// ===========================================================================
// Audio  (I2S playback via esp_codec_dev)
// ===========================================================================
bool Face2Face::audio_init_() {
  if (!audio_enabled_)
    return false;
  ESP_LOGI(TAG, "audio_init_(): I2S speaker via esp_codec_dev");
  // esp_codec_dev_open(audio_dev_, &fs);  // playback at audio_sample_rate_
  return true;
}

void Face2Face::audio_deinit_() {
  // esp_codec_dev_close(audio_dev_);
  audio_dev_ = nullptr;
}

void Face2Face::audio_play_(const uint8_t *pcm, uint32_t len) {
  // Decode (if G711/AAC) then esp_codec_dev_write(audio_dev_, pcm, len);
  (void) pcm;
  (void) len;
}

}  // namespace face2face
}  // namespace esphome
