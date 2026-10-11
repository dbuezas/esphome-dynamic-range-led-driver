"""ESPHome's esp8266_pwm, with CPU-cycle instead of microsecond steps.

A drop-in replacement under its own name: change `platform: esp8266_pwm` to
`platform: esp8266_fine_pwm`. Same options.
"""

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import output
from esphome.components.esp8266.const import require_waveform
import esphome.config_validation as cv
from esphome.const import CONF_FREQUENCY, CONF_ID, CONF_NUMBER, CONF_PIN

CODEOWNERS = ["@dbuezas"]
DEPENDENCIES = ["esp8266"]


def valid_pwm_pin(value):
    cv.one_of(0, 1, 2, 3, 4, 5, 9, 10, 12, 13, 14, 15, 16)(value[CONF_NUMBER])
    return value


ns = cg.esphome_ns.namespace("esp8266_fine_pwm")
ESP8266FinePWM = ns.class_("ESP8266FinePWM", output.FloatOutput, cg.Component)
SetFrequencyAction = ns.class_("SetFrequencyAction", automation.Action)
validate_frequency = cv.All(cv.frequency, cv.float_range(min=1.0e-6))

DEFAULT_FREQUENCY = 1000.0  # also the C++ default

CONFIG_SCHEMA = output.FLOAT_OUTPUT_SCHEMA.extend(
    {
        cv.Required(CONF_ID): cv.declare_id(ESP8266FinePWM),
        cv.Required(CONF_PIN): cv.All(pins.internal_gpio_output_pin_schema, valid_pwm_pin),
        cv.Optional(CONF_FREQUENCY, default=DEFAULT_FREQUENCY): validate_frequency,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    require_waveform()
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await output.register_output(var, config)
    cg.add(var.set_pin(await cg.gpio_pin_expression(config[CONF_PIN])))
    if config[CONF_FREQUENCY] != DEFAULT_FREQUENCY:
        cg.add(var.set_frequency(config[CONF_FREQUENCY]))


@automation.register_action(
    "output.esp8266_fine_pwm.set_frequency",
    SetFrequencyAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(ESP8266FinePWM),
            cv.Required(CONF_FREQUENCY): cv.templatable(validate_frequency),
        }
    ),
    synchronous=True,
)
async def set_frequency_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    cg.add(var.set_frequency(await cg.templatable(config[CONF_FREQUENCY], args, cg.float_)))
    return var
