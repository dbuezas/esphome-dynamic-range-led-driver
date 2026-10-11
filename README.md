[!["Buy Me A Coffee"](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/dbuezas)

# esphome-dynamic-range-led-driver

For ESPHome bulbs with one of these LED driver chips: **BP5758D, BP5768,
BP1658CJ, SM2235, SM2335, SM2135**. It gives you two things:

- **A much lower minimum brightness.** On a BP5758D bulb with `current: 16`,
  the lowest setting is 16 times dimmer than with ESPHome alone. On my
  bulbs it is so dim that you only see it in a pitch black room at night.
- **Smooth dimming at low brightness**, with many more steps and no visible
  jumps.

The repo also has two smaller components, see below: `bp5758d_dither`
(temporal dithering, to go even lower on the BP5758D and BP5768) and
`esp8266_stretch_pwm` (for bulbs dimmed by ESP8266 PWM).

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
  checks that this component gives the same output as that code. `bp5758d_dither`
  runs on one of them (an 18 W globe, ESP8266).
- **BP5768:** works on a GY G150 23 W bulb (two BP5768, BK7231N). Use the
  `bp5758d` platform: the BP5768 speaks the BP5758D protocol, and
  `dynamic_range` treats it the same way. `bp5758d_dither` runs on the same
  bulb.
- **BP1658CJ, SM2235, SM2335, SM2135:** compiled and simulated only. If you
  have one of these bulbs, please open an issue and say if it works.

## Known limits

- The current setting changes in steps: 1 mA on the BP5758D, larger on the
  other chips. If the chip's real current for a step is a bit off, you see a
  very small jump in brightness at that step.
- With `separate_modes: true` (the default), the SM2135 driver only sends the
  group (colour or white) that changed last. ESPHome does this, not this
  component.

## bp5758d_dither (BP5758D and BP5768)

At `dynamic_range`'s lowest current, the level itself goes down to a few
codes, and each code is a big jump: from 1 to 2 doubles the brightness, and
below 1 the light is off. `bp5758d_dither` switches fast between two
neighbouring codes: a wanted 3.4 shows 4 in 40 % of the frames and 3 in the
others. It uses error diffusion, so the average is exact.

What each layer gives on a BP5758D with `current: 16`, at the dim end:

| | Lowest brightness | Largest jump between neighbouring levels (1 to 40 codes at the lowest current) |
|---|---|---|
| ESPHome alone | 0.098 % | 100 % |
| `dynamic_range` | 0.0061 % (16 times lower) | 100 % |
| `dynamic_range` + `bp5758d_dither` | 0.0012 % (80 times lower) | 20 % |

These numbers come from `tests/run.sh`, which simulates the chip at 250
frames per second, what the main loop of a BK7231N gives (see below). With
`current: 90` the lowest brightness is 0.00022 %. At 1000 frames per
second it would be 4 times better again. On my G150 lamps the difference is
visible in slow fades at the very dim end.

It sits between `dynamic_range` and the chip's channel:

```yaml
external_components:
  - source: github://dbuezas/esphome-dynamic-range-led-driver
    components: [dynamic_range, bp5758d_dither]

output:
  - {platform: bp5758d, id: ch_red, channel: 3, current: 14}
  - {platform: bp5758d_dither, id: dith_red, output: ch_red}
  - {platform: dynamic_range, id: out_red, output: dith_red}
  # ... the same for each channel; the light uses out_red etc.

switch:
  - platform: template
    name: Dithering
    optimistic: true
    restore_mode: RESTORE_DEFAULT_OFF
    turn_on_action:
      - lambda: bp5758d_dither::set_enabled(true);
    turn_off_action:
      - lambda: bp5758d_dither::set_enabled(false);
```

Dithering is off until the switch turns it on. Off, the component passes
every level on unchanged. It also works without `dynamic_range`, on a plain
`bp5758d` channel. See [`examples/bp5758d_dither.yaml`](examples/bp5758d_dither.yaml).

How it works:

- Each frame writes only that channel's 2 level bytes straight to the chip
  (3 bytes, about 0.6 ms on a BK7231N), and only if the code changed. The
  driver itself always sends all 17 bytes, which is too slow for this.
- The frames come from the main loop, which runs continuously while a
  channel dithers. At most 1000 frames per second; a BK7231N gets about 250,
  because its main loop waits for the 2 ms system tick after every pass.
- Only the level is dithered, never the current, so it cannot flash.
- Only below 256 codes. Above that, one code is less than 0.4 %.
- The pattern never repeats slower than 50 times per second, unless its
  ripple is under 1 %. Where it would, the level is rounded to the nearest
  code instead. A channel that is on never rounds down to off: it stays at
  the smallest share that still repeats fast enough (1/5 of a code at 250
  frames per second). So in a colour, no channel goes dark before the
  others. `bp5758d_dither::set_min_flicker_hz(hz)` changes the 50: higher
  is safer against flicker, lower dims deeper. On my lamps I see flicker at
  the dim end only from the side of my eye, and only well below 50 Hz.

Also, with dithering on or off: the chip sleeps while every level is 0, and
may lose the first write after it wakes. That shows as a too long black,
then a too bright start, e.g. when a light changes between colour and white.
So after a wake, `bp5758d_dither` has the driver send once more.

## esp8266_stretch_pwm (ESP8266 PWM bulbs)

Another component in this repo, for bulbs without a driver chip, where the
ESP8266 dims the LEDs with PWM (for example Athom GU10 and 15 W bulbs). It
replaces ESPHome's `esp8266_pwm` under its own name.

ESPHome's `esp8266_pwm` at 1000 Hz turns the pin on for at least 1 µs per
1000 µs period, so its lowest brightness is 0.1 %. Many bulbs have a driver
behind the pin that loses pulses shorter than about 1 µs, so a shorter pulse
does not help. This component keeps the pulse at `min_pulse` and makes the
period longer instead. The pulse rate then drops from `frequency` down to
`min_frequency`, only at the dim end.

| `min_frequency` | Lowest brightness (1 µs pulse) |
|---|---|
| 1000 Hz (`esp8266_pwm`) | 0.1 % |
| 250 Hz (default) | 0.025 % |
| 40 Hz | 0.004 % |

On my Athom GU10 bulbs, 40 Hz shows no flicker at that dim level, and 25 Hz
flickers. Above 0.1 %, it works like `esp8266_pwm` at 1000 Hz.

Times are in CPU cycles (12.5 ns at 80 MHz), not whole microseconds as in
`esp8266_pwm`.

```yaml
external_components:
  - source: github://dbuezas/esphome-dynamic-range-led-driver
    components: [esp8266_stretch_pwm]

output:
  - platform: esp8266_stretch_pwm   # was: esp8266_pwm
    id: warm_white_output
    pin: GPIO13
    min_frequency: 40Hz             # default 250Hz
    # frequency: 1000Hz             # default, as in esp8266_pwm
    # min_pulse: 1us                # default; the shortest pulse your bulb shows
```

See `examples/esp8266_stretch_pwm.yaml`.

## Development

```sh
tests/run.sh                          # logic tests, need only a C++ compiler
esphome compile examples/bp5758d.yaml # a real build
```

## License

MIT
