# esphome-dynamic-range-led-driver

For ESPHome bulbs with one of these LED driver chips: **BP5758D, BP5768,
BP1658CJ, SM2235, SM2335, SM2135**. It gives you two things:

- **A much lower minimum brightness.** On a BP5758D bulb with `current: 16`,
  the lowest setting is 16 times dimmer than with ESPHome alone. On my
  bulbs it is so dim that you only see it in a pitch black room at night.
- **Smooth dimming at low brightness**, with many more steps and no visible
  jumps.

## Why

Each channel on these chips has two brightness settings:

- a coarse one, the **current setting** (for example 1 to 90 mA on the BP5758D)
- a fine one, the **level** (10 bits, so 1023 steps; 8 bits on the SM2135)

ESPHome fixes the current setting and dims with the level only. So a dim light
uses only the bottom of the level's range:

- the lowest brightness above off is 1 of 1023 level steps at full current,
  about 0.1 %
- below 10 % brightness there are only 102 steps, so dimming jumps

## What this does

When the light is dim, it lowers the current setting and raises the level to
match. The level stays between half and full, where it has hundreds of steps.
At the lowest current, the level can still go all the way down, so the
minimum brightness drops too.

| Chip, setting | Steps below 10 % | | Lowest brightness | |
|---|---|---|---|---|
| | ESPHome | this | ESPHome | this |
| BP5758D, `current: 16` | 102 | 1313 | 0.098 % | 0.006 % |
| SM2235 / SM2335 colour, `max_power_color_channels: 15` | 102 | 1313 | 0.098 % | 0.006 % |
| SM2235 / SM2335 white, `max_power_white_channels: 4` | 102 | 511 | 0.098 % | 0.020 % |
| BP1658CJ colour, `max_power_color_channels: 15` | 102 | 1279 | 0.098 % | 0.007 % |
| SM2135 white, `cw_current: 60mA` | 25 | 152 | 0.39 % | 0.065 % |

These numbers come from `tests/run.sh`, which simulates each chip. A lower
minimum means the light goes dimmer before it turns off. More steps mean
smaller jumps when you dim.

While the light is on, the current never jumps. It moves one step per loop
pass (about 16 ms), and the level follows each step. A big jump would flash:
the chip applies the current bytes before the level bytes, so for a moment it
runs the new current with the old level. So switching dynamic range on or off,
or a big brightness jump, takes up to about half a second to settle.

## Install

Keep your chip's outputs as they are. Add one `dynamic_range` output for each
channel, and give those to the light:

```yaml
external_components:
  - source: github://dbuezas/esphome-dynamic-range-led-driver

bp5758d:
  data_pin: GPIO4
  clock_pin: GPIO5

output:
  - {platform: bp5758d, id: red_raw,   channel: 3, current: 16}
  - {platform: bp5758d, id: green_raw, channel: 2, current: 16}
  - {platform: bp5758d, id: blue_raw,  channel: 1, current: 16}
  - {platform: bp5758d, id: warm_raw,  channel: 4, current: 26}
  - {platform: bp5758d, id: cold_raw,  channel: 5, current: 26}

  - {platform: dynamic_range, id: red,   output: red_raw}
  - {platform: dynamic_range, id: green, output: green_raw}
  - {platform: dynamic_range, id: blue,  output: blue_raw}
  - {platform: dynamic_range, id: warm,  output: warm_raw}
  - {platform: dynamic_range, id: cold,  output: cold_raw}

light:
  - platform: rgbww
    name: Bulb
    red: red
    green: green
    blue: blue
    warm_white: warm
    cold_white: cold
    cold_white_color_temperature: 153 mireds
    warm_white_color_temperature: 500 mireds
```

There are no other settings. The current you already set on the chip is used
at full brightness. There is an example for each chip in
[`examples/`](examples).

## Rules

- On BP1658CJ, SM2235, SM2335 and SM2135, the three colour channels share one
  current setting, and the two white channels share another. Wrap all channels
  of a group, not only some. The config check tells you if one is missing.
- Put `min_power`, `max_power` and `inverted` on the `dynamic_range` output,
  not on the chip's output.

## Compare with and without

This switch turns the component off and on. Every example in
[`examples/`](examples) has it:

```yaml
switch:
  - platform: template
    name: Dynamic range
    optimistic: true
    restore_mode: RESTORE_DEFAULT_ON
    turn_on_action:
      - lambda: dynamic_range::set_enabled(true);
    turn_off_action:
      - lambda: dynamic_range::set_enabled(false);
```

## Tested on

- **BP5758D:** the same logic has run on two bulbs since 2022. The test
  checks that this component gives the same output as that code.
- **BP5768:** works on a GY G150 23 W bulb (two BP5768, BK7231N). Use the
  `bp5758d` platform: the BP5768 speaks the BP5758D protocol, and
  `dynamic_range` treats it the same way.
- **BP1658CJ, SM2235, SM2335, SM2135:** compiled and simulated only. If you
  have one of these bulbs, please open an issue and say if it works.

## Known limits

- The current setting changes in steps: 1 mA on the BP5758D, larger on the
  other chips. If the chip's real current for a step is a bit off, you see a
  very small jump in brightness at that step.
- With `separate_modes: true` (the default), the SM2135 driver only sends the
  group (colour or white) that changed last. ESPHome does this, not this
  component.

## Development

```sh
tests/run.sh                          # logic test, needs only a C++ compiler
esphome compile examples/bp5758d.yaml # a real build
```

## License

MIT
