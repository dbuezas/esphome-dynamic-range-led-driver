#pragma once

#ifdef USE_ESP8266

#include "esphome/components/output/float_output.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"

namespace esphome::esp8266_fine_pwm {

// ESPHome's esp8266_pwm, with the pulse timed in CPU cycles instead of whole
// microseconds. At 1 kHz that is 80000 steps per period (160000 at 160 MHz)
// instead of 1000. It matters at the dim end: at 2 % the pulse is 20 us, so a
// 1 us step is a 5 % jump in brightness; a cycle step is under 0.1 %.
//
// The waveform generator (or the LED) cannot make pulses much shorter than
// about 1 us: a shorter request still gives about 1 us, so the light would
// come on early and too bright. Below min_pulse, the pulse stays at min_pulse
// and the period gets longer instead, down to min_frequency. At 1 kHz and
// 250 Hz that makes the lowest brightness 4 times lower, smoothly.
class ESP8266FinePWM final : public output::FloatOutput, public Component {
 public:
  void set_pin(InternalGPIOPin *pin) { this->pin_ = pin; }
  void set_frequency(float frequency) { this->frequency_ = frequency; }
  void set_min_pulse(float us) { this->min_pulse_us_ = us; }
  void set_min_frequency(float frequency) { this->min_frequency_ = frequency; }
  void set_fine_steps(bool fine) { this->fine_steps_ = fine; }
  void update_frequency(float frequency) override {
    this->set_frequency(frequency);
    this->write_state(this->last_output_);
  }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

 protected:
  void write_state(float state) override;

  InternalGPIOPin *pin_;
  float frequency_{1000.0};  // keep in sync with DEFAULT_FREQUENCY in output.py
  float last_output_{0.0};   // for update_frequency()
  float min_pulse_us_{1.0};     // keep in sync with output.py
  float min_frequency_{250.0};  // keep in sync with output.py
  bool fine_steps_{true};       // CPU-cycle steps; false: whole microseconds
};

template<typename... Ts> class SetFrequencyAction final : public Action<Ts...> {
 public:
  SetFrequencyAction(ESP8266FinePWM *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(float, frequency);

  void play(const Ts &...x) { this->parent_->update_frequency(this->frequency_.value(x...)); }

  ESP8266FinePWM *parent_;
};

}  // namespace esphome::esp8266_fine_pwm

#endif
