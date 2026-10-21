#include "frame_pacer.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <string>

namespace atet {

namespace {

struct U128 {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
};

U128 Mul64(std::uint64_t a, std::uint64_t b) {
  const std::uint64_t a_lo = a & 0xFFFFFFFFull;
  const std::uint64_t a_hi = a >> 32;
  const std::uint64_t b_lo = b & 0xFFFFFFFFull;
  const std::uint64_t b_hi = b >> 32;
  const std::uint64_t ll = a_lo * b_lo;
  const std::uint64_t lh = a_lo * b_hi;
  const std::uint64_t hl = a_hi * b_lo;
  const std::uint64_t hh = a_hi * b_hi;
  const std::uint64_t mid = (ll >> 32) + (lh & 0xFFFFFFFFull) + (hl & 0xFFFFFFFFull);
  U128 r;
  r.lo = (mid << 32) | (ll & 0xFFFFFFFFull);
  r.hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
  return r;
}

// floor(x / d) with the remainder; the quotient must fit in 64 bits (x.hi < d).
std::uint64_t Div128(U128 x, std::uint64_t d, std::uint64_t* remainder) {
  std::uint64_t r = x.hi % d;
  std::uint64_t q = 0;
  for (int bit = 63; bit >= 0; --bit) {
    const bool carry = (r >> 63) != 0;
    r = (r << 1) | ((x.lo >> bit) & 1u);
    q <<= 1;
    if (carry || r >= d) {
      r -= d;
      q |= 1u;
    }
  }
  if (remainder != nullptr) {
    *remainder = r;
  }
  return q;
}

}  // namespace

std::uint64_t PitTicksForCounts(std::uint64_t counts, std::uint64_t freq) {
  if (freq == 0) {
    return 0;
  }
  return Div128(Mul64(counts, PitTickRate::kTicks), PitTickRate::kSeconds * freq, nullptr);
}

std::uint64_t PitCountsForTicks(std::uint64_t ticks, std::uint64_t freq) {
  std::uint64_t remainder = 0;
  const std::uint64_t q = Div128(Mul64(ticks, PitTickRate::kSeconds * freq), PitTickRate::kTicks, &remainder);
  return remainder != 0 ? q + 1 : q;
}

void FramePacer::Reset() {
  baseline_ = clock_.Now();
  last_elapsed_ = 0;
}

int FramePacer::Pace(const std::function<void()>& present) {
  const std::uint64_t snapshot = clock_.Now();
  if (present) {
    present();
  }
  clock_.WaitForTickAfter(snapshot);
  const std::uint64_t tick = clock_.Now();
  last_elapsed_ = tick - baseline_;
  baseline_ = tick;
  return static_cast<int>(std::min<std::uint64_t>(last_elapsed_, kMaxStepsPerFrame));
}

// ---------------------------------------------------------------------------------------------
// Self-test (fake clock in thousandths of a tick).
// ---------------------------------------------------------------------------------------------
namespace {

class FakeClock final : public TickClock {
 public:
  static constexpr std::uint64_t kUnit = 1000;
  std::uint64_t Now() override { return t_ / kUnit; }
  void WaitForTickAfter(std::uint64_t tick) override {
    if (Now() <= tick) {
      t_ = (tick + 1) * kUnit;
    }
  }
  void Work(std::uint64_t milliticks) { t_ += milliticks; }
  [[nodiscard]] std::uint64_t t() const { return t_; }
  [[nodiscard]] bool OnEdge() const { return t_ % kUnit == 0; }

