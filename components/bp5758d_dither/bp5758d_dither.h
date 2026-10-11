#pragma once

#include <cstdint>
#include "esphome/components/output/float_output.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome::bp5758d {
class BP5758D;
}  // namespace esphome::bp5758d

namespace esphome::bp5758d_dither {

// Write one channel's level straight to a BP5758D: 3 bytes instead of the
// driver's 17. Also puts the code (at least 1) in the driver's copy, without
// making it send. channel: 0-4 (OUT1-OUT5).
void bp5758d_write(bp5758d::BP5758D *chip, uint8_t channel, uint16_t code);
// Whether the driver will send a full write in its next loop().
bool bp5758d_send_pending(bp5758d::BP5758D *chip);
// Whether every level in the driver's copy is 0 (then its next write puts the
// chip to sleep, or it is asleep).
bool bp5758d_all_off(bp5758d::BP5758D *chip);
// Make the driver send a full write in its next loop().
void bp5758d_resend(bp5758d::BP5758D *chip);

// Switch dithering on or off for every bp5758d_dither output, and re-apply at
// once. Off at boot.
void set_enabled(bool enabled);
bool is_enabled();

// One dithering frame for every dithering output that has one due. Called by
// the dithering task (DITHER_TASK), otherwise each output runs its own frames
// from loop().
void dither_frames();

// Sits between a light (or dynamic_range) and one BP5758D/BP5768 channel.
//
// The chip's level has 1023 steps (10 bits). At the dim end each step is a
// big jump: 3 to 4 is 33 % brighter, and below 1 it is off. There the level is
// dithered in time: a wanted 3.4 sends 4 in 40 % of the frames and 3 in the
// others, chosen by error diffusion so the average is exact and the pattern
// repeats as fast as possible. With dynamic_range this happens at its lowest
// current, where the light is dimmest.
//
// A frame goes out at most every DITHER_FRAME_US. The driver always sends
// every register, 17 bytes, about 1 ms; too slow for this. So each frame
// writes only the channel's level, straight to the chip (the BP5758D takes a
// start address in its first byte), and puts the same code in the driver's
// copy, so a full write from the driver sends it too. Dithering only runs
// below DITHER_BELOW codes, where a code is a visible step. When the pattern
// would repeat slower than MIN_FLICKER_HZ, which could be seen, the level is
// rounded to the nearest code instead. Below the smallest share that repeats
// fast enough, a channel that is on stays at that share instead of going off:
// in a colour, one channel going dark first would change the colour.
//
// Every level is passed on to the driver at once, as without this output, so
// the driver sends when it would anyway. dynamic_range relies on that: it
// writes a throw-away level to make the driver send a new current. The
// dithering itself only looks at the level a pass ends with, in its frames,
// so a throw-away level never reaches the chip by a direct write.
//
// On LibreTiny (BK72xx) the main loop waits for the 2 ms system tick after
// every pass, so it gets about 250 frames per second at best. There the frames
// come from a task of their own at the lowest priority instead: it only runs
// while the main loop sleeps, and it holds the scheduler for each frame, so
// the main loop never runs in the middle of one. The two never touch the chip
// or the shared state at the same time. Elsewhere the main loop runs
// continuously while something dithers, and the frames come from loop().
//
// Also, dithering on or off: the chip sleeps while every level is 0, and may
// lose the first write after it wakes (Tasmota wakes it with a separate
// command first). Seen as a too long black, then a too bright start, when a
// light comes up from 0, e.g. in a colour mode change. So after the driver's
// waking write, this has it send once more.
class DitherOutput : public output::FloatOutput, public Component {
 public:
  using DirectFn = void (*)(uint16_t code);
  using PendingFn = bool (*)();
  using AllOffFn = bool (*)();
  using ResendFn = void (*)();

  DitherOutput();

  void set_inner(output::FloatOutput *inner) { this->inner_ = inner; }
  // The chip behind the inner output, see bp5758d_write() and
  // bp5758d_send_pending().
  void set_chip(DirectFn direct, PendingFn pending) {
    this->direct_ = direct;
    this->pending_ = pending;
  }
  // The resend after a wake, see bp5758d_all_off() and bp5758d_resend(). For
  // one output per chip, so the chip gets one resend, not one per channel.
  void set_wake(AllOffFn all_off, ResendFn resend) {
    this->all_off_ = all_off;
    this->resend_ = resend;
  }

  // Resend after a wake, and the frames when there is no dithering task.
  void loop() override;

 protected:
  friend void dither_frames();
  friend void set_enabled(bool enabled);

  void write_state(float state) override;
  // The code to aim for (with a fraction) for a state, whether it is in this
  // output's range (dithering on, dim enough), and whether to dither it.
  float target_(float state, bool *ours, bool *dither) const;
  // The level to give the driver for a state: as is, or the nearest code
  // (at least 1) where this output dithers or rounds.
  float driver_level_(float state) const;
  // One dithering frame. frames: frames since the last one.
  void frame_(uint32_t frames);
  void dither_frame_();
  void set_active_(bool active);
  void write_direct_(int code);
  void log_stats_(uint32_t now);

  output::FloatOutput *inner_{nullptr};
  DirectFn direct_{nullptr};
  PendingFn pending_{nullptr};
  AllOffFn all_off_{nullptr};
  ResendFn resend_{nullptr};

  float state_{0.0f};       // the level asked for last
  float error_{0.0f};       // light owed (+) or given too much (-), in codes x frames
  int16_t sent_{-1};        // code dithering wants on the chip now, -1 if none yet
  int16_t on_chip_{-1};     // code last written straight to the chip, -1 if none
  uint32_t seen_writes_{0};  // driver_writes_ at that time
  bool dithering_{false};
  bool was_off_{true};             // the chip's levels were all 0 at the last pass
  bool resend_after_wake_{false};  // the driver should send once more
  uint32_t frame_index_{0};        // index of the last frame
  float frames_per_frame_{1};      // average frames between two of ours (> 1 if they come late)
  HighFrequencyLoopRequester high_freq_;
};

}  // namespace esphome::bp5758d_dither
