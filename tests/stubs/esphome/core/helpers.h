// Host-test stand-in for ESPHome's helpers.h: only what dynamic_range uses.
#pragma once
#include <algorithm>
namespace esphome {
template<typename T> T clamp(T v, T lo, T hi) { return std::min(std::max(v, lo), hi); }
}  // namespace esphome
