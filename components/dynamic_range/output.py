import esphome.codegen as cg
from esphome.components import output
import esphome.config_validation as cv
from esphome.const import CONF_CHANNEL, CONF_CURRENT, CONF_ID, CONF_OUTPUT, CONF_PLATFORM
from esphome.core import CORE, ID
import esphome.final_validate as fv

AUTO_LOAD = ["output"]

ns = cg.esphome_ns.namespace("dynamic_range")
DynamicRangeOutput = ns.class_("DynamicRangeOutput", output.FloatOutput)
CurrentGroup = ns.class_("CurrentGroup", cg.Component)

CONFIG_SCHEMA = output.FLOAT_OUTPUT_SCHEMA.extend(
    {
        cv.Required(CONF_ID): cv.declare_id(DynamicRangeOutput),
        # A channel output of one of the supported chips.
        cv.Required(CONF_OUTPUT): cv.use_id(output.FloatOutput),
    }
)

# Channels 0-2 are the colours and 3-4 the whites on every shared-current chip.
COLOR, WHITE = "color", "white"


def _group_of(channel):
    return COLOR if channel <= 2 else WHITE


def _sm10bit(hub_key):
    """sm2235, sm2335: 16 steps, current = (index + 1) x step."""

    def describe(hub, hub_var, group):
        key = f"max_power_{group}_channels"
        setter = f"{hub_var}->set_max_power_{group}_channels(i)"
        return 0, hub[key], 1, setter

    return f"{hub_key}_id", describe


def _bp1658cj(hub, hub_var, group):
    """bp1658cj: 16 steps, current = index x step, so index 0 is off."""
    key = f"max_power_{group}_channels"
    setter = f"{hub_var}->set_max_power_{group}_channels(i)"
    return 1, hub[key], 0, setter


def _sm2135(hub, hub_var, group):
    """sm2135: current = 10 + 5 x index mA = (index + 2) x 5 mA."""
    key, name = ("rgb_current", "rgb") if group == COLOR else ("cw_current", "cw")
    max_index = (int(str(hub[key]).replace("mA", "")) - 10) // 5
    setter = f"{hub_var}->set_{name}_current(static_cast<sm2135::SM2135Current>(i))"
    return 0, max_index, 2, setter


# platform -> (key of the hub id in the channel's config, describe function)
SHARED = {
    "sm2235": _sm10bit("sm2235"),
    "sm2335": _sm10bit("sm2335"),
    "bp1658cj": ("bp1658cj_id", _bp1658cj),
    "sm2135": ("sm2135_id", _sm2135),
}
SUPPORTED = ["bp5758d", *SHARED]


def _find(items, id_):
    for item in items if isinstance(items, list) else [items]:
        if item.get(CONF_ID) is not None and item[CONF_ID].id == id_.id:
            return item
    return None


def _resolve(full_config, inner_id):
    """Everything needed about the wrapped channel, from the whole config.

    Returns (platform, inner channel config, hub config or None, group key).
    """
    inner = _find(full_config.get("output", []), inner_id)
    platform = inner[CONF_PLATFORM] if inner else None
    if platform not in SUPPORTED:
        raise cv.Invalid(
            f"'{inner_id.id}' is a {platform} output; dynamic_range supports "
            f"{', '.join(SUPPORTED)}"
        )
    if platform == "bp5758d":
        # Every BP5758D channel has its own current.
        return platform, inner, None, inner_id.id
    hub_key, _ = SHARED[platform]
    hub_id = inner[hub_key]
    hub = _find(full_config[platform], hub_id)
    return platform, inner, hub, f"{hub_id.id}_{_group_of(inner[CONF_CHANNEL])}"


def _final_validate(config):
    full = fv.full_config.get()
    platform, inner, hub, group = _resolve(full, config[CONF_OUTPUT])

    if platform == "bp5758d":
        if inner[CONF_CURRENT] < 1:
            raise cv.Invalid(f"'{inner[CONF_ID].id}' needs a current of at least 1 mA")
        return config

    if platform == "bp1658cj":
        group_name = _group_of(inner[CONF_CHANNEL])
        if hub[f"max_power_{group_name}_channels"] < 1:
            raise cv.Invalid(
                f"max_power_{group_name}_channels of 0 is 0 mA on the bp1658cj"
            )

    # A current change hits every channel in the group, so every one of them
    # must go through dynamic_range, or the unwrapped ones would flicker.
    hub_key, _ = SHARED[platform]
    wrapped = {
        o[CONF_OUTPUT].id
        for o in full.get("output", [])
        if o[CONF_PLATFORM] == "dynamic_range"
    }
    for o in full.get("output", []):
        if (
            o[CONF_PLATFORM] == platform
            and o[hub_key].id == inner[hub_key].id
            and _group_of(o[CONF_CHANNEL]) == _group_of(inner[CONF_CHANNEL])
            and o[CONF_ID].id not in wrapped
        ):
            raise cv.Invalid(
                f"'{o[CONF_ID].id}' shares its current with '{inner[CONF_ID].id}', "
                "so it needs a dynamic_range output too"
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def _group_for(platform, inner, inner_var, hub, key):
    groups = CORE.data.setdefault("dynamic_range", {})
    if key in groups:
        return groups[key]

    if platform == "bp5758d":
        min_index, max_index, offset = 1, inner[CONF_CURRENT], 0
        setter = f"{inner_var}->set_current(i)"
    else:
        hub_key, describe = SHARED[platform]
        hub_var = await cg.get_variable(inner[hub_key])
        min_index, max_index, offset, setter = describe(
            hub, hub_var, _group_of(inner[CONF_CHANNEL])
        )

    # The generated pointers are globals, so a capture-less lambda reaches them.
    apply = cg.RawExpression(f"[](uint8_t i) {{ {setter}; }}")
    group_id = ID(f"dynamic_range_{key}", is_declaration=True, type=CurrentGroup)
    groups[key] = cg.new_Pvariable(group_id, min_index, max_index, offset, apply)
    # A component, for its loop(): it steps the current gradually.
    CORE.component_ids.add(group_id.id)
    await cg.register_component(groups[key], {})
    return groups[key]


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await output.register_output(var, config)

    inner_var = await cg.get_variable(config[CONF_OUTPUT])
    cg.add(var.set_inner(inner_var))

    platform, inner, hub, key = _resolve(CORE.config, config[CONF_OUTPUT])
    group = await _group_for(platform, inner, inner_var, hub, key)
    cg.add(var.set_group(group))
