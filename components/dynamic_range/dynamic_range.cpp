#include "dynamic_range.h"
#include <algorithm>
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::dynamic_range {

// The level to aim for. Half way leaves room both ways, so every current step
// can be cancelled exactly by the level, and the level never has to work in
// its coarse bottom end.
static const float TARGET_LEVEL = 0.5f;

static const char *const TAG = "dynamic_range";

static bool enabled_ = true;
static std::vector<CurrentGroup *> &all_groups() {
  static std::vector<CurrentGroup *> groups;
  return groups;
}

void set_enabled(bool enabled) {
  ESP_LOGD(TAG, "%s", enabled ? "Enabled" : "Disabled (fixed current)");
  enabled_ = enabled;
  for (auto *g : all_groups())
    g->update();
}

bool is_enabled() { return enabled_; }

CurrentGroup::CurrentGroup(uint8_t min_index, uint8_t max_index, uint8_t offset, ApplyFn apply)
    : min_index_(min_index),
      max_index_(max_index),
      offset_(offset),
      apply_(apply),
      index_(max_index),
      target_(max_index) {
  all_groups().push_back(this);
}

void DynamicRangeOutput::set_group(CurrentGroup *group) {
  this->group_ = group;
  group->add(this);
}

void DynamicRangeOutput::write_state(float state) {
  this->wanted_ = state;
  this->group_->update();
}

uint8_t CurrentGroup::pick_(float needed) const {
  // The highest current that still keeps the level at TARGET_LEVEL or above...
  int index = clamp<int>(static_cast<int>(needed / TARGET_LEVEL) - this->offset_, this->min_index_, this->max_index_);
  // ...but never one so low that the level would have to go above full.
  while (index < this->max_index_ && this->rel_(index) < needed)
    index++;
  return index;
}

void CurrentGroup::update() {
  float wanted = 0.0f;
  for (auto *m : this->members_)
    wanted = std::max(wanted, m->wanted_);
  const bool was_dark = this->dark_;
  this->dark_ = wanted == 0.0f;

  if (this->dark_) {
    // Off: leave the current where it is. The driver sends 0 mA for a
    // channel at level 0 anyway.
    this->target_ = this->index_;
  } else if (enabled_) {
    this->target_ = this->pick_(wanted * this->rel_(this->max_index_));
  } else {
    this->target_ = this->max_index_;
  }
  if (was_dark)
    this->index_ = this->target_;  // nothing was lit, so a jump cannot show

  this->write_();
  if (this->index_ != this->target_) {
    ESP_LOGV(TAG, "Current %u -> %u", this->index_, this->target_);
    this->enable_loop();
  }
}

void CurrentGroup::loop() {
  if (this->index_ == this->target_) {
    this->disable_loop();
    return;
  }
  this->index_ += this->target_ > this->index_ ? 1 : -1;
  this->write_();
  if (this->index_ == this->target_)
    ESP_LOGV(TAG, "Current reached %u", this->index_);
}

void CurrentGroup::write_() {
  const float full = this->rel_(this->max_index_);
  const float current = this->rel_(this->index_);

  if (this->index_ != this->sent_index_) {
    this->apply_(this->index_);
    this->sent_index_ = this->index_;
    // The drivers only send to the chip when a level changes; a new current
    // alone is never sent. Here the current often changes while the levels
    // stay put, so write a throw-away level first to mark the change. Both
    // writes land before the driver's next loop(), so only the real level
    // ever reaches the chip.
    if (!this->members_.empty()) {
      auto *m = this->members_.front();
      m->inner_->set_level(m->wanted_ * full / current > 0.5f ? 0.0f : 1.0f);
    }
  }

  // (wanted x full) / current, in this order: the same rounding as the
  // original 2022 code, so a BP5758D lamp gets exactly the same output.
  // While the current is still climbing towards its target, the level can
  // hit full; the light is then a little dimmer until the current catches up.
  for (auto *m : this->members_)
    m->inner_->set_level(std::min(m->wanted_ * full / current, 1.0f));
}

}  // namespace esphome::dynamic_range
