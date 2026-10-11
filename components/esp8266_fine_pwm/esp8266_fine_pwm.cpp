#ifdef USE_ESP8266

#include "esp8266_fine_pwm.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/macros.h"

#include <utility>

#include <core_esp8266_waveform.h>

namespace esphome::esp8266_fine_pwm {

static const char *const TAG = "esp8266_fine_pwm";

void ESP8266FinePWM::setup() {
  this->pin_->setup();
  this->turn_off();
}

void ESP8266FinePWM::dump_config() {
  ESP_LOGCONFIG(TAG,
                "ESP8266 fine PWM:\n"
                "  Frequency: %.1f Hz (down to %.1f Hz at the dim end)\n"
                "  Minimum pulse: %.2f us\n"
                "  Steps per period: %.0f",
                this->frequency_, this->min_frequency_, this->min_pulse_us_,
                clockCyclesPerMicrosecond() * 1e6f / this->frequency_);
  LOG_PIN("  Pin: ", this->pin_);
  LOG_FLOAT_OUTPUT(this);
}

void HOT ESP8266FinePWM::write_state(float state) {
  this->last_output_ = state;

  // As esp8266_pwm, but in CPU cycles: same waveform generator, 80 or 160
  // times finer steps.
  const float per_us = clockCyclesPerMicrosecond();
  uint32_t period = static_cast<uint32_t>(roundf(per_us * 1e6f / this->frequency_));
  uint32_t on = static_cast<uint32_t>(roundf(period * state));
  const auto min_on = static_cast<uint32_t>(roundf(per_us * this->min_pulse_us_));
  if (on > 0 && on < min_on) {
    // Too short to make: keep the shortest pulse and stretch the period, as
    // far as min_frequency allows. Below that, the nearest of the shortest
    // pulse at the longest period, or off.
    const auto longest = static_cast<uint32_t>(roundf(per_us * 1e6f / this->min_frequency_));
    const float stretched = min_on / state;
    if (stretched <= longest) {
      on = min_on;
      period = static_cast<uint32_t>(roundf(stretched));
    } else {
      on = state * longest >= min_on / 2.0f ? min_on : 0;
      period = longest;
    }
  }
  uint32_t off = period - on;
  // An inverted pin is high while the light is off. The waveform generator
  // works on the physical pin level.
  uint32_t high = on, low = off;
  if (this->pin_->is_inverted())
    std::swap(high, low);

  if (high == 0) {
    // esp8266_pwm's fix for servos: their pulse is at most 2.4 ms; let it end
    // before stopping at 50-300 Hz.
    if (this->pin_->digital_read() && 50 <= this->frequency_ && this->frequency_ <= 300)
      delay(3);
    stopWaveform(this->pin_->get_pin());
    this->pin_->digital_write(this->pin_->is_inverted());  // physically low (digital_write inverts)
  } else if (low == 0) {
    stopWaveform(this->pin_->get_pin());
    this->pin_->digital_write(!this->pin_->is_inverted());  // physically high
  } else {
    startWaveformClockCycles(this->pin_->get_pin(), high, low, 0);
  }
}

}  // namespace esphome::esp8266_fine_pwm

#endif
