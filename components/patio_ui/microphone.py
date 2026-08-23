import esphome.codegen as cg
from esphome.components import audio, microphone
import esphome.config_validation as cv
from esphome.const import CONF_ID

from . import patio_ui_ns

CODEOWNERS = ["@sslivins"]
DEPENDENCIES = ["patio_ui", "esp32"]

CONF_GAIN_DB = "gain_db"

# Locked to the BSP duplex I2S rate shared with the speaker platform; both
# surfaces must open at the same rate (esp_codec_dev rejects a paired mismatch).
SAMPLE_RATE = 16000

PatioMicrophone = patio_ui_ns.class_(
    "PatioMicrophone", microphone.Microphone, cg.Component
)


def _set_stream_limits(config):
    audio.set_stream_limits(
        min_bits_per_sample=16,
        max_bits_per_sample=16,
        min_channels=1,
        max_channels=1,
        min_sample_rate=SAMPLE_RATE,
        max_sample_rate=SAMPLE_RATE,
    )(config)
    return config


CONFIG_SCHEMA = cv.All(
    microphone.MICROPHONE_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(PatioMicrophone),
            cv.Optional(CONF_GAIN_DB, default=30.0): cv.float_range(min=0.0, max=60.0),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _set_stream_limits,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await microphone.register_microphone(var, config)
    cg.add(var.set_gain_db(config[CONF_GAIN_DB]))
