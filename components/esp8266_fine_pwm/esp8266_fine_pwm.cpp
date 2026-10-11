#ifdef USE_ESP8266

#include "esp8266_fine_pwm.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/macros.h"

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
                "  Frequency: %.1f Hz\n"
                "  Steps per period: %.0f",
                this->frequency_, clockCyclesPerMicrosecond() * 1e6f / this->frequency_);
  LOG_PIN("  Pin: ", this->pin_);
  LOG_FLOAT_OUTPUT(this);
}

void HOT ESP8266FinePWM::write_state(float state) {
  this->last_output_ = state;
  if (this->pin_->is_inverted())
    state = 1.0f - state;

  // As esp8266_pwm, but in CPU cycles: same waveform generator, 80 or 160
  // times finer steps.
  const auto period = static_cast<uint32_t>(roundf(clockCyclesPerMicrosecond() * 1e6f / this->frequency_));
  const auto high = static_cast<uint32_t>(roundf(period * state));
  const uint32_t low = period - high;

  if (high == 0) {
    // esp8266_pwm's fix for servos: their pulse is at most 2.4 ms; let it end
    // before stopping at 50-300 Hz.
    if (this->pin_->digital_read() && 50 <= this->frequency_ && this->frequency_ <= 300)
      delay(3);
    stopWaveform(this->pin_->get_pin());
    this->pin_->digital_write(this->pin_->is_inverted());
  } else if (low == 0) {
    stopWaveform(this->pin_->get_pin());
    this->pin_->digital_write(!this->pin_->is_inverted());
  } else {
    startWaveformClockCycles(this->pin_->get_pin(), high, low, 0);
  }
}

}  // namespace esphome::esp8266_fine_pwm

#endif
