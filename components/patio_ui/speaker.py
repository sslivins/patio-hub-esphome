import esphome.codegen as cg
from esphome.components import audio, speaker
import esphome.config_validation as cv
from esphome.const import CONF_ID

from . import patio_ui_ns

CODEOWNERS = ["@sslivins"]
DEPENDENCIES = ["patio_ui", "esp32"]
AUTO_LOAD = ["ring_buffer"]

# Locked to the BSP duplex I2S rate shared with the microphone platform.
SAMPLE_RATE = 16000

PatioSpeaker = patio_ui_ns.class_("PatioSpeaker", speaker.Speaker, cg.Component)


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
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(PatioSpeaker),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _set_stream_limits,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)
