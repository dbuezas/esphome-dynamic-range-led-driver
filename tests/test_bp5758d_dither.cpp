// Host test for bp5758d_dither, below dynamic_range as on a real lamp, against
// a fake BP5758D that behaves like the ESPHome driver: the level is truncated
// to 10 bits, and the driver only sends (every channel) in its loop(), when a
// level changed. A direct write changes one channel on the chip.
//
// Outputs register themselves and live forever on the device, so the tests
// allocate them with new and never free them.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>
#include "bp5758d_dither/bp5758d_dither.h"
#include "dynamic_range/dynamic_range.h"
#include "esphome/core/hal.h"

using namespace esphome;
using bp5758d_dither::DitherOutput;
using dynamic_range::CurrentGroup;
using dynamic_range::DynamicRangeOutput;

static int failures = 0;
#define CHECK(cond, ...)                          \
  do {                                            \
    if (!(cond)) {                                \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
      failures++;                                 \
    }                                             \
  } while (0)

static const uint32_t FRAME_US = 1000;            // DITHER_FRAME_US
static const int FRAMES_PER_FLICKER = 1000 / 50;  // frame rate / the default slowest pattern

// The chip: one current for all channels here (enough for these tests), and
// the driver's "send" flag.
static struct {
  int current{-1};
  bool dirty{false};
} chip;
static void apply(uint8_t i) { chip.current = i; }

// One channel: the driver's copy (amount) and what the chip shows (on_chip).
struct Channel : output::FloatOutput {
  int amount{0};
  int on_chip{0};
  void write_state(float state) override {
    int a = static_cast<int>(state * 1023);  // truncates, like the driver
    if (a != this->amount)
      chip.dirty = true;
    this->amount = a;
  }
};
static std::vector<Channel *> channels;
static int direct_writes = 0, resends = 0;
static std::vector<int> written;  // codes written straight to the chip, for a test to look at

// The driver's loop(): sends everything if anything changed.
static void driver_loop() {
  if (chip.dirty)
    for (auto *c : channels)
      c->on_chip = c->amount;
  chip.dirty = false;
}

// bp5758d_write(): the chip shows the code; the driver's copy is set too
// (at least 1), without sending.
template<int N> static void direct(uint16_t code) {
  channels[N]->on_chip = code;
  channels[N]->amount = std::max<int>(code, 1);
  direct_writes++;
  written.push_back(code);
}
static bool pending() { return chip.dirty; }
static bool all_off() {
  for (auto *c : channels)
    if (c->amount > 0)
      return false;
  return true;
}
static void resend() {
  chip.dirty = true;
  resends++;
}

struct Model {
  const char *name;
  uint8_t min_index, max_index, offset;
};

// light -> dynamic_range -> bp5758d_dither -> channel N
struct Lamp {
  Channel *ch;
  DitherOutput *dither;
  CurrentGroup *group;
  DynamicRangeOutput *out;
};

static void reset() {
  chip = {};
  channels.clear();
}

template<int N> static Lamp make_lamp(const Model &m) {
  Lamp l{new Channel(), new DitherOutput(), new CurrentGroup(m.min_index, m.max_index, m.offset, apply),
         new DynamicRangeOutput()};
  channels.push_back(l.ch);
  l.dither->set_inner(l.ch);
  l.dither->set_chip(direct<N>, pending);
  if (N == 0)
    l.dither->set_wake(all_off, resend);  // one output per chip
  l.out->set_inner(l.dither);
  l.out->set_group(l.group);
  return l;
}

// One main loop pass of these lamps' components, then the driver's.
template<typename... L> static void pass(L &...lamps) {
  ((lamps.group->loop(), lamps.dither->loop()), ...);
  driver_loop();
}

static float shown(const Model &m, const Channel &ch) {
  return float(chip.current + m.offset) / float(m.max_index + m.offset) * ch.on_chip / 1023.0f;
}

