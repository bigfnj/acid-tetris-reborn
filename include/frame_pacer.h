#pragma once

#include <cstdint>
#include <functional>

namespace atet {

// The original game clock. ATET.EXE programs PIT channel 0 once at startup (mode 3, divisor
// 0x4DAE = 19886) and its INT 8 handler steps the music volume ramp, pumps the sound driver and
// increments the tick counter. The PIT input is 105/88 MHz, so the tick rate is
// 105,000,000 / (88 * 19886) = 6,562,500 / 109,373 = 60.00109716 Hz (216,003.95 ticks per hour).
// The display refresh (Mode X, 59.713 Hz) plays no part: no live code waits for the retrace.
struct PitTickRate {
  static constexpr std::uint64_t kTicks = 6562500;    // this many ticks ...
  static constexpr std::uint64_t kSeconds = 109373;   // ... every this many seconds
};

// Exact tick arithmetic against a performance counter of `freq` counts per second (128-bit
// intermediates, so there is no drift and no overflow for any realistic run length).
// Ticks completed after `counts` counts: floor(counts * kTicks / (kSeconds * freq)).
std::uint64_t PitTicksForCounts(std::uint64_t counts, std::uint64_t freq);
// Counts after which `ticks` ticks have completed: ceil(ticks * kSeconds * freq / kTicks).
std::uint64_t PitCountsForTicks(std::uint64_t ticks, std::uint64_t freq);

// Source of the absolute tick count. Real time derives it from the performance counter; smoke and
// scripted runs use a virtual tick so they stay frame-exact and deterministic.
class TickClock {
 public:
  virtual ~TickClock() = default;
  virtual std::uint64_t Now() = 0;
  // Block until Now() > tick.
  virtual void WaitForTickAfter(std::uint64_t tick) = 0;
};

// The original's frame pacer. Every interactive screen of the original is one loop shape: run
// `steps` logic steps, draw once, then call the pacer, which flips the page FIRST and then waits
// for the tick to change from the value it read on entry, so every frame ends on a tick edge and
// lasts a whole number of ticks (never zero). It returns the ticks since its previous return,
// capped at 6 (the excess is discarded, so the game only slows below about 8.6 frames per second),
// and that is the next frame's step count. A reset only rebases (returns 0); the caller then runs
// exactly one step. The baseline is 0 at boot and moves only when the pacer is called, so the first
// call after a phase that never calls it (a splash page, a palette fade) returns 6.
class FramePacer {
 public:
  static constexpr int kMaxStepsPerFrame = 6;

  explicit FramePacer(TickClock& clock) : clock_(clock) {}

  // Pacer with AL = 1: baseline = tick, no flip, no wait.
  void Reset();
  // Pacer with AL = 0: snapshot the tick, present, wait until the tick differs from the snapshot,
  // return min(tick - baseline, 6) and rebase. (The original re-reads the tick for the rebase and
  // can lose one tick to an interrupt between the two reads; that race is not reproduced.)
  int Pace(const std::function<void()>& present);

  [[nodiscard]] std::uint64_t baseline() const { return baseline_; }
  // Ticks between the previous return and the last one, before the cap (diagnostics).
  [[nodiscard]] std::uint64_t last_elapsed() const { return last_elapsed_; }

 private:
  TickClock& clock_;
  std::uint64_t baseline_ = 0;
  std::uint64_t last_elapsed_ = 0;
};

// Virtual tick for smoke and scripted runs: each frame of simulated work takes `work_ticks`
// whole ticks before the pacer's wait (0 by default, i.e. every frame lasts exactly one tick).
class VirtualTickClock final : public TickClock {
 public:
  std::uint64_t Now() override { return tick_; }
  void WaitForTickAfter(std::uint64_t tick) override {
    if (tick_ <= tick) {
      tick_ = tick + 1;
    }
  }
  void SimulateWork(std::uint64_t ticks) { tick_ += ticks; }
  void Set(std::uint64_t tick) { tick_ = tick; }

 private:
  std::uint64_t tick_ = 0;
};

// Fake-clock unit tests of the tick arithmetic and of FramePacer (no SDL, no window). Logs one
// line per check and returns the number of failed checks. Run with `--selftest-pacer`.
int RunFramePacerSelfTest();

}  // namespace atet
