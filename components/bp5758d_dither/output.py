"""Temporal dithering for one BP5758D/BP5768 channel, see bp5758d_dither.h."""

import esphome.codegen as cg
from esphome.components import output
import esphome.config_validation as cv
from esphome.const import CONF_CHANNEL, CONF_ID, CONF_OUTPUT, CONF_PLATFORM
from esphome.core import CORE
import esphome.final_validate as fv

AUTO_LOAD = ["output"]
CODEOWNERS = ["@dbuezas"]

ns = cg.esphome_ns.namespace("bp5758d_dither")
DitherOutput = ns.class_("DitherOutput", output.FloatOutput, cg.Component)

CONF_BP5758D_ID = "bp5758d_id"

CONFIG_SCHEMA = (
    output.FLOAT_OUTPUT_SCHEMA.extend(
        {
            cv.Required(CONF_ID): cv.declare_id(DitherOutput),
            # A bp5758d channel output.
            cv.Required(CONF_OUTPUT): cv.use_id(output.FloatOutput),
        }
    )
    # A component, for its loop(): the resend after a wake, and the frames
    # where there is no dithering task.
    .extend(cv.COMPONENT_SCHEMA)
)


def _channel_config(full_config, channel_id):
    for item in full_config.get("output", []):
        if item.get(CONF_ID) is not None and item[CONF_ID].id == channel_id.id:
            return item
    return None


def _final_validate(config):
    channel = _channel_config(fv.full_config.get(), config[CONF_OUTPUT])
    platform = channel[CONF_PLATFORM] if channel else None
    if platform != "bp5758d":
        raise cv.Invalid(
            f"'{config[CONF_OUTPUT].id}' is a {platform} output; bp5758d_dither needs a bp5758d output"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await output.register_output(var, config)
    cg.add(var.set_inner(await cg.get_variable(config[CONF_OUTPUT])))

    channel = _channel_config(CORE.config, config[CONF_OUTPUT])
    chip = await cg.get_variable(channel[CONF_BP5758D_ID])
    index = channel[CONF_CHANNEL] - 1
    # The generated pointers are globals, so capture-less lambdas reach them.
    direct = cg.RawExpression(f"[](uint16_t c) {{ bp5758d_dither::bp5758d_write({chip}, {index}, c); }}")
    pending = cg.RawExpression(f"[]() {{ return bp5758d_dither::bp5758d_send_pending({chip}); }}")
    cg.add(var.set_chip(direct, pending))

    # One output per chip watches for the chip waking up.
    watched = CORE.data.setdefault("bp5758d_dither", set())
    if channel[CONF_BP5758D_ID].id not in watched:
        watched.add(channel[CONF_BP5758D_ID].id)
        all_off = cg.RawExpression(f"[]() {{ return bp5758d_dither::bp5758d_all_off({chip}); }}")
        resend = cg.RawExpression(f"[]() {{ bp5758d_dither::bp5758d_resend({chip}); }}")
        cg.add(var.set_wake(all_off, resend))
