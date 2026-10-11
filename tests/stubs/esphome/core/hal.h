// Host-test stand-in for ESPHome's hal.h: a clock the test moves by hand.
#pragma once
#include <cstdint>
namespace esphome {
inline uint32_t fake_micros = 0;
inline uint32_t micros() { return fake_micros; }
}  // namespace esphome
