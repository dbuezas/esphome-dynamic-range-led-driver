// Host-test stand-in for ESPHome's Component: loop control only. The test
// calls loop() itself, as often as the real main loop would.
#pragma once
namespace esphome {
class Component {
 public:
  virtual ~Component() = default;
  virtual void loop() {}
  void enable_loop() { this->loop_enabled_ = true; }
  void disable_loop() { this->loop_enabled_ = false; }
  bool loop_enabled_{true};
};
}  // namespace esphome
