#pragma once

#include <cstdint>
#include <vector>
#include "esphome/components/output/float_output.h"
#include "esphome/core/component.h"

namespace esphome::dynamic_range {

class CurrentGroup;

// Switch every dynamic_range output between dynamic and fixed current, and
// re-apply at once. For comparing the two, e.g. from a template switch.
void set_enabled(bool enabled);
bool is_enabled();

// Sits between a light and one channel of an LED driver chip. The light talks
// to this output; this output sets both the chip's current setting and the
// channel's level, see CurrentGroup.
class DynamicRangeOutput : public output::FloatOutput {
 public:
  void set_inner(output::FloatOutput *inner) { this->inner_ = inner; }
  void set_group(CurrentGroup *group);

 protected:
  friend class CurrentGroup;
  void write_state(float state) override;

  output::FloatOutput *inner_{nullptr};
  CurrentGroup *group_{nullptr};
  float wanted_{0.0f};
};

// The channels that share one current setting on the chip: a single channel
// on the BP5758D, the three colour channels or the two whites on the others.
//
// Each channel has two settings: a coarse one (the current setting) and a
// fine one (the 10-bit level, 8-bit on the SM2135). The light output follows
// their product. ESPHome sets the current once, so a dim channel only uses
// the bottom of the fine setting: 2 % is about 20 of 1023 steps. Here the
// current follows the brightness instead, and the fine level stays between
// about half and full, where it has hundreds of steps. Each change of current
// is cancelled exactly by the fine level, so brightness never jumps.
//
// The current never jumps while the light is lit: it moves one step per loop
// pass, with the level following each step. The chip applies each byte as it
// arrives, current bytes before level bytes, so a big jump shows the new
// current with the old level for a moment - a visible flash or dip, e.g. when
// switching dynamic range on or off. One step at a time is no worse than
// normal dimming. While the group is dark the current may jump at once:
// nothing is lit, so nothing can flash.
//
// Current settings are indexes into the chip's table. Every supported chip has
// a linear table, current = (index + offset) x step, so only index + offset
// matters here, never the mA.
class CurrentGroup : public Component {
 public:
  using ApplyFn = void (*)(uint8_t index);

  CurrentGroup(uint8_t min_index, uint8_t max_index, uint8_t offset, ApplyFn apply);

  void add(DynamicRangeOutput *output) { this->members_.push_back(output); }

  // Pick the target current and recompute every member's level. Called after
  // any member changes. The light sets its channels one after another, and
  // the chip only sends in its own loop(), so what goes out is the state
  // after the last member's call.
  void update();

  // One current step towards the target per pass, until it is reached.
  void loop() override;

 protected:
  uint8_t pick_(float needed) const;
  float rel_(uint8_t index) const { return index + this->offset_; }
  void write_();

  std::vector<DynamicRangeOutput *> members_;
  uint8_t min_index_;
  uint8_t max_index_;
  uint8_t offset_;
  ApplyFn apply_;
  uint8_t index_;   // current setting now
  uint8_t target_;  // where it is going
  int sent_index_{-1};
  bool dark_{true};
};

}  // namespace esphome::dynamic_range
