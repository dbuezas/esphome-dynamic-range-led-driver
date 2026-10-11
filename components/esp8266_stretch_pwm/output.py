"""ESPHome's esp8266_pwm, with a stretched period at the dim end.

A drop-in replacement under its own name: change `platform: esp8266_pwm` to
`platform: esp8266_stretch_pwm`. Same options, plus min_pulse and min_frequency
for the dim end (see esp8266_stretch_pwm.h).
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


ns = cg.esphome_ns.namespace("esp8266_stretch_pwm")
ESP8266StretchPWM = ns.class_("ESP8266StretchPWM", output.FloatOutput, cg.Component)
SetFrequencyAction = ns.class_("SetFrequencyAction", automation.Action)
validate_frequency = cv.All(cv.frequency, cv.float_range(min=1.0e-6))

DEFAULT_FREQUENCY = 1000.0  # also the C++ default
CONF_MIN_PULSE = "min_pulse"
CONF_MIN_FREQUENCY = "min_frequency"
CONF_FINE_STEPS = "fine_steps"
DEFAULT_MIN_PULSE_US = 1.0  # also the C++ default
DEFAULT_MIN_FREQUENCY = 250.0  # also the C++ default


def _min_frequency_below_frequency(config):
    if config[CONF_MIN_FREQUENCY] > config[CONF_FREQUENCY]:
        raise cv.Invalid(f"{CONF_MIN_FREQUENCY} must not be above {CONF_FREQUENCY}")
    return config

CONFIG_SCHEMA = output.FLOAT_OUTPUT_SCHEMA.extend(
    {
        cv.Required(CONF_ID): cv.declare_id(ESP8266StretchPWM),
        cv.Required(CONF_PIN): cv.All(pins.internal_gpio_output_pin_schema, valid_pwm_pin),
        cv.Optional(CONF_FREQUENCY, default=DEFAULT_FREQUENCY): validate_frequency,
        # The shortest pulse to make; below it the period gets longer instead.
        cv.Optional(CONF_MIN_PULSE, default="1us"): cv.positive_time_period_nanoseconds,
        # How far the period may get longer at the dim end.
        cv.Optional(CONF_MIN_FREQUENCY, default=DEFAULT_MIN_FREQUENCY): validate_frequency,
        # CPU-cycle steps instead of whole microseconds like esp8266_pwm.
        cv.Optional(CONF_FINE_STEPS, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)
CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _min_frequency_below_frequency)


async def to_code(config):
    require_waveform()
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await output.register_output(var, config)
    cg.add(var.set_pin(await cg.gpio_pin_expression(config[CONF_PIN])))
    if config[CONF_FREQUENCY] != DEFAULT_FREQUENCY:
        cg.add(var.set_frequency(config[CONF_FREQUENCY]))
    min_pulse_us = config[CONF_MIN_PULSE].total_nanoseconds / 1000
    if min_pulse_us != DEFAULT_MIN_PULSE_US:
        cg.add(var.set_min_pulse(float(min_pulse_us)))
    if config[CONF_MIN_FREQUENCY] != DEFAULT_MIN_FREQUENCY:
        cg.add(var.set_min_frequency(config[CONF_MIN_FREQUENCY]))
    if config[CONF_FINE_STEPS]:
        cg.add(var.set_fine_steps(True))


@automation.register_action(
    "output.esp8266_stretch_pwm.set_frequency",
    SetFrequencyAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(ESP8266StretchPWM),
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
