"""fdaudio — full-duplex I2S audio for ESP32-P4, built on Espressif APIs only.

One I2S port runs TX+RX simultaneously (esp_driver_i2s), with ES8311/ES8388
(speaker) and ES7210 (mic) driven via esp_codec_dev, and AEC via esp-sr. It
exposes standard ESPHome microphone + speaker platforms (see ./microphone.py
and ./speaker.py) so voice_assistant / media_player / face2face use them
unchanged. No third-party component.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32
from esphome import pins
from esphome.const import CONF_ID, CONF_SAMPLE_RATE

CODEOWNERS = ["@youkorr"]
DEPENDENCIES = ["esp32", "i2c"]
MULTI_CONF = False

CONF_MCLK_PIN = "mclk_pin"
CONF_BCLK_PIN = "bclk_pin"
CONF_LRCLK_PIN = "lrclk_pin"
CONF_DIN_PIN = "din_pin"
CONF_DOUT_PIN = "dout_pin"
CONF_I2C_PORT = "i2c_port"
CONF_OUTPUT_CODEC = "output_codec"
CONF_OUTPUT_ADDRESS = "output_address"
CONF_MIC_ADDRESS = "mic_address"
CONF_MIC_GAIN_DB = "mic_gain_db"
CONF_MIC_CHANNELS = "mic_channels"
CONF_OUTPUT_VOLUME = "output_volume"
CONF_USE_MCLK = "use_mclk"
CONF_ENABLE_AEC = "enable_aec"
CONF_USE_AFE = "use_afe"
CONF_CODEC_SAMPLE_RATE = "codec_sample_rate"
CONF_MIC_DIGITAL_GAIN = "mic_digital_gain"
CONF_NOISE_GATE = "noise_gate"
CONF_ECHO_SUPPRESSION = "echo_suppression"
CONF_MIC_AGC = "mic_agc"

fdaudio_ns = cg.esphome_ns.namespace("fdaudio")
FdAudio = fdaudio_ns.class_("FdAudio", cg.Component)
OutputCodec = fdaudio_ns.enum("OutputCodec")

OUTPUT_CODECS = {
    "es8311": OutputCodec.OUT_ES8311,
    "es8388": OutputCodec.OUT_ES8388,
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(FdAudio),
        cv.Required(CONF_LRCLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Required(CONF_BCLK_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_MCLK_PIN, default=-1): cv.Any(
            cv.int_(-1), pins.internal_gpio_output_pin_number
        ),
        cv.Required(CONF_DIN_PIN): pins.internal_gpio_input_pin_number,
        cv.Required(CONF_DOUT_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_I2C_PORT, default=0): cv.int_range(min=0, max=1),
        cv.Optional(CONF_OUTPUT_CODEC, default="es8311"): cv.enum(OUTPUT_CODECS, lower=True),
        cv.Optional(CONF_OUTPUT_ADDRESS, default=0x18): cv.i2c_address,
        cv.Optional(CONF_MIC_ADDRESS, default=0x40): cv.i2c_address,
        cv.Optional(CONF_MIC_GAIN_DB, default=37.5): cv.float_range(min=0.0, max=42.0),
        # ES7210 mic input bitmask: MIC1=1 MIC2=2 MIC3=4 MIC4=8 (combine to enable
        # several, e.g. 3 = MIC1+MIC2). Default MIC1; change if your board wires
        # the analog mic to another channel (mic reads near-silent otherwise).
        cv.Optional(CONF_MIC_CHANNELS, default=1): cv.int_range(min=1, max=15),
        cv.Optional(CONF_OUTPUT_VOLUME, default=70): cv.int_range(min=0, max=100),
        cv.Optional(CONF_USE_MCLK, default=True): cv.boolean,
        # Rate exposed to ESPHome (mic output / voice_assistant / face2face).
        cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_,
        # Actual I2S/codec clock. Many ESP32-P4 boards only clock the ES7210 mic
        # correctly at 48 kHz, so the engine runs the codec at 48 kHz and
        # decimates the mic down to 'sample_rate'. Must be an integer multiple.
        cv.Optional(CONF_CODEC_SAMPLE_RATE, default=48000): cv.int_,
        # Software boost applied to the (decimated) mic. Raise if the voice is
        # too weak for wake word / STT (your AFE used AGC for the same reason).
        cv.Optional(CONF_MIC_DIGITAL_GAIN, default=1.0): cv.float_range(min=1.0, max=16.0),
        # Software noise gate threshold (mic amplitude, 0..32767). 0 = disabled.
        # The mic is attenuated when only ambient noise is present (envelope below
        # the threshold) and passes at full level when you speak. Try ~250-500.
        cv.Optional(CONF_NOISE_GATE, default=0): cv.int_range(min=0, max=5000),
        # Far-end ducking / echo suppression for calls (% mic attenuation while
        # the speaker plays the far end). 0 = off. ~80-90 kills call echo. Leave
        # 0 for voice_assistant (it would break barge-in); face2face can enable
        # it per-call at runtime via id(audio_engine).set_echo_suppression(85).
        cv.Optional(CONF_ECHO_SUPPRESSION, default=0): cv.int_range(min=0, max=100),
        # Automatic gain control: auto-boost a weak mic to this target peak
        # (0 = off). ~10000 gives a good call level. Replaces hand-tuned
        # mic_digital_gain. This is what fixes faint face2face audio.
        cv.Optional(CONF_MIC_AGC, default=0): cv.int_range(min=0, max=30000),
        cv.Optional(CONF_ENABLE_AEC, default=True): cv.boolean,
        # Use the full esp-sr AFE (AEC + NS + AGC with an aligned reference)
        # instead of the simple aec_create path. The proper echo fix.
        cv.Optional(CONF_USE_AFE, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_pins(
        config[CONF_MCLK_PIN], config[CONF_BCLK_PIN], config[CONF_LRCLK_PIN],
        config[CONF_DIN_PIN], config[CONF_DOUT_PIN],
    ))
    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_codec_sample_rate(config[CONF_CODEC_SAMPLE_RATE]))
    cg.add(var.set_mic_digital_gain(config[CONF_MIC_DIGITAL_GAIN]))
    cg.add(var.set_noise_gate(config[CONF_NOISE_GATE]))
    cg.add(var.set_echo_suppression(config[CONF_ECHO_SUPPRESSION]))
    cg.add(var.set_mic_agc(config[CONF_MIC_AGC]))
    cg.add(var.set_i2c_port(config[CONF_I2C_PORT]))
    cg.add(var.set_output_codec(config[CONF_OUTPUT_CODEC]))
    cg.add(var.set_codec_addrs(config[CONF_OUTPUT_ADDRESS], config[CONF_MIC_ADDRESS]))
    cg.add(var.set_mic_gain_db(config[CONF_MIC_GAIN_DB]))
    cg.add(var.set_mic_channels(config[CONF_MIC_CHANNELS]))
    cg.add(var.set_out_volume(config[CONF_OUTPUT_VOLUME]))
    cg.add(var.set_use_mclk(config[CONF_USE_MCLK]))
    cg.add(var.set_aec_enabled(config[CONF_ENABLE_AEC]))
    cg.add(var.set_use_afe(config[CONF_USE_AFE]))

    # Espressif codec driver (drives ES8311/ES8388/ES7210 over I2C+I2S).
    # Use a recent version + force the NEW i2c_master driver. The default builds
    # esp_codec_dev against the legacy I2C driver, which conflicts with ESPHome's
    # new i2c_master driver on IDF 5.4+ -> boot abort: "driver_ng is not allowed
    # to be used with this old driver".
    esp32.add_idf_component(name="espressif/esp_codec_dev", ref="1.5.4")
    esp32.add_idf_sdkconfig_option("CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE", False)

    # Force the I2S driver's control structures into INTERNAL RAM. The i2s
    # driver allocates its channel object with MALLOC_CAP_DEFAULT; on builds
    # where default mallocs may land in PSRAM, gdma (when compiled with
    # CONFIG_GDMA_ISR_IRAM_SAFE, as video/LCD stacks do) then rejects it at
    # channel init: "gdma: user context not in internal RAM" ->
    # "i2s tx init_std failed". I2S_ISR_IRAM_SAFE switches those allocations
    # to MALLOC_CAP_INTERNAL and puts the I2S ISR in IRAM, which is also the
    # safe pairing whenever GDMA_ISR_IRAM_SAFE is enabled elsewhere.
    esp32.add_idf_sdkconfig_option("CONFIG_I2S_ISR_IRAM_SAFE", True)

    if config[CONF_ENABLE_AEC] or config[CONF_USE_AFE]:
        # esp-sr master depends on esp-dsp 1.8.0 (matches the project override
        # 'espressif/esp-dsp==1.8.0' and esp-dl >=1.7.0). Pulls both the simple
        # AEC (aec_create) and the full AFE (esp_afe_sr).
        esp32.add_idf_component(
            name="esp-sr", repo="https://github.com/espressif/esp-sr",
            ref="master",
        )
        cg.add_define("FDAUDIO_USE_AEC")
