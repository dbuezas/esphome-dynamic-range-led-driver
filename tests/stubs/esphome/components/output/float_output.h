// Host-test stand-in for ESPHome's FloatOutput: set_level clamps and calls
// write_state, as the real one does with default min/max power.
#pragma once
#include "esphome/core/helpers.h"
namespace esphome::output {
class FloatOutput {
 public:
  virtual ~FloatOutput() = default;
  void set_level(float state) { this->write_state(clamp(state, 0.0f, 1.0f)); }
 protected:
  virtual void write_state(float state) = 0;
};
}  // namespace esphome::output
