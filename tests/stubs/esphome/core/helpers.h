// Host-test stand-in for ESPHome's helpers.h: only what dynamic_range uses.
#pragma once
#include <algorithm>
#include <cstdint>
namespace esphome {
template<typename T> T clamp(T v, T lo, T hi) { return std::min(std::max(v, lo), hi); }

// Counts requests, so a test can check that the fast loop is released again.
class HighFrequencyLoopRequester {
 public:
  void start() {
    if (!this->started_)
      num_requests++;
    this->started_ = true;
  }
  void stop() {
    if (this->started_)
      num_requests--;
    this->started_ = false;
  }
  static bool is_high_frequency() { return num_requests > 0; }
  static inline int num_requests = 0;

 protected:
  bool started_{false};
};
}  // namespace esphome
