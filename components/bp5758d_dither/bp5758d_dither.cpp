#include "bp5758d_dither.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#if __has_include("esphome/components/bp5758d/bp5758d.h")
#define DITHER_CHIP
#include "esphome/components/bp5758d/bp5758d.h"
#endif

namespace esphome::bp5758d_dither {

// The level's highest code: 10 bits.
static const uint16_t MAX_CODE = 1023;
// At most one frame per ms. A direct write is 3 bytes, about 0.6 ms on a
// BK7231N, and only goes out when the code changes.
static const uint32_t DITHER_FRAME_US = 1000;
// Above this many codes one code is under 0.4 %: not visible, not worth the CPU.
static const float DITHER_BELOW = 256.0f;
// The pattern may not repeat slower than this, see set_min_flicker_hz()...
static float min_flicker_hz_ = 40.0f;
// ...unless its ripple is this small compared with the brightness.
static const float MAX_SLOW_RIPPLE = 0.01f;

static const char *const TAG = "bp5758d_dither";

static bool enabled_ = false;  // off at boot, see set_enabled()
static uint8_t dithering_count_ = 0;  // outputs dithering now
// Counts the driver's full writes (as far as these outputs see them). One
// may put a dithering channel back on the driver's copy of its code.
static uint32_t driver_writes_ = 0;
// Diagnostics while dithering, logged once per second in verbose builds
// (logger level VERBOSE): are the frames getting out, and what do the writes
// and the log line cost?
static struct {
  uint32_t passes, frames, direct_writes, direct_us, direct_max_us, full_sends, gap_max_us;
  uint32_t last_pass_us, last_log_us;
} stats_{};

static std::vector<DitherOutput *> &all_outputs() {
  static std::vector<DitherOutput *> outputs;
  return outputs;
}

void set_enabled(bool enabled) {
  ESP_LOGD(TAG, "Dithering %s", enabled ? "on" : "off");
  enabled_ = enabled;
  for (auto *o : all_outputs())
    o->write_state(o->state_);
}

bool is_enabled() { return enabled_; }

void set_min_flicker_hz(float hz) {
  ESP_LOGD(TAG, "Slowest pattern: %.0f Hz", hz);
  min_flicker_hz_ = std::max(hz, 1.0f);
  for (auto *o : all_outputs())
    o->write_state(o->state_);
}

DitherOutput::DitherOutput() { all_outputs().push_back(this); }

float DitherOutput::target_(float state, bool *ours, bool *dither) const {
  const float wanted = state * MAX_CODE;
  *ours = enabled_ && wanted > 0 && wanted < DITHER_BELOW;
  *dither = false;
  if (!*ours)
    return wanted;
  // The smallest share of 1 code whose pattern still repeats fast enough. A
  // channel that is on never goes below it.
  const float frame_hz = 1e6f / (DITHER_FRAME_US * this->frames_per_frame_);
  const float smallest = min_flicker_hz_ / frame_hz;
  const float code = smallest < 0.5f ? std::max(wanted, smallest) : wanted;
  // How fast the pattern repeats: the rarer of the two codes comes once in
  // 1 / share frames. Its ripple is about 2 x share codes.
  const float frac = code - static_cast<int>(code);
  const float share = std::min(frac, 1.0f - frac);
  *dither = share > 0.001f && (share >= smallest * 0.999f || 2 * share <= MAX_SLOW_RIPPLE * code);
  return code;
}

float DitherOutput::driver_level_(float state) const {
  bool ours, dither;
  const float code = this->target_(state, &ours, &dither);
  if (!ours)
    return state;
  // Never 0 while dithering: at 0 the driver turns the channel's current
  // off, and with all channels at 0 it puts the chip to sleep; a direct write
  // undoes neither.
  const int fixed = static_cast<int>(std::lround(code));
  if (fixed == 0 && !dither)
    return 0.0f;
  return (std::max(fixed, 1) + 0.5f) / MAX_CODE;  // the middle: the driver truncates
}

void DitherOutput::write_state(float state) {
  this->state_ = state;
  this->inner_->set_level(this->driver_level_(state));
  if (this->pending_ != nullptr && this->pending_())
    driver_writes_++;
  bool ours, dither;
  this->target_(state, &ours, &dither);
  // Only ever starts here: this may be a throw-away level. The frames see
  // the level the pass ends with, and stop when there is nothing to dither.
  if (dither)
    this->set_active_(true);
}

void DitherOutput::loop() {
  const uint32_t now = micros();
  if (this->all_off_ != nullptr) {
    // The chip wakes: the levels were all 0 at the last pass, now they are not.
    const bool off = this->all_off_();
    if (this->was_off_ && !off)
      this->resend_after_wake_ = true;
    this->was_off_ = off;
    if (this->resend_after_wake_ && !this->pending_()) {
      // The driver has sent the waking write (in its loop, since the last
      // pass): send the same once more.
      this->resend_after_wake_ = false;
      this->resend_();
      driver_writes_++;
    }
  }
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  // Diagnostics only in verbose builds: logging itself stalls the loop.
  if (this->dithering_)
    this->log_stats_(now);
#endif
  if (this->dithering_)
    this->dither_frame_();
  (void) now;
}

void DitherOutput::dither_frame_() {
  const uint32_t frame = micros() / DITHER_FRAME_US;
  if (frame == this->frame_index_)
    return;
  // Frames since the last one: 1 if they keep up. Capped, for the first
  // frame and the micros() wrap.
  const uint32_t frames = clamp<uint32_t>(frame - this->frame_index_, 1, 20);
  this->frame_index_ = frame;
  this->frames_per_frame_ += (frames - this->frames_per_frame_) * 0.05f;
  stats_.frames++;
  this->frame_(frames);
}

void DitherOutput::frame_(uint32_t frames) {
  bool ours, dither;
  const float code = this->target_(this->state_, &ours, &dither);
  if (!dither) {
    // Done: the chip should show what the driver holds. A 0 the driver sends
    // itself (it was given one, so it has a write pending or done).
    const int held = static_cast<int>(this->driver_level_(this->state_) * MAX_CODE);
    if (held != 0 && (this->on_chip_ != held || this->seen_writes_ != driver_writes_))
      this->write_direct_(held);
    this->sent_ = -1;
    this->error_ = 0;
    this->set_active_(false);
    return;
  }

  const int low = static_cast<int>(code);
  if (this->sent_ != low && this->sent_ != low + 1) {
    // Not dithering this code yet: start fresh.
    this->error_ = 0;
    this->sent_ = static_cast<int16_t>(std::lround(code));
  }
  // Book what the last frames gave, then pick the code that brings the owed
  // light closest to zero over the next frame.
  this->error_ = clamp(this->error_ + (code - this->sent_) * frames, -2.0f, 2.0f);
  this->sent_ = this->error_ + code - low >= 0.5f ? low + 1 : low;
  // A full write from the driver since the last direct write sent the
  // driver's copy instead (the nearest code, at least 1): write again.
  if (this->sent_ != this->on_chip_ || this->seen_writes_ != driver_writes_)
    this->write_direct_(this->sent_);
}

void DitherOutput::set_active_(bool active) {
  if (active == this->dithering_)
    return;
  this->dithering_ = active;
  if (active) {
    if (dithering_count_++ == 0)
      stats_.last_pass_us = 0;  // the loop was not watched until now
    this->frame_index_ = micros() / DITHER_FRAME_US;
    // Run the main loop continuously instead of at about 60 Hz, so frames
    // can go out as often as DITHER_FRAME_US allows.
    this->high_freq_.start();
  } else {
    dithering_count_--;
    this->high_freq_.stop();
  }
}

void DitherOutput::write_direct_(int code) {
  const uint32_t start = micros();
  this->direct_(code);
  const uint32_t took = micros() - start;
  stats_.direct_writes++;
  stats_.direct_us += took;
  stats_.direct_max_us = std::max(stats_.direct_max_us, took);
  this->on_chip_ = code;
  this->seen_writes_ = driver_writes_;
}

void DitherOutput::log_stats_(uint32_t now) {
  // Once per pass, from whichever dithering output runs first in it: the
  // others see the same time (to the microsecond, close enough).
  if (now == stats_.last_pass_us)
    return;
  // last_pass_us is 0 after a time without any dithering: no gap then.
  if (stats_.last_pass_us != 0)
    stats_.gap_max_us = std::max(stats_.gap_max_us, now - stats_.last_pass_us);
  stats_.last_pass_us = now;
  stats_.passes++;
  if (this->pending_ != nullptr && this->pending_())
    stats_.full_sends++;  // a pass that starts with a driver write pending: about one full write each
  if (now - stats_.last_log_us < 1000000)
    return;
  static uint32_t log_us = 0;  // how long the last of these log lines took
  if (stats_.last_log_us != 0) {
    ESP_LOGV(TAG,
             "%u loop passes/s, %u frames/s (all outputs), longest pass gap %u us, %u direct writes/s (avg %u us, "
             "max %u us), ~%u full driver writes/s, last log line %u us",
             stats_.passes, stats_.frames, stats_.gap_max_us, stats_.direct_writes,
             stats_.direct_writes ? stats_.direct_us / stats_.direct_writes : 0, stats_.direct_max_us,
             stats_.full_sends, log_us);
  }
  // The log line is not a gap of the loop's own: start the next second after it.
  const uint32_t after = micros();
  log_us = after - now;
  stats_ = {};
  stats_.last_pass_us = after;
  stats_.last_log_us = after;
}

#ifdef DITHER_CHIP
// The driver keeps its pins and level copy protected, and the class is final.
// An explicit template instantiation is exempt from access checks (C++
// [temp.spec]), so it can hand out member pointers to them.
template<typename Tag, typename Tag::type M> struct Expose {
  friend typename Tag::type get(Tag) { return M; }
};
struct DataPin {
  using type = GPIOPin *bp5758d::BP5758D::*;
  friend type get(DataPin);
};
struct ClockPin {
  using type = GPIOPin *bp5758d::BP5758D::*;
  friend type get(ClockPin);
};
struct PwmAmounts {
  using type = std::vector<uint16_t> bp5758d::BP5758D::*;
  friend type get(PwmAmounts);
};
struct Update {
  using type = bool bp5758d::BP5758D::*;
  friend type get(Update);
};
template struct Expose<DataPin, &bp5758d::BP5758D::data_pin_>;
template struct Expose<ClockPin, &bp5758d::BP5758D::clock_pin_>;
template struct Expose<PwmAmounts, &bp5758d::BP5758D::pwm_amounts_>;
template struct Expose<Update, &bp5758d::BP5758D::update_>;

bool bp5758d_send_pending(bp5758d::BP5758D *chip) { return chip->*get(Update()); }

bool bp5758d_all_off(bp5758d::BP5758D *chip) {
  for (uint16_t level : chip->*get(PwmAmounts()))
    if (level > 0)
      return false;
  return true;
}

void bp5758d_resend(bp5758d::BP5758D *chip) { chip->*get(Update()) = true; }

// The same bus timing as the driver (bp5758d.cpp).
static const uint8_t BP5758D_DELAY = 2;

static void bp5758d_byte(GPIOPin *data, GPIOPin *clock, uint8_t byte) {
  for (uint8_t mask = 0x80; mask; mask >>= 1) {
    data->digital_write(byte & mask);
    delayMicroseconds(BP5758D_DELAY);
    clock->digital_write(true);
    delayMicroseconds(BP5758D_DELAY);
    clock->digital_write(false);
    delayMicroseconds(BP5758D_DELAY);
  }
  // ack bit
  data->pin_mode(gpio::FLAG_INPUT);
  clock->digital_write(true);
  delayMicroseconds(BP5758D_DELAY);
  clock->digital_write(false);
  delayMicroseconds(BP5758D_DELAY);
  data->pin_mode(gpio::FLAG_OUTPUT);
}

void bp5758d_write(bp5758d::BP5758D *chip, uint8_t channel, uint16_t code) {
  // The driver's copy of the level, so its next full write sends the same.
  // Not changed through the driver's setter, which would make it send now.
  // Never 0, see DitherOutput::driver_level_().
  (chip->*get(PwmAmounts()))[channel] = std::max<uint16_t>(code, 1);

  GPIOPin *data = chip->*get(DataPin());
  GPIOPin *clock = chip->*get(ClockPin());
  // Byte 0: model id, channel mode, start address. The mode as the driver's
  // own full write would pick it from the same levels: colours only, whites
  // only, or all 5. Addresses 0-5 are the channel enables and currents; the
  // levels start at 6, 2 bytes per channel, 5 bits each, low bits first (as
  // the driver sends them).
  const auto &pwm = chip->*get(PwmAmounts());
  const bool rgb = pwm[0] > 0 || pwm[1] > 0 || pwm[2] > 0;
  const bool white = pwm[3] > 0 || pwm[4] > 0;
  const uint8_t mode = rgb && !white ? 0b00010000 : white && !rgb ? 0b00100000 : 0b00110000;
  const uint8_t bytes[3] = {
      static_cast<uint8_t>(0b10000000 | mode | (6 + 2 * channel)),
      static_cast<uint8_t>(code & 0x1F),
      static_cast<uint8_t>((code >> 5) & 0x1F),
  };
  data->digital_write(false);
  delayMicroseconds(BP5758D_DELAY);
  clock->digital_write(false);
  delayMicroseconds(BP5758D_DELAY);
  for (uint8_t b : bytes)
    bp5758d_byte(data, clock, b);
  clock->digital_write(true);
  delayMicroseconds(BP5758D_DELAY);
  data->digital_write(true);
  delayMicroseconds(BP5758D_DELAY);
}
#endif

}  // namespace esphome::bp5758d_dither
