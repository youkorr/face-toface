# ESP32-S3-BOX-3 - Full-duplex audio with fdaudio

This document explains how to configure and run full-duplex audio
(simultaneous microphone and speaker) on the ESP32-S3-BOX-3 using the
`fdaudio` component, including echo cancellation (AEC / AFE) based on esp-sr.

The complete example file is `example/fdaudio-s3box3-duplex-test.yaml`.

## 1. Hardware

The ESP32-S3-BOX-3 is an ESP32-S3 N16R8 board (16 MB flash, 8 MB octal PSRAM).
Its audio subsystem has two separate codecs on the same I2S bus and the same
I2C bus:

| Role                | Codec   | I2C address | Direction |
| ------------------- | ------- | ----------- | --------- |
| Speaker (output)    | ES8311  | 0x18        | DAC       |
| Microphones (input) | ES7210  | 0x40        | ADC       |

The speaker amplifier (PA) is controlled by GPIO46. It is disabled on reset and
must be driven high at boot, otherwise the codec plays but no sound comes out.

### Pin mapping

| Signal        | GPIO    |
| ------------- | ------- |
| I2C SDA       | GPIO8   |
| I2C SCL       | GPIO18  |
| I2S MCLK      | GPIO2   |
| I2S BCLK      | GPIO17  |
| I2S WS/LRCLK  | GPIO45  |
| I2S DIN (mic) | GPIO16  |
| I2S DOUT (HP) | GPIO15  |
| PA_CTRL (amp) | GPIO46  |

## 2. How full-duplex works

`fdaudio` opens a single I2S port in full-duplex mode: one call to
`i2s_new_channel` creates a TX handle and an RX handle at the same time, sharing
the same clock. The same I2S data interface is shared by the ES8311 (output) and
the ES7210 (input) through `esp_codec_dev`. The microphone and the speaker
therefore run at the same time on the same clock, which is the condition for
true full-duplex.

The codec runs at 48 kHz (the clock at which the ES7210 is genuinely stable). On
the ESPHome side:

- the microphone is decimated from 48 kHz to 16 kHz (average of groups of 3
  samples) before being exposed to `voice_assistant` or `face2face`;
- the speaker receives audio at 48 kHz; 16 kHz sources are upsampled (linear
  interpolation) before being written.

The component exposes a standard ESPHome `microphone` and `speaker`, so
`voice_assistant`, `media_player` and `face2face` use them unchanged.

## 3. fdaudio configuration

```yaml
fdaudio:
  id: audio_engine
  mclk_pin: GPIO2
  bclk_pin: GPIO17
  lrclk_pin: GPIO45
  din_pin: GPIO16
  dout_pin: GPIO15
  i2c_port: 0
  output_codec: es8311
  output_address: 0x18
  mic_address: 0x40
  mic_gain_db: 37.5
  output_volume: 70
  sample_rate: 16000          # rate exposed to ESPHome
  codec_sample_rate: 48000    # actual I2S/codec clock, decimated to 16 kHz
  mic_channels: 1             # ES7210: MIC1=1, MIC2=2, MIC3=4, MIC4=8
  enable_aec: true
  use_afe: true
  aec_gate_ms: 250
```

### Reference of the main options

| Option              | Default | Role |
| ------------------- | ------- | ---- |
| `sample_rate`       | 16000   | Microphone rate exposed to ESPHome. |
| `codec_sample_rate` | 48000   | Actual I2S/codec clock. Must be an integer multiple of `sample_rate`. |
| `mic_gain_db`       | 37.5    | ES7210 analog gain (0 to 42 dB). |
| `mic_channels`      | 1       | ES7210 mic input bitmask. Change it if the microphone is nearly silent. |
| `mic_digital_gain`  | 1.0     | Software boost of the decimated microphone (1.0 to 16.0). |
| `mic_agc`           | 0       | Automatic gain control toward a target level (0 = off). |
| `noise_gate`        | 0       | Software noise-gate threshold (0 = off). |
| `echo_suppression`  | 0       | Far-end ducking for calls, in percent (0 = off). |
| `enable_aec`        | true    | Enable the simple esp-sr AEC (aec_create). |
| `use_afe`           | false   | Enable the full esp-sr AFE (AEC + NS + AGC). |
| `aec_gate_ms`       | 250     | AEC adaptation window after speaker activity (0 = off). |

## 4. Echo cancellation: simple AEC or full AFE

Two paths are available. Both rely on esp-sr.

### Simple AEC (`enable_aec: true`, `use_afe: false`)

Lightweight path based on `aec_create` in `AEC_MODE_SR_LOW_COST` mode. The
processing runs inline in the microphone task. Modest footprint (a few kilobytes
of buffers, moderate CPU load). This is the recommended mode for coexistence
with `voice_assistant` and `micro_wake_word`, because linear AEC preserves the
spectral features needed by the neural wake word.

### Full AFE (`use_afe: true`)

Complete esp-sr pipeline: AEC, noise suppression (NS) and automatic gain control
(AGC), with an aligned far-end reference. Input format "MNR" (microphone, null
channel, reference). Much heavier:

- flash: esp-sr, esp-dl and esp-dsp add on the order of 1 MB to the binary;
- PSRAM required (several hundred kilobytes of internal buffers);
- significant and continuous CPU load;
- dedicated tasks (codec read and AFE feed, plus the esp-sr internal processing
  thread).

