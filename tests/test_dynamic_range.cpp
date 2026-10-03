// Host test for the dynamic_range logic, against fake chips that behave like
// the ESPHome drivers: the level is truncated to the chip's bits, and the
// chip is only marked for sending when a level changes.
//
// Groups register themselves for set_enabled() and live forever on the
// device, so the tests allocate them, and everything they point to, with new
// and never free them.
//
//   c++ -std=c++17 -I stubs -I ../components tests/test_dynamic_range.cpp \
//       ../components/dynamic_range/dynamic_range.cpp && ./a.out
// (or just run tests/run.sh)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>
#include "dynamic_range/dynamic_range.h"

using namespace esphome;
using dynamic_range::CurrentGroup;
using dynamic_range::DynamicRangeOutput;

static int failures = 0;
#define CHECK(cond, ...)                 \
  do {                                   \
    if (!(cond)) {                       \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);               \
      printf("\n");                      \
      failures++;                        \
    }                                    \
  } while (0)

// One fake chip: a current index per group and a "send" flag.
struct FakeChip {
  int current{-1};
  bool dirty{false};
};
static FakeChip chip;
static void apply(uint8_t i) { chip.current = i; }

struct FakeChannel : output::FloatOutput {
  explicit FakeChannel(int bits) : max_amount((1 << bits) - 1) {}
  int max_amount;
  int amount{0};
  void write_state(float state) override {
    int a = static_cast<int>(state * this->max_amount);  // truncates, like the drivers
    if (a != this->amount)
      chip.dirty = true;
    this->amount = a;
  }
};

struct Model {
  const char *name;
  uint8_t min_index, max_index, offset;
  int bits;
};

// Run the main loop until the group has reached its target current, as
// ESPHome would over the next passes. Checks that while anything is lit, each
// pass moves the current by at most one step.
static int max_lit_jump = 0;
static void settle(CurrentGroup &group, FakeChannel &ch) {
  for (int i = 0; i < 1000 && group.loop_enabled_; i++) {
    int before = chip.current;
    group.loop();
    if (ch.amount > 0)
      max_lit_jump = std::max(max_lit_jump, std::abs(chip.current - before));
  }
}

// What the light actually gets, as a fraction of full: current x level.
static float emitted(const Model &m, const FakeChannel &ch) {
  return float(chip.current + m.offset) / float(m.max_index + m.offset) * ch.amount / float(ch.max_amount);
}

// BP5758D: must give exactly the same output as the original 2022 code, for
// every 10-bit input.
static void test_bp5758d_matches_2022() {
  for (int max_ma : {16, 26, 90}) {
    chip = {};
    FakeChannel &ch = *new FakeChannel(10);
    CurrentGroup &group = *new CurrentGroup(1, max_ma, 0, apply);
    DynamicRangeOutput &out = *new DynamicRangeOutput();
    out.set_inner(&ch);
    out.set_group(&group);
    for (int i = 1; i <= 1023; i++) {
      float s = i / 1023.0f;
      out.set_level(s);
      settle(group, ch);
      float ma = s * max_ma;
      int want_current = clamp<int>(int(ma / 0.5f), 1, max_ma);
      int want_amount = int((ma / want_current) * 1023);
      CHECK(chip.current == want_current && ch.amount == want_amount,
            "bp5758d %d mA step %d: got %d mA/%d, want %d mA/%d", max_ma, i, chip.current, ch.amount,
            want_current, want_amount);
    }
  }
}

// Every chip: brightness stays close to the request, there are many more
// distinct steps below 10 % than with a fixed current, and a current change
// is always sent.
static void test_smooth_and_sent(const Model &m) {
  chip = {};
  FakeChannel &ch = *new FakeChannel(m.bits);
  CurrentGroup &group = *new CurrentGroup(m.min_index, m.max_index, m.offset, apply);
  DynamicRangeOutput &out = *new DynamicRangeOutput();
  out.set_inner(&ch);
  out.set_group(&group);

  std::set<long> steps;  // distinct non-zero outputs below 10 %
  float lowest = 1;
  int last_current = -1;
  const int n = 200000;
  for (int i = 1; i <= n; i++) {
    float s = float(i) / n;
    chip.dirty = false;
    out.set_level(s);
    if (chip.current != last_current)
      CHECK(chip.dirty, "%s: current changed to %d but nothing marked for sending", m.name, chip.current);
    last_current = chip.current;
    for (int pass = 0; pass < 1000 && group.loop_enabled_; pass++) {
      chip.dirty = false;
      group.loop();
      if (chip.current != last_current)
        CHECK(chip.dirty, "%s: current changed to %d but nothing marked for sending", m.name, chip.current);
      last_current = chip.current;
    }

    float e = emitted(m, ch);
    // Off by at most one level step at the chosen current.
    float step = float(chip.current + m.offset) / float(m.max_index + m.offset) / ch.max_amount;
    CHECK(std::fabs(e - s) <= step * 1.001f, "%s: asked %.5f, got %.5f", m.name, s, e);
    if (e > 0 && e < 0.1f) {
      steps.insert(std::lround(e * 1e9));
      lowest = std::min(lowest, e);
    }
  }
  int fixed_steps = int(0.1f * ch.max_amount);  // fixed current: k / max for k = 1, 2, ... below 10 %
  printf("%-28s steps below 10%%: %5zu (fixed current: %3d)   lowest: %.4f %% (fixed: %.4f %%)\n", m.name,
         steps.size(), fixed_steps, lowest * 100, 100.0f / ch.max_amount);
  CHECK(int(steps.size()) > fixed_steps, "%s: not more steps than a fixed current", m.name);

  out.set_level(0);
  CHECK(ch.amount == 0, "%s: off should be level 0", m.name);
}

