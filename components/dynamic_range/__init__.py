"""Smooth low-end dimming for LED driver chips with a current setting.

Wraps the channel outputs of bp5758d, bp1658cj, sm2235, sm2335 and sm2135.
Lowers the chip's current setting when dim and raises the level to match, so
the level keeps most of its steps. See README.md.
"""

CODEOWNERS = ["@dbuezas"]