struct Run {
  double average;  // mean shown brightness, fraction of full
  int longest;     // longest stretch of one code, in frames
  int codes;       // distinct codes seen
  int highest;     // highest code seen
};

// n main loop passes, one per frame. What the light averages over them.
static Run run_frames(const Model &m, Lamp &l, int n, uint32_t us = FRAME_US) {
  double sum = 0;
  int longest = 0, stretch = 0, last = -1, highest = 0;
  std::set<int> codes;
  for (int i = 0; i < n; i++) {
    esphome::fake_micros += us;
    pass(l);
    sum += shown(m, *l.ch);
    stretch = l.ch->on_chip == last ? stretch + 1 : 1;
    last = l.ch->on_chip;
    longest = std::max(longest, stretch);
    highest = std::max(highest, last);
    codes.insert(l.ch->on_chip);
  }
  return {sum / n, longest, int(codes.size()), highest};
}

// Request s, let the current settle, then average over many frames.
static Run measure(const Model &m, Lamp &l, float s, uint32_t us = FRAME_US) {
  l.out->set_level(s);
  run_frames(m, l, 300, us);
  return run_frames(m, l, 4000, us);
}

// Dithering off: the output passes every level on unchanged.
static void test_off_passes_through() {
  reset();
  bp5758d_dither::set_enabled(false);
  Channel plain;
  DitherOutput *d = new DitherOutput();
  Channel *ch = new Channel();
  channels.push_back(ch);
  d->set_inner(ch);
  d->set_chip(direct<0>, pending);
  for (int i = 0; i <= 4096; i++) {
    float s = i / 4096.0f;
    d->set_level(s);
    plain.set_level(s);
    for (int k = 0; k < 3; k++) {
      esphome::fake_micros += FRAME_US;
      d->loop();
      driver_loop();
    }
    CHECK(ch->amount == plain.amount && ch->on_chip == plain.amount, "off: %.5f gave %d/%d, want %d", s, ch->amount,
          ch->on_chip, plain.amount);
  }
  CHECK(direct_writes == 0, "off: %d direct writes", direct_writes);
  d->set_level(0);
}