 private:
  std::uint64_t t_ = 0;
};

struct Checker {
  int failures = 0;
  void Check(bool ok, const char* name, const std::string& detail) {
    std::fprintf(stderr, "pacer-selftest: %s %s (%s)\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stderr);
    if (!ok) {
      ++failures;
    }
  }
};

std::string U64(std::uint64_t v) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%" PRIu64, v);
  return buffer;
}

// One frame from the clock's current position: `work` milliticks of work, then the pacer.
struct FrameResult {
  int steps = 0;
  std::uint64_t duration = 0;   // milliticks from frame start to the pacer's return
  std::uint64_t present_at = 0; // clock position when the page was flipped
  bool ended_on_edge = false;
  int presents = 0;
};

FrameResult RunFrame(FakeClock& clock, FramePacer& pacer, std::uint64_t work) {
  FrameResult result;
  const std::uint64_t start = clock.t();
  clock.Work(work);
  result.steps = pacer.Pace([&]() {
    result.present_at = clock.t();
    ++result.presents;
  });
  result.duration = clock.t() - start;
  result.ended_on_edge = clock.OnEdge();
  return result;
}

}  // namespace

int RunFramePacerSelfTest() {
  Checker c;
  constexpr std::uint64_t kU = FakeClock::kUnit;

  // Tick rate: 3600.000 s of counter time is 216,003 completed ticks (exactly 216,003.95), for
  // counters of several frequencies. A 60.000 Hz clock would give 216,000.
  const std::uint64_t freqs[] = {10000000ull, 1000000000ull, 3579545ull, 2441406ull, 24000000ull, 1000000ull};
  for (const std::uint64_t f : freqs) {
    const std::uint64_t hour = PitTicksForCounts(3600ull * f, f);
    c.Check(hour == 216003ull, "clock.hour", "freq " + U64(f) + ": " + U64(hour) + " ticks, want 216003");
  }
  {
    const std::uint64_t day = PitTicksForCounts(86400ull * 10000000ull, 10000000ull);
    c.Check(day == 5184094ull, "clock.day", U64(day) + " ticks in 86400 s, want 5184094");
  }
  // Tick edges are exact: the first count of tick k maps to k, the count before it to k - 1.
  {
    bool ok = true;
    std::string where = "all edges exact";
    const std::uint64_t ks[] = {1ull, 2ull, 21ull, 60ull, 61ull, 216003ull, 216004ull, 5184094ull, 155522820ull};
    for (const std::uint64_t f : freqs) {
      for (const std::uint64_t k : ks) {
        const std::uint64_t counts = PitCountsForTicks(k, f);
        if (PitTicksForCounts(counts, f) != k || PitTicksForCounts(counts - 1, f) != k - 1) {
          ok = false;
          where = "freq " + U64(f) + " tick " + U64(k);
        }
      }
    }
    c.Check(ok, "clock.edges", where);
  }

  // Light work (0.4 tick) from a tick edge: one step, the frame ends at the next edge.
  {
    FakeClock clock;
    clock.Work(100 * kU);
    FramePacer pacer(clock);
    pacer.Reset();
    const FrameResult r = RunFrame(clock, pacer, 400);
    c.Check(r.steps == 1 && r.duration == kU && r.ended_on_edge, "pacer.light-work",
            "steps " + std::to_string(r.steps) + " duration " + U64(r.duration) + "/1000");
    c.Check(r.presents == 1 && r.present_at == 100 * kU + 400, "pacer.flip-before-wait",
            "presents " + std::to_string(r.presents) + " at " + U64(r.present_at) + ", want 100400");
  }
  // Work of 1.5 ticks: the pacer waits for the edge after its snapshot, so the frame lasts exactly
  // 2 ticks and returns 2. ("Run as soon as one tick has passed" would return 1 after 1.5 ticks.)
  {
    FakeClock clock;
    clock.Work(100 * kU);
    FramePacer pacer(clock);
    pacer.Reset();
    const FrameResult r = RunFrame(clock, pacer, 1500);
    c.Check(r.steps == 2 && r.duration == 2 * kU && r.ended_on_edge, "pacer.overrun-1.5",
            "steps " + std::to_string(r.steps) + " duration " + U64(r.duration) + "/1000, want 2 and 2000");
  }
  // Work of 9.5 ticks: 10 ticks elapse, 6 steps run, 4 are dropped.
  {
    FakeClock clock;
    clock.Work(100 * kU);
    FramePacer pacer(clock);
    pacer.Reset();
    const FrameResult r = RunFrame(clock, pacer, 9500);
    c.Check(r.steps == 6 && pacer.last_elapsed() == 10 && r.duration == 10 * kU, "pacer.cap",
            "steps " + std::to_string(r.steps) + " elapsed " + U64(pacer.last_elapsed()) + ", want 6 of 10");
  }
  // A phase that never calls the pacer (a palette fade, 63 ticks) leaves the baseline stale: the next
  // frame returns 6. A reset rebases, so the frame after it returns 1.
  {
    FakeClock clock;
    FramePacer pacer(clock);
    clock.Work(10 * kU);
    RunFrame(clock, pacer, 300);
    clock.Work(63 * kU);  // blocking phase, no pacer calls
    const FrameResult stale = RunFrame(clock, pacer, 200);
    c.Check(stale.steps == 6, "pacer.stale-baseline", "steps " + std::to_string(stale.steps) + ", want 6");

    RunFrame(clock, pacer, 300);
    clock.Work(63 * kU);
    pacer.Reset();
    const FrameResult resumed = RunFrame(clock, pacer, 200);
    c.Check(resumed.steps == 1, "pacer.reset", "steps " + std::to_string(resumed.steps) + ", want 1");
  }
  // The baseline is 0 at boot (BSS): the first pacer call after the boot splashes returns 6.
  {
    FakeClock clock;
    clock.Work(720 * kU);
    FramePacer pacer(clock);
    const FrameResult r = RunFrame(clock, pacer, 300);
    c.Check(r.steps == 6, "pacer.boot-baseline", "steps " + std::to_string(r.steps) + ", want 6");
  }
  // Property: arbitrary work never yields a zero-length frame, every frame ends on an edge, the step
  // count is min(ticks elapsed, 6), and frame durations add up to the clock span (no drift).
  {
    FakeClock clock;
    FramePacer pacer(clock);
    pacer.Reset();
    std::uint32_t lcg = 12345u;
    bool ok = true;
    std::string where = "20000 frames";
    std::uint64_t total = 0;
    const std::uint64_t start = clock.t();
    for (int frame = 0; frame < 20000 && ok; ++frame) {
      lcg = lcg * 1103515245u + 12345u;
      const std::uint64_t work = (lcg >> 8) % 12000u;  // 0 .. 11.999 ticks
      const FrameResult r = RunFrame(clock, pacer, work);
      const std::uint64_t ticks = pacer.last_elapsed();
      total += r.duration;
      // From an edge, work w ends inside tick floor(w); the frame lasts until the next edge.
      const std::uint64_t expected_ticks = work / kU + 1;
      const int expected = static_cast<int>(std::min<std::uint64_t>(expected_ticks, 6));
      if (!r.ended_on_edge || ticks != expected_ticks || r.steps != expected || r.duration != expected_ticks * kU) {
        ok = false;
        where = "frame " + std::to_string(frame) + " work " + U64(work) + " steps " + std::to_string(r.steps) +
                " elapsed " + U64(ticks);
      }
    }
    c.Check(ok && total == clock.t() - start, "pacer.property", where);
  }
  // One hour of light frames from boot runs exactly 216,003 steps (one per tick, none lost).
  {
    FakeClock clock;
    FramePacer pacer(clock);
    pacer.Reset();
    std::uint64_t steps = 0;
    const std::uint64_t hour_end = PitTicksForCounts(3600ull * 10000000ull, 10000000ull);
    while (clock.Now() < hour_end) {
      steps += static_cast<std::uint64_t>(RunFrame(clock, pacer, 250).steps);
    }
    c.Check(steps == 216003ull, "pacer.hour-steps", U64(steps) + " steps, want 216003");
  }

  std::fprintf(stderr, "pacer-selftest: %s (%d failure(s))\n", c.failures == 0 ? "ALL PASS" : "FAILED", c.failures);
  std::fflush(stderr);
  return c.failures;
}

}  // namespace atet