// Two whites on one shared current: the brighter one decides the current,
// and both still get their own brightness.
static void test_shared_group() {
  Model m{"sm2235 white", 0, 4, 1, 10};
  chip = {};
  FakeChannel &warm_ch = *new FakeChannel(10), &cold_ch = *new FakeChannel(10);
  CurrentGroup &group = *new CurrentGroup(m.min_index, m.max_index, m.offset, apply);
  DynamicRangeOutput &warm = *new DynamicRangeOutput(), &cold = *new DynamicRangeOutput();
  warm.set_inner(&warm_ch);
  warm.set_group(&group);
  cold.set_inner(&cold_ch);
  cold.set_group(&group);

  warm.set_level(0.30f);
  cold.set_level(0.05f);
  settle(group, warm_ch);
  CHECK(std::fabs(emitted(m, warm_ch) - 0.30f) < 0.005f, "warm %.4f", emitted(m, warm_ch));
  CHECK(std::fabs(emitted(m, cold_ch) - 0.05f) < 0.005f, "cold %.4f", emitted(m, cold_ch));

  // Turning the brighter one down must lower the shared current and rescale
  // the other one, not leave it at the old ratio.
  warm.set_level(0.0f);
  settle(group, cold_ch);
  CHECK(warm_ch.amount == 0, "warm should be off");
  CHECK(std::fabs(emitted(m, cold_ch) - 0.05f) < 0.005f, "cold after warm off %.4f", emitted(m, cold_ch));
  CHECK(chip.current < m.max_index, "current should have dropped, is %d", chip.current);
}

// Switching dynamic range off and on while lit: the current walks one step
// per loop pass instead of jumping (a jump flashes on the real chip), and
// lands where it should.
static void test_switching_walks() {
  Model m{"bp5758d", 1, 16, 0, 10};
  chip = {};
  FakeChannel &ch = *new FakeChannel(10);
  CurrentGroup &group = *new CurrentGroup(m.min_index, m.max_index, m.offset, apply);
  DynamicRangeOutput &out = *new DynamicRangeOutput();
  out.set_inner(&ch);
  out.set_group(&group);

  out.set_level(0.02f);  // from dark: may jump straight there
  CHECK(chip.current == 1 && ch.amount > 0, "on from dark: %d mA", chip.current);

  max_lit_jump = 0;
  dynamic_range::set_enabled(false);
  CHECK(chip.current == 1, "switching must not jump at once, is %d mA", chip.current);
  group.loop();
  CHECK(chip.current == 2, "first pass of switching off should be one step, is %d mA", chip.current);
  settle(group, ch);
  CHECK(chip.current == 16 && ch.amount == int(0.02f * 1023), "disabled: %d mA/%d", chip.current, ch.amount);

  dynamic_range::set_enabled(true);
  settle(group, ch);
  CHECK(chip.current == 1, "enabled again: %d mA", chip.current);
  CHECK(max_lit_jump <= 1, "current jumped %d steps while lit", max_lit_jump);

  // A jump up in brightness: the current walks up too, then lands right.
  max_lit_jump = 0;
  out.set_level(0.9f);
  settle(group, ch);
  CHECK(max_lit_jump <= 1, "current jumped %d steps while lit", max_lit_jump);
  CHECK(std::fabs(emitted(m, ch) - 0.9f) < 0.002f, "after walking up: %.4f", emitted(m, ch));
}

int main() {
  test_bp5758d_matches_2022();
  // min index, max index (as configured), offset, level bits
  test_smooth_and_sent({"bp5758d 16 mA", 1, 16, 0, 10});
  test_smooth_and_sent({"bp5758d 90 mA", 1, 90, 0, 10});
  test_smooth_and_sent({"sm2235/sm2335 color, max 15", 0, 15, 1, 10});
  test_smooth_and_sent({"sm2235/sm2335 white, max 4", 0, 4, 1, 10});
  test_smooth_and_sent({"bp1658cj color, max 15", 1, 15, 0, 10});
  test_smooth_and_sent({"bp1658cj white, max 6", 1, 6, 0, 10});
  test_smooth_and_sent({"sm2135 color 45 mA", 0, 7, 2, 8});
  test_smooth_and_sent({"sm2135 white 60 mA", 0, 10, 2, 8});
  test_shared_group();
  test_switching_walks();
  printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