// Dim end: with dithering the average follows the request far more closely
// than one code, goes lower, and never flickers slower than allowed. Prints
// the numbers for the README. us: time between main loop passes (frames).
static void test_dither(const Model &m, uint32_t us) {
  const int frames_per_flicker = 1000000 / us / 50;
  reset();
  Lamp l = make_lamp<0>(m);
  // One code at the lowest current, as a fraction of full.
  const float code_step = float(m.min_index + m.offset) / float(m.max_index + m.offset) / 1023;

  struct Result {
    float lowest = 1;      // lowest brightness above off
    float worst_jump = 0;  // largest relative jump between neighbouring requests
  } res[2];
  for (int dither = 0; dither < 2; dither++) {
    bp5758d_dither::set_enabled(dither);
    Result &r = res[dither];
    double prev = 0;
    // A fine sweep from below one code to 40 codes at the lowest current.
    for (int i = 1; i <= 2000; i++) {
      float s = code_step * 40 * i / 2000;
      Run run = measure(m, l, s, us);
      if (run.average > 0)
        r.lowest = std::min<float>(r.lowest, run.average);
      // Jumps from 1 code up: below that, the sweep's own steps are too big.
      if (prev >= code_step * 0.999f)
        r.worst_jump = std::max<float>(r.worst_jump, (run.average - prev) / prev);
      prev = run.average;
      if (!dither)
        continue;
      // Rounded where the pattern would be too slow: off by at most that share.
      float code = s / code_step, got = run.average / code_step;
      CHECK(std::fabs(got - code) <= 1.0f / frames_per_flicker + 0.01f, "%s: asked %.3f codes, got %.3f", m.name,
            code, got);
      // The rule: a slower pattern is fine if its ripple is under 1 %.
      float share = std::min(code - int(code), 1 - (code - int(code)));
      bool small_ripple = 2 * share <= 0.01f * code * 1.001f;
      CHECK(run.longest <= frames_per_flicker + 1 || run.codes == 1 || small_ripple,
            "%s: %.3f codes held one code for %d frames", m.name, code, run.longest);
      CHECK(run.codes <= 2, "%s: %d codes in one pattern", m.name, run.codes);
    }
  }
  printf("%-14s %4u frames/s   lowest: %.5f %% -> %.6f %%   largest jump (1 to 40 codes): %3.0f %% -> %.1f %%\n",
         m.name, 1000000 / us, res[0].lowest * 100, res[1].lowest * 100, res[0].worst_jump * 100,
         res[1].worst_jump * 100);
  // About frames_per_flicker times lower and smoother.
  CHECK(res[1].lowest < res[0].lowest / (frames_per_flicker * 0.9f), "%s: dithering should go much lower", m.name);
  CHECK(res[1].worst_jump < res[0].worst_jump / (frames_per_flicker * 0.7f), "%s: dithering should be much smoother",
        m.name);

  // Few direct writes: about two per pattern, not one per frame.
  direct_writes = 0;
  measure(m, l, 10.5f * code_step, us);
  CHECK(direct_writes <= 4300, "%s: %d direct writes in 4300 frames", m.name, direct_writes);
  direct_writes = 0;
  measure(m, l, 10.02f * code_step, us);
  CHECK(direct_writes < 4300 / 10, "%s: %d direct writes for a rare code", m.name, direct_writes);

  // Off, a whole code, or bright: no dithering, the fast loop is released,
  // and the chip shows exactly what the driver holds.
  for (float s : {0.0f, 3 * code_step, 0.8f, 0.5f * code_step}) {
    measure(m, l, 1.5f * code_step, us);
    measure(m, l, s, us);
    CHECK(esphome::HighFrequencyLoopRequester::num_requests == 0 || s == 0.5f * code_step,
          "%s: fast loop still on at %.6f", m.name, s);
    if (s != 0.5f * code_step)
      CHECK(l.ch->on_chip == l.ch->amount || (l.ch->on_chip == 0 && s == 0), "%s: chip %d, driver %d at %.6f",
            m.name, l.ch->on_chip, l.ch->amount, s);
  }
  // Switching dithering off while dithering: the chip ends on the driver's code.
  measure(m, l, 1.5f * code_step);
  bp5758d_dither::set_enabled(false);
  run_frames(m, l, 50);
  CHECK(l.ch->on_chip == l.ch->amount, "%s: after dithering off: chip %d, driver %d", m.name, l.ch->on_chip,
        l.ch->amount);
  CHECK(esphome::HighFrequencyLoopRequester::num_requests == 0, "%s: fast loop on after dithering off", m.name);
  bp5758d_dither::set_enabled(true);
  l.out->set_level(0);
  run_frames(m, l, 2);
  CHECK(l.ch->amount == 0 && l.ch->on_chip == 0, "%s: off should be level 0", m.name);
}

// While dithering, dynamic_range's current steps write a throw-away level
// (full, or 0) before the real one. It must never reach the chip: no frame
// much brighter than where the light goes, the chip always within one code
// of the driver's copy (which dithering keeps on its code), and every direct
// write too: the chip shows each one for a while, even if another follows in
// the same pass.
static void test_throw_away_level() {
  Model m{"bp5758d 16 mA", 1, 16, 0};
  reset();
  bp5758d_dither::set_enabled(true);
  Lamp l = make_lamp<0>(m);
  const float code_step = 1.0f / 16 / 1023;
  for (float from : {0.3f, 3.3f}) {
    measure(m, l, from * code_step);
    float before = from * code_step;
    // Up to 30 % (the current walks up to 5 mA, one step per pass), and back.
    for (float to : {0.3f, from * code_step}) {
      const float hi = std::max(before, to);
      before = to;
      l.out->set_level(to);
      for (int i = 0; i < 100; i++) {
        esphome::fake_micros += FRAME_US;
        written.clear();
        pass(l);
        for (int code : written)
          CHECK(std::abs(code - l.ch->amount) <= 1, "from %.1f codes to %.4f: frame %d: wrote %d, driver %d", from, to,
                i, code, l.ch->amount);
        const float now = shown(m, *l.ch);
        CHECK(now <= hi * 1.1f + code_step, "from %.1f codes to %.4f: frame %d at %.5f", from, to, i, now);
        CHECK(std::abs(l.ch->on_chip - l.ch->amount) <= 1, "from %.1f codes to %.4f: frame %d: chip %d, driver %d",
              from, to, i, l.ch->on_chip, l.ch->amount);
      }
    }
  }
  l.out->set_level(0);
  run_frames(m, l, 2);
}

