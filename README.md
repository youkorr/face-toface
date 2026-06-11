# face-toface

**FaceTime-like, peer-to-peer audio + video calls between ESP32-P4 boards**, as
native [ESPHome](https://esphome.io) external components.

The project ships three independent components that work together (or on their own):

| Component | What it does | Network |
|-----------|--------------|---------|
| **`face2face`** | Direct-IP (UDP) video + audio call between two P4 boards. Hardware-JPEG video, raw-PCM audio, built-in call signaling, presence, ringtone. | Same LAN |
| **`fdaudio`** | Full-duplex I2S audio engine (mic **and** speaker on one I2S port) exposing standard ESPHome `microphone` + `speaker` platforms. Drives ES8311/ES8388 + ES7210 over `esp_codec_dev`. | — |
| **`webrtc_call`** | Real WebRTC call (ICE/STUN/TURN, apprtc signaling) so any P4 can call any other P4 **across different networks** — true "FaceTime anywhere". | Internet |

Everything is built on **Espressif APIs only** (esp_codec_dev, esp-sr, the P4
hardware JPEG codec, esp-webrtc-solution). No paid services, no cloud lock-in.

> ⚠️ **Status: components are feature-complete, validate on hardware.** The author
> cannot compile P4 firmware in this environment. Expect 1–2 iterations on the
> board, mostly for IDF enum/field names that drift slightly between ESP-IDF
> versions. Each component degrades gracefully and logs what it is doing.

---

## Table of contents

1. [Hardware](#1-hardware)
2. [`face2face` — the LAN video call](#2-face2face--the-lan-video-call)
3. [`fdaudio` — full-duplex audio (the duplex)](#3-fdaudio--full-duplex-audio-the-duplex)
4. [AEC & AFE — echo cancellation (optional, resource-heavy)](#4-aec--afe--echo-cancellation-optional-resource-heavy)
5. [Voice assistant integration](#5-voice-assistant-integration)
6. [`webrtc_call` — cross-network calls (signaling + coturn + app)](#6-webrtc_call--cross-network-calls-signaling--coturn--app)
7. [Examples](#7-examples)
8. [Build notes & troubleshooting](#8-build-notes--troubleshooting)
9. [Sources](#9-sources)

---

## 1. Hardware

Designed around the common Waveshare / M5Stack Tab5 ESP32-P4 stack, but the pins
are all configurable:

- **Camera**: OV5647 (or any sensor) via the `esp_cam_sensor` MIPI-CSI component
  (+ the `esp_video` pipeline), or a USB UVC camera. These live in a **separate
  repo** — see [Camera setup](#camera-setup-external-repo-esp_video--esp_cam_sensor).
- **Audio codec**: ES8311 **or** ES8388 for playback, ES7210 for the mic, on a
  single I2S port (full-duplex) over I2C control.
- **Display**: MIPI-DSI + LVGL 9.x (canvas widget for the remote video).
- **Video codec**: the P4's **hardware JPEG engine** (`esp_driver_jpeg`).
- **WiFi**: ESP32-C6 over SDIO (esp_hosted) on most P4 boards.

> The P4 has **one** hardware JPEG engine shared by encode **and** decode. For a
> bidirectional call the two directions serialize on it — this, not the WiFi
> link, is the practical frame-rate ceiling at full resolution. Use `scale:` to
> trade resolution for frame rate (see below).

---

## 2. `face2face` — the LAN video call

A self-contained, FaceTime-like P2P call between **two** ESP32-P4 boards on the
**same LAN**. No external intercom dependency, no server.

```
  TX (each board sends)                         RX (each board receives)
  ┌───────────────────────────┐                 ┌───────────────────────────┐
  │ camera RGB565             │                  │  UDP :9000 (video)        │
  │   → HW JPEG encode  ──────┼─ UDP :9000 ─►    │   reassemble → HW JPEG    │
  │   → sendto(peer)          │                  │   decode → RGB565         │
  │                           │                  │   → LVGL canvas           │
  │ microphone PCM 16k ───────┼─ UDP :9001 ─►    │  speaker.play(peer PCM)   │
  └───────────────────────────┘                 └───────────────────────────┘
```

- **Video**: RGB565 → hardware JPEG → UDP. MJPEG is stateless between frames, so
  a lost frame is simply skipped (low latency, loss-tolerant).
- **Audio**: raw 16-bit / mono PCM over UDP (~32 KB/s at 16 kHz).
- **Call signaling**: **native**, built in. UDP control messages
  (`INVITE/RING/ANSWER/HANGUP/DECLINE`) with an
  `IDLE → OUTGOING/RINGING → STREAMING` state machine.
- **Presence**: a 1 Hz UDP heartbeat tells each board whether the peer is
  reachable (`id(f2f).peer_online()`), even outside a call.

### Why a custom component (not `camera_web_server`)

`camera_web_server` (MJPEG over HTTP/TCP) does not hold up for real-time. Sending
JPEG over **UDP** directly to the peer means no server, no TCP head-of-line
blocking, minimal latency, and dropped frames instead of stalls.

### Camera setup (external repo: `esp_video` + `esp_cam_sensor`)

`face2face` does **not** drive the camera itself — it consumes RGB565 frames from
the **`esp_cam_sensor`** camera component, which lives in a **separate repository**
together with the `esp_video` pipeline (CSI/ISP/JPEG) and `lvgl_camera_display`:

> 📷 **<https://github.com/youkorr/test2_esp_video_esphome>**
> components: `esp_video`, `esp_cam_sensor`, `lvgl_camera_display`

Pull it via `external_components` (alongside this repo's `face2face`), then
configure the video pipeline + the sensor, and hand the sensor's id to
`face2face` as `camera_id:`.

```yaml
external_components:
  # Camera + video pipeline (the OTHER repo)
  - source:
      type: git
      url: https://github.com/youkorr/test2_esp_video_esphome
      ref: main 
    components: [esp_video, esp_cam_sensor, lvgl_camera_display]
    refresh: always
  # This repo — only the LAN components here (NOT webrtc_call, see note below)
  - source:
      type: git
      url: https://github.com/youkorr/face-toface
      ref: main
    components: [face2face, fdaudio]
    refresh: always

# Video pipeline: CSI input, ISP, hardware JPEG, external clock to the sensor
esp_video:
  i2c_id: bsp_bus
  xclk_pin: GPIO36
  xclk_freq: 24000000
  enable_jpeg: true
  enable_isp: true

# The actual sensor — this id is what face2face's camera_id points at
esp_cam_sensor:
  id: tab5_cam
  i2c_id: bsp_bus
  sensor_type: ov5647          # your sensor (ov5647, ov02c10, sc202cs, ...)
  resolution: "640x480"
  framerate: 30
  jpeg_quality: 15

face2face:
  id: f2f
  camera_id: tab5_cam          # <-- the esp_cam_sensor id above
  width: 640                   # MUST match the sensor RGB output
  height: 480
  # ...
```

Notes:
- The `esp_cam_sensor` **`resolution`** and the `face2face` **`width`/`height`**
  must agree (or add a resize) — see [§8](#8-build-notes--troubleshooting).
- **USB UVC** cameras are also supported by `esp_video` (`/dev/videoN`); point
  `camera_id` at the corresponding sensor. Plain MIPI-CSI (OV5647) is the tested
  path.
- For the `webrtc_call` firmware the camera is owned by GMF instead and described
  through `codec_board` / `board_config` — see [§6](#6-webrtc_call--cross-network-calls-signaling--coturn--app).

> **Why isn't `webrtc_call` in the `external_components` above?** On purpose.
> `webrtc_call` is a **separate, dedicated firmware**: it owns the camera/I2S/LCD
> via GMF and conflicts with `face2face` / `fdaudio` / `esp_video`, so it is never
> combined with them in one config. A webrtc_call build uses its own block —
> `components: [webrtc_call]`, **without** the camera repo — shown in
> [§6](#6-webrtc_call--cross-network-calls-signaling--coturn--app).

### Actions, triggers, presence

```yaml
# Actions (wire to HA buttons / LVGL taps)
on_press:
  - face2face.call: f2f        # place a call to the peer
  - face2face.answer: f2f      # accept an incoming call
  - face2face.hangup: f2f      # hang up / cancel
  - face2face.decline: f2f     # reject an incoming call
```

```yaml
# Triggers (declared inside the face2face: block)
face2face:
  on_ringing:        # peer is calling us
  on_outgoing_call:  # we are calling
  on_streaming:      # call connected
  on_idle:           # call ended / declined / timed out
```

```cpp
// Presence, usable in lambdas / template sensors
id(f2f).peer_online()         // true if a packet was seen from the peer < 4 s ago
id(f2f).peer_last_seen_ms()   // millis() of the last packet from the peer
```

### Ringtone

`face2face` plays the embedded **`ring.aac`** (AAC) as the ringtone. It is
decoded **once** at runtime by Espressif's `esp_audio_codec` (standalone AAC
decoder), resampled to the audio rate, cached as PCM, and looped on the speaker
while ringing. If AAC decoding fails it falls back to a synthesized beep.

```yaml
face2face:
  ringtone: true     # default; embedded ring.aac
  # ringtone: false  # silent (use your own sound via on_ringing + media_player)
```

See [`example/ringtone-options.yaml`](example/ringtone-options.yaml) for using
your own file through a `media_player` instead.

### Configuration

```yaml
face2face:
  id: f2f
  # peer_ip: "192.168.1.51"        # the OTHER board's IP (optional; see below)
  camera_id: tab5_cam              # esp_cam_sensor MIPI-DSI camera
  microphone_id: esp32_microphone  # any ESPHome microphone (e.g. fdaudio)
  speaker_id: esp32_speaker        # any ESPHome speaker (e.g. fdaudio)
  amplifier: pa_enable             # optional PA enable switch, on during a call
  width: 640
  height: 480
  framerate: 15
  jpeg_quality: 40                 # 10..100 (85 is a sane practical max)
  scale: 3                         # downscale factor before JPEG (1..8)
  enable_audio: true
  audio_sample_rate: 16000
  ring_timeout: 30s
  auto_answer: false               # true = intercom mode
  ringtone: true
  # --- AEC (see §4) ---
  enable_aec: true
  aec_mode: sr_low_cost
  aec_filter_length: 4
  audio_start_delay: 1500ms        # wait for wake-word to release the I2S bus
```

| Option | Default | Notes |
|--------|---------|-------|
| `peer_ip` | `0.0.0.0` | IP of the **other** board. Best set at runtime from HA (see below). |
| `camera_id` | — (required) | `esp_cam_sensor` camera providing RGB565 frames. |
| `microphone_id` / `speaker_id` | — | Any ESPHome mic/speaker; pair with `fdaudio` for full-duplex. |
| `amplifier` | — | `switch` toggled on for the call duration. |
| `video_port` / `audio_port` | 9000 / 9001 | UDP ports. |
| `width` / `height` | 640 / 480 | Must match the camera RGB output. |
| `framerate` | 15 | 1..60. |
| `jpeg_quality` | 40 | 10..100. |
| `scale` | 3 | Downscale before encode; the main FPS/latency lever. |
| `audio_sample_rate` | 16000 | Raw PCM rate over UDP. |
| `ring_timeout` | 30s | Auto-hangup if unanswered. |
| `auto_answer` | false | Intercom-style auto-pickup. |
| `ringtone` | true | Embedded `ring.aac`. |

#### Setting `peer_ip` from Home Assistant (recommended)

Give each board a static IP, then set the **other** board's IP at runtime so you
never recompile to change it:

```yaml
text:
  - platform: template
    name: "Peer IP"
    id: peer_ip_input
    mode: text
    optimistic: true
    restore_value: true            # persists across reboots
    pattern: '^(\d{1,3}\.){3}\d{1,3}$'
    on_value:
      - lambda: "id(f2f).set_peer_ip(x);"
```

#### Displaying the remote video (LVGL)

A `canvas` widget plus an `interval` that calls `lv_canvas_set_buffer(...)` with
`id(f2f).remote_rgb565()`. See
[`example/lvgl-call-page.yaml`](example/lvgl-call-page.yaml) for a complete
1024×600 call page with a presence dot.

---

## 3. `fdaudio` — full-duplex audio (the duplex)

`fdaudio` runs **one** I2S port in TX **and** RX at the same time, so the mic and
speaker are live simultaneously — that is the "duplex" you need for a real call.
It drives ES8311/ES8388 (playback) + ES7210 (mic) through `esp_codec_dev`, and
exposes **standard ESPHome platforms**:

- a `microphone` platform → usable by `voice_assistant`, `face2face`, etc.
- a `speaker` platform → usable by `media_player`, TTS, `face2face`, etc.

So nothing downstream needs to know about `fdaudio`; it is a drop-in mic+speaker.

```yaml
# The engine (one per board)
fdaudio:
  id: audio_engine
  bclk_pin: 12
  lrclk_pin: 10
  din_pin: 11          # mic data in (ES7210)
  dout_pin: 9          # speaker data out (ES8311/ES8388)
  mclk_pin: 13         # optional (use_mclk)
  i2c_port: 0
  output_codec: es8311 # es8311 | es8388
  output_address: 0x18
  mic_address: 0x40
  output_volume: 70
  sample_rate: 16000        # rate exposed to ESPHome
  codec_sample_rate: 48000  # actual I2S clock (P4 + ES7210 want 48k; mic decimated)
  mic_gain_db: 37.5         # ES7210 analog PGA
  mic_channels: 1           # ES7210 channel bitmask: MIC1=1 MIC2=2 MIC3=4 MIC4=8
  mic_agc: 10000            # auto-boost weak mic to this peak (0 = off)
  mic_digital_gain: 1.0     # fixed software boost (alternative to AGC)
  noise_gate: 0             # attenuate ambient noise below this amplitude (0 = off)
  echo_suppression: 0       # % mic ducking while speaker plays (call echo; 0 = off)
  enable_aec: true          # simple esp-sr AEC (see §4)
  use_afe: false            # full esp-sr AFE — heavy (see §4)

# The standard ESPHome platforms backed by it
microphone:
  - platform: fdaudio
    id: esp32_microphone
    fdaudio_id: audio_engine

speaker:
  - platform: fdaudio
    id: esp32_speaker
    fdaudio_id: audio_engine
```

### Key design points

- **48 kHz codec, 16 kHz mic.** Many P4 boards only clock the ES7210 correctly at
  48 kHz, so the engine runs the codec at `codec_sample_rate` (48 kHz) and
  **decimates** the mic down to `sample_rate` (16 kHz) for wake-word/STT. The
  speaker therefore plays at 48 kHz; the `fdaudio` speaker upsamples your stream
  (e.g. 16 kHz call audio → 48 kHz) and applies `media_player` volume/mute in
  software.
- **`mic_agc`** is the usual fix for *faint call / weak wake-word* audio — it
  auto-boosts a quiet mic to a target peak. Use it instead of hand-tuning
  `mic_digital_gain`.
- **`echo_suppression`** is a cheap, per-call mic-ducking knob (0 by default so
  it never breaks `voice_assistant` barge-in). `face2face` can raise it at
  runtime for the call only: `id(audio_engine).set_echo_suppression(85);`.

| Option | Default | Notes |
|--------|---------|-------|
| `bclk_pin` / `lrclk_pin` / `din_pin` / `dout_pin` | — (required) | I2S pins. |
| `mclk_pin` / `use_mclk` | -1 / true | Master clock (some codecs need it). |
| `i2c_port` | 0 | I2C bus for codec control. |
| `output_codec` | es8311 | `es8311` or `es8388`. |
| `output_address` / `mic_address` | 0x18 / 0x40 | I2C addresses. |
| `output_volume` | 70 | 0..100. |
| `sample_rate` | 16000 | Rate exposed to ESPHome. |
| `codec_sample_rate` | 48000 | Real I2S clock; integer multiple of `sample_rate`. |
| `mic_gain_db` | 37.5 | ES7210 analog gain (0..42 dB). |
| `mic_channels` | 1 | ES7210 input bitmask. |
| `mic_agc` | 0 | Target peak for auto-gain (~10000 for calls). |
| `mic_digital_gain` | 1.0 | Fixed software boost (1..16). |
| `noise_gate` | 0 | Ambient-noise gate threshold (~250–500). |
| `echo_suppression` | 0 | % mic ducking while playing (~80–90 kills call echo). |
| `enable_aec` | true | Simple esp-sr AEC. |
| `use_afe` | false | Full esp-sr AFE (heavy). |

---

## 4. AEC & AFE — echo cancellation (optional, resource-heavy)

Hands-free calling needs **acoustic echo cancellation**: the far-end audio coming
out of the speaker must be removed from the mic so the other side does not hear
themselves. Two paths are available, both from **Espressif ESP-SR**, both
**optional**.

### `enable_aec` — the lightweight path (recommended)

A simple `aec_create` filter. For each mic frame it runs
`aec_process(mic, reference)` where the **reference** is the audio just played
from the peer, aligned with a short FIFO (~2 frames, i.e. "the previous frame").
Low CPU; good enough for most rooms.

In `face2face`:

```yaml
face2face:
  enable_aec: true
  aec_mode: sr_low_cost   # sr_low_cost | sr_high_perf | voip_low_cost | voip_high_perf | fd_low_cost | fd_high_perf
  aec_filter_length: 4    # 1..8 (longer = more RAM/CPU = longer echo tail handled)
```

In `fdaudio`: `enable_aec: true`.

### `use_afe` — the full AFE (AEC + NS + AGC)

The complete esp-sr **AFE** (Audio Front-End: WebRTC noise suppression + AGC +
AEC with a properly aligned reference). It is the most thorough echo/noise fix —
**but it is heavy**. In testing it can consume most of one core continuously and
starve the rest of the pipeline, so it is **off by default**:

```yaml
fdaudio:
  use_afe: true   # full AFE; expect high CPU — measure before shipping
```

> **In our testing, both AEC and especially the AFE are expensive.** They remain
> **options** precisely so you can pick the trade-off for your board and room:
> start with `enable_aec: true` + a little `echo_suppression:`, and only reach for
> `use_afe: true` if you specifically need WebRTC-grade NS/AGC and can spare the
> CPU. Set `enable_aec: false` / `use_afe: false` to compile **none** of esp-sr
> (no managed component pulled, smaller image).

Notes:
- Enabling either path pulls the `espressif/esp-sr` managed component (large) and
  defines `FDAUDIO_USE_AEC` / `FACE2FACE_USE_AEC`.
- Buffers are 16-byte-aligned `int16`, 16 kHz mono, per Espressif's guidance.
- esp-sr master depends on `esp-dsp==1.8.0` (matches the project's `esp-dl`/`esp-dsp`
  pins, so `face_detection`/wake-word can coexist).

---

## 5. Voice assistant integration

Because `fdaudio` exposes a standard `microphone` and `speaker`, Home Assistant
**voice assistant**, `micro_wake_word`, and TTS work unchanged:

```yaml
micro_wake_word:
  models: [okay_nabu]
  microphone: esp32_microphone

voice_assistant:
  microphone: esp32_microphone
  speaker: esp32_speaker
```

> **One mic, one capture pipeline at a time.** The ES7210 mic cannot cleanly feed
> two capture consumers simultaneously. `fdaudio` reconciles listeners with
> reference-counting, but in practice you should **pause `voice_assistant` /
> `micro_wake_word` during a `face2face` call** (and resume on `on_idle`). Use
> `audio_start_delay` in `face2face` to wait for the wake-word to release the I2S
> bus before the call grabs the mic.

---

## 6. `webrtc_call` — cross-network calls (signaling + coturn + app)

`face2face` is LAN-only (direct IP). To let **any** ESP32-P4 call **any** other
P4 across **different networks / the internet** — real "FaceTime anywhere" — you
need real WebRTC: a **signaling server** to introduce the two peers and
**STUN/TURN** for NAT traversal. `webrtc_call` wraps Espressif's
[`esp-webrtc-solution`](https://github.com/espressif/esp-webrtc-solution)
(apprtc signaling, `esp_peer` ICE, MJPEG + G.711 codecs, GMF capture/render).

> **Important — hardware ownership.** `esp-webrtc` **owns** the camera (CSI), the
> I2S codec, and the LCD via Espressif's GMF (`esp_capture` / `av_render`). It
> therefore **conflicts** with `esp_video` / `fdaudio` / LVGL-on-MIPI. Use
> `webrtc_call` in a **dedicated "WebRTC-mode" firmware**: do **not** also declare
> `face2face` / `fdaudio` / a MIPI display in the same config. This is also why
> `webrtc_call` has no `width`/`height`/`framerate` knobs — that would duplicate
> `face2face`; the WebRTC codec tracks the camera capture internally.

### What you need to run (server side)

You host two things (e.g. on a home server / Unraid, internet-reachable):

1. **A signaling server** — apprtc-compatible (room based: both boards join the
   same room and get connected).
2. **coturn** — a STUN + TURN server. STUN is enough on many networks; **TURN is
   the relay fallback** required for symmetric NATs (e.g. mobile/CGNAT).

Minimal `coturn` idea (`turnserver.conf`):

```
listening-port=3478
fingerprint
lt-cred-mech
user=esp32:yourpassword
realm=yourdomain.com
# expose 3478/udp+tcp (and a relay port range) to the internet
```

### What you need (client side)

The two boards run `webrtc_call` firmware. To call **from a phone/PC** as well,
you point a small **WebRTC web app** (the apprtc-style client that ships with
`esp-webrtc-solution`, or your own) at the **same signaling server + room**. Any
standards-compliant WebRTC client that joins the room can talk to a board.

### Configuration

This is a **dedicated firmware** — its `external_components` pulls **only**
`webrtc_call` (no camera repo, no `face2face` / `fdaudio`; GMF owns the hardware):

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/youkorr/face-toface
      ref: claude/esp32p4-video-communication-W0yEc
    components: [webrtc_call]      # webrtc_call ONLY — separate from the LAN build
    refresh: always

webrtc_call:
  id: rtc
  signaling_url: "https://signal.yourdomain.com"   # your apprtc server (required)
  room: "myroom"                                    # both peers join the same room
  board_type: "ESP32_P4_DEV"                        # a codec_board definition...
  # ...or describe YOUR hardware inline instead of a predefined board:
  # board_config: |
  #   i2c: {name: i2c, sda: 7, scl: 8}
  #   i2s: {name: i2s, mclk: 13, bclk: 12, ws: 10, dout: 9, din: 11}
  #   out: {name: es8311, bus: i2c, pa: 53}
  #   in:  {name: es7210, bus: i2c}
  #   ...camera + lcd lines...
  stun_server: "stun:stun.l.google.com:19302"
  turn_url: "turn:turn.yourdomain.com:3478"
  turn_username: "esp32"
  turn_password: "yourpassword"
  auto_connect: false        # true = connect as soon as the room is joined

# Call control
on_...:
  - webrtc_call.start: rtc   # enable the peer connection
  - webrtc_call.hangup: rtc  # disable it
```

| Option | Default | Notes |
|--------|---------|-------|
| `signaling_url` | — (required) | apprtc signaling server URL. |
| `room` | `esp_room` | Peers joining the same room are connected. |
| `board_type` | `ESP32_P4_DEV` | A `codec_board` definition name. |
| `board_config` | — | Inline `codec_board` definition (parsed at runtime) — describe your exact i2c/i2s/codec/camera/lcd pinout in YAML, no predefined board needed. |
| `stun_server` | — | `stun:host:port`. |
| `turn_url` / `turn_username` / `turn_password` | — | TURN relay (NAT fallback). |
| `auto_connect` | false | Auto-connect on room join, else use `webrtc_call.start`. |

> **Status: this path is the newest and needs end-to-end testing on hardware**
> (server deployment + two boards + a browser client). MJPEG is used over WebRTC
> on purpose: it is loss-tolerant, unlike SW-decoded H.264 over lossy links.

---

## 7. Examples

| File | What it shows |
|------|---------------|
| [`example/standalone-facetime.yaml`](example/standalone-facetime.yaml) | Complete, flashable face2face config from scratch. |
| [`example/face2face-snippet.yaml`](example/face2face-snippet.yaml) | Just the `face2face:` block to paste into your YAML. |
| [`example/lvgl-call-page.yaml`](example/lvgl-call-page.yaml) | LVGL 9.x call page (1024×600) with remote video + presence dot. |
| [`example/ringtone-options.yaml`](example/ringtone-options.yaml) | Default `ring.aac`, silent, or your own file via `media_player`. |
| [`example/fdaudio-waveshare-full.yaml`](example/fdaudio-waveshare-full.yaml) | Full Waveshare audio (fdaudio + duplex + voice assistant). |
| [`example/fdaudio-habbit-full.yaml`](example/fdaudio-habbit-full.yaml) | Full Habbit board audio config. |
| [`example/waveshare-face2face.yaml`](example/waveshare-face2face.yaml) / [`habbit-face2face.yaml`](example/habbit-face2face.yaml) | Per-board face2face configs. |

---

## 8. Build notes & troubleshooting

- **Hardware JPEG enums** (`esp_driver_jpeg`): names vary slightly across
  ESP-IDF. If the build complains, check `driver/jpeg_encode.h` /
  `driver/jpeg_decode.h` in *your* IDF (`JPEG_ENCODE_IN_FORMAT_RGB565`,
  `JPEG_DECODE_OUT_FORMAT_RGB565`, `JPEG_DOWN_SAMPLING_YUV420`, …).
- **`esp_codec_dev` + I2C**: `fdaudio` forces the new `i2c_master` driver
  (`CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE=false`); the legacy I2C driver aborts at
  boot on IDF 5.4+ ("driver_ng is not allowed to be used with this old driver").
- **`width`/`height`** must match the camera's RGB output, or add a resize.
- **Stutter** (face2face): the shared JPEG engine is usually the limit. Lower
  `framerate`, raise `scale`, or lower `jpeg_quality` — in that order.
- **Faint call audio / weak wake-word**: set `fdaudio` `mic_agc: 10000`; enable
  the `amplifier:` switch in `face2face` so the PA is on during the call.
- **Echo**: start with `enable_aec: true` and a little `echo_suppression:`; only
  use `use_afe: true` if you can afford the CPU (see §4).
- **esp-sr size/refs**: enabling AEC/AFE pulls `espressif/esp-sr` (`ref: master`);
  watch flash usage on the first build.
- **WiFi**: both `face2face` boards must be on the same LAN. For `webrtc_call`,
  the boards need outbound internet to your signaling + TURN servers.

---

## 9. Sources

- P4 hardware JPEG (`esp_driver_jpeg`): ESP-IDF `components/esp_driver_jpeg`.
- ESP-SR AEC/AFE: <https://docs.espressif.com/projects/esp-sr/en/latest/esp32p4/audio_front_end/README.html>
- WebRTC stack: <https://github.com/espressif/esp-webrtc-solution>
- Audio codec lib (AAC ringtone): `espressif/esp_audio_codec` (esp-adf-libs).
- coturn (STUN/TURN): <https://github.com/coturn/coturn>
- Camera components (`esp_video` / `esp_cam_sensor` / `lvgl_camera_display`):
  <https://github.com/youkorr/test2_esp_video_esphome>
- Author's related components: <https://github.com/youkorr>
</content>