The AFE is only worth it when the simple AEC does not suppress enough echo.

### Core placement

The real-time tasks (codec read/write, microphone, speaker) run on core 1. The
heavy AFE processing (esp-sr internal thread and feed task) is placed on core 0
so it does not starve the real-time audio path. Without this separation the
audio can become choppy.

## 5. AEC gating (aec_gate_ms)

Echo only exists in the microphone while the speaker is (or just was) playing.
Outside that window there is nothing to cancel; if the adaptive filter keeps
running on a microphone-only signal it drifts and eventually attenuates real
speech.

`aec_gate_ms` defines the adaptation window after speaker activity:

- inside the window, the AEC adapts normally;
- beyond it (true speaker silence) adaptation is frozen:
  - in simple AEC, `aec_process` is skipped and the microphone passes through
    untouched;
  - in AFE, the reference is forced to zero (the ring is still drained to keep
    microphone/reference alignment).

Tuning:

- 250 ms (default) covers the usual acoustic and codec tail;
- increase it (350 to 500) if the start of sentences is cut right after the
  speaker stops;
- decrease it (150 to 200) if too much echo passes immediately after the speaker
  stops;
- `aec_gate_ms: 0` disables gating (AEC always on, the previous behaviour).

The gating state is printed in the logs at startup:

```
AEC: enabled (gate: on)
AEC gate window: 250 ms
```

## 6. Dependencies and esp-dsp alignment

Enabling `enable_aec` or `use_afe` automatically pulls esp-sr into the firmware.
esp-sr and esp-dl (the latter also pulled by `micro_wake_word`) each declare
their own esp-dsp dependency. Without a single forced version, the IDF component
manager can resolve different and incompatible esp-dsp versions; the audio/ML
stack then faults at startup and both the speaker and the microphone are lost,
while the same firmware without AEC works (because esp-sr is never pulled).

To avoid this, the `fdaudio` component pins `espressif/esp-dsp==1.8.0` itself as
soon as AEC or AFE is enabled. The example file also adds the override at the
project level (final priority):

```yaml
esp32:
  framework:
    type: esp-idf
    components:
      - espressif/esp-dsp==1.8.0
    advanced:
      enable_idf_experimental_features: true
```

This alignment lets the esp-sr AFE and `micro_wake_word` coexist.

## 7. Build and flash

```bash
esphome run example/fdaudio-s3box3-duplex-test.yaml
```

Notes:

- `compile_process_limit: 1` is recommended (the esp-dl and esp-sr stacks make
  compilation heavy and can trigger an out-of-memory "cc1plus Killed").
- Octal PSRAM is required by the AFE; the example enables
  `CONFIG_SPIRAM_MODE_OCT`, `CONFIG_SPIRAM_SPEED_80M` and
  `CONFIG_SPIRAM_USE_MALLOC`.
- The task watchdog is disabled at startup, because esp-sr initialization can
  block the loop task for a few seconds.

## 8. Validating full-duplex

1. Start the voice assistant.
2. During the spoken response (TTS), say the wake word.
3. Interpretation:
   - the device hears you while it is speaking: full-duplex is working;
   - it only hears you once the speaker goes silent: the AEC or the duplex needs
     adjustment.

The first microphone reads are logged at INFO level:

```
mic read #1: ret=0 raw_peak=1234 (read 1536 codec samples)
```

A `raw_peak` close to 0 indicates a silent microphone (see troubleshooting).

## 9. Troubleshooting

| Symptom | Likely cause | Action |
| ------- | ------------ | ------ |
| No sound and no mic as soon as AEC/AFE is enabled | Incompatible esp-dsp versions between esp-sr and esp-dl | Check the `esp-dsp==1.8.0` pin (automatic in the component; also present in the example). |
| No sound while the microphone works | PA not enabled | Make sure GPIO46 is driven high at boot (`output.turn_on`). |
| Microphone nearly silent (`raw_peak` close to 0) | Wrong ES7210 channel | Try `mic_channels: 2`, `4`, or `3` depending on the wiring. |
| Microphone too weak for the wake word | Insufficient gain | Increase `mic_gain_db`, or use `mic_digital_gain` / `mic_agc`. |
| AEC cuts the start of sentences | Gating window too short | Increase `aec_gate_ms` (350 to 500). |
| Residual echo right after the speaker stops | Gating window too long | Decrease `aec_gate_ms` (150 to 200). |
| Choppy sound while the AFE runs | CPU contention on core 1 | Confirm the AFE is on core 0 (the default placement). |
| Compilation aborted ("cc1plus Killed") | Out of memory during compilation | Keep `compile_process_limit: 1`. |

## 10. Resources and trade-offs

| Path | Flash | RAM / PSRAM | CPU |
| ---- | ----- | ----------- | --- |
| Without AEC | minimal | minimal | minimal |
| Simple AEC | a few hundred KB | a few KB | moderate (inline) |
| Full AFE | on the order of 1 MB | several hundred KB in PSRAM | significant (core 0) |

Recommendation: for reliable full-duplex with barge-in at the lowest cost, start
with `enable_aec: true` and `use_afe: false`, then move to the AFE only if the
residual echo remains a problem.