// A slow loop (frames come late) must not make the pattern slower than
// allowed: it dithers less instead.
static void test_dither_slow_loop() {
  Model m{"bp5758d 16 mA", 1, 16, 0};
  reset();
  bp5758d_dither::set_enabled(true);
  Lamp l = make_lamp<0>(m);
  const float code_step = 1.0f / 16 / 1023;
  for (float codes : {0.1f, 0.5f, 1.15f, 3.3f}) {
    l.out->set_level(codes * code_step);
    run_frames(m, l, 300, 3 * FRAME_US);  // only every third frame gets out
    Run run = run_frames(m, l, 4000, 3 * FRAME_US);
    // A third of the frame rate: a third of the frames per flicker.
    CHECK(run.longest <= FRAMES_PER_FLICKER / 3 + 2 || run.codes == 1, "slow loop, %.2f codes: held %d slow frames",
          codes, run.longest);
    CHECK(std::fabs(run.average / code_step - codes) <= 3.0f / FRAMES_PER_FLICKER + 0.01f,
          "slow loop, %.2f codes: got %.3f", codes, run.average / code_step);
  }
  l.out->set_level(0);
  run_frames(m, l, 2);
}

// A channel that is on never goes off, however small: it stays at the
// smallest share of a code that still repeats fast enough (in a colour, one
// channel going dark first changes the colour).
static void test_dither_never_off() {
  Model m{"bp5758d 16 mA", 1, 16, 0};
  reset();
  bp5758d_dither::set_enabled(true);
  Lamp l = make_lamp<0>(m);
  const float code_step = 1.0f / 16 / 1023;
  for (float codes : {0.001f, 0.01f, 0.03f}) {
    Run run = measure(m, l, codes * code_step);
    const float got = run.average / code_step;
    CHECK(std::fabs(got - 1.0f / FRAMES_PER_FLICKER) < 0.005f, "%.3f codes: got %.4f, want the smallest share %.4f",
          codes, got, 1.0f / FRAMES_PER_FLICKER);
    CHECK(run.longest <= FRAMES_PER_FLICKER + 1, "%.3f codes: held one code for %d frames", codes, run.longest);
  }
  l.out->set_level(0);
  run_frames(m, l, 2);
  CHECK(l.ch->on_chip == 0, "off should be off, chip shows %d", l.ch->on_chip);
}

// Two channels on one chip: when the other one changes every pass, the driver
// sends every channel each time. Its copy holds the dithered code, so the
// dithering one stays right. The only exception is a dithered 0: the driver
// holds 1 instead (0 would turn the channel off). The driver may send after
// the frame in a pass, and then the chip shows 1 until the next frame.
static void test_dither_driver_resend() {
  Model m{"bp5758d 16 mA", 1, 16, 0};
  const float code_step = 1.0f / 16 / 1023;
  for (float codes : {0.5f, 3.3f}) {
    reset();
    bp5758d_dither::set_enabled(true);
    Lamp a = make_lamp<0>(m), b = make_lamp<1>(m);
    b.out->set_level(0.5f);
    a.out->set_level(codes * code_step);
    run_frames(m, a, 300);
    double sum = 0;
    for (int i = 0; i < 4000; i++) {
      esphome::fake_micros += FRAME_US;
      b.out->set_level(i % 2 ? 0.5f : 0.6f);  // the other channel changes every pass
      pass(a, b);
      sum += a.ch->on_chip;
    }
    double avg = sum / 4000;
    if (codes >= 1)
      CHECK(std::fabs(avg - codes) < 0.05, "a should average %.2f codes, got %.3f", codes, avg);
    else
      CHECK(avg >= codes - 0.05 && avg <= 1.0, "a at %.2f codes got %.3f", codes, avg);
    a.out->set_level(0);
    b.out->set_level(0);
    pass(a, b);
  }
}

// When the driver wakes the chip (all levels were 0, now one is not), it has
// to send once more after its waking write: the chip may lose that one. Only
// once per chip, and also with dithering off.
static void test_resend_after_wake() {
  Model m{"bp5758d 16 mA", 1, 16, 0};
  for (bool dither : {false, true}) {
    reset();
    bp5758d_dither::set_enabled(dither);
    Lamp a = make_lamp<0>(m), b = make_lamp<1>(m);
    pass(a, b);
    pass(a, b);
    resends = 0;
    a.out->set_level(0.3f / 16 / 1023 * 10);  // wakes the chip
    b.out->set_level(0.2f);
    CHECK(chip.dirty, "waking write not marked");
    for (int i = 0; i < 10; i++)
      pass(a, b);  // driver sends, then one more
    CHECK(resends == 1, "dither %d: %d resends after wake, want 1", dither, resends);
    a.out->set_level(0.5f);  // no wake: no resend
    for (int i = 0; i < 50; i++)
      pass(a, b);
    CHECK(resends == 1, "dither %d: resent without a wake (%d)", dither, resends);
    // dynamic_range's throw-away level 0 within a pass is not a sleep.
    b.out->set_level(0);
    a.out->set_level(0.9f);
    for (int i = 0; i < 50; i++)
      pass(a, b);
    CHECK(resends == 1, "dither %d: resent after a throw-away 0 (%d)", dither, resends);
    a.out->set_level(0);
    for (int i = 0; i < 3; i++)
      pass(a, b);
  }
}

// The slowest pattern is adjustable: at 100 Hz no code is held longer than
// 10 frames, and the lowest level is 1/10 of a code instead of 1/25.
static void test_min_flicker_hz() {
  Model m{"bp5758d 16 mA", 1, 16, 0};
  reset();
  bp5758d_dither::set_enabled(true);
  bp5758d_dither::set_min_flicker_hz(100);
  Lamp l = make_lamp<0>(m);
  const float code_step = 1.0f / 16 / 1023;
  for (float codes : {0.001f, 0.15f, 0.5f, 2.3f, 7.95f}) {
    Run run = measure(m, l, codes * code_step);
    CHECK(run.longest <= 11 || run.codes == 1, "100 Hz, %.3f codes: held one code for %d frames", codes, run.longest);
    if (codes < 0.1f)
      CHECK(std::fabs(run.average / code_step - 0.1f) < 0.005f, "100 Hz, %.3f codes: got %.4f, want 0.1", codes,
            run.average / code_step);
  }
  bp5758d_dither::set_min_flicker_hz(50);
  l.out->set_level(0);
  run_frames(m, l, 2);
}

int main() {
  test_off_passes_through();
  // 1000 frames/s: the most it makes. 250: what a BK7231N's main loop gives.
  test_dither({"bp5758d 16 mA", 1, 16, 0}, 1000);
  test_dither({"bp5758d 16 mA", 1, 16, 0}, 4000);
  test_dither({"bp5758d 90 mA", 1, 90, 0}, 4000);
  test_throw_away_level();
  test_dither_slow_loop();
  test_dither_never_off();
  test_dither_driver_resend();
  test_resend_after_wake();
  test_min_flicker_hz();
  printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
