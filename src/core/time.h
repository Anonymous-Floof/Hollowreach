// Frame timing, ported from the loop in js/main.js:524-533.
//
// The web build ran a fully variable timestep with dt clamped to 0.05 s, and
// that is kept deliberately: horizontal player velocity is assigned from input
// rather than integrated, so displacement is exact at any frame rate, and the
// jump apex only varies between 1.11 and 1.32 blocks across the whole rate
// range — never crossing a ledge height that matters.
//
// What the web build lacked is a bound on how far a single step can move a body,
// so substeps() splits a long frame into at most kMaxSubsteps pieces for the
// player and entity updates. Everything else still sees the whole dt.

#pragma once

#include <algorithm>

namespace hr {

class FrameClock {
 public:
  // js/main.js:527 — a long frame is clamped, not accumulated, so time lost to
  // a stall is simply discarded rather than replayed as a physics catch-up.
  static constexpr double kMaxDt = 0.05;
  // Above this, a frame is split. 1/60 keeps trajectory variance small without
  // adding substeps on any normal display.
  static constexpr double kSubstepDt = 1.0 / 60.0;
  static constexpr int kMaxSubsteps = 3;

  // Advances to `now` (seconds, monotonic) and returns the clamped delta.
  double tick(double now) {
    if (last_ < 0.0) {
      last_ = now;
      return 0.0;
    }
    double dt = now - last_;
    last_ = now;
    if (dt < 0.0) dt = 0.0;
    raw_ = dt;
    if (dt > kMaxDt) dt = kMaxDt;
    dt_ = dt;
    simTime_ += dt;

    // The RAW delta, deliberately. Accumulating the clamped one makes the meter
    // saturate: once every frame is over 50 ms the sum is frames * kMaxDt and the
    // readout sticks at exactly 20 fps no matter how bad it gets. A performance
    // gauge that cannot show you the bottom of the range is worse than none, and
    // this one hid a five-fold difference behind a single number.
    fpsAccum_ += raw_;
    ++fpsFrames_;
    if (fpsAccum_ >= 0.5) {
      fps_ = static_cast<float>(fpsFrames_ / fpsAccum_);
      frameMs_ = static_cast<float>(1000.0 * fpsAccum_ / fpsFrames_);
      fpsAccum_ = 0.0;
      fpsFrames_ = 0;
    }
    return dt_;
  }

  double dt() const { return dt_; }
  // Unclamped delta, for diagnostics only — never feed it to simulation.
  double rawDt() const { return raw_; }
  float fps() const { return fps_; }
  // Mean wall-clock milliseconds per frame over the last window.
  float frameMs() const { return frameMs_; }

  // Monotonic accumulated simulation seconds. Used anywhere the web build
  // reached for performance.now(), so behaviour does not depend on wall clock —
  // it matters for the double-tap-to-fly window, since multiplayer never pauses.
  double simTime() const { return simTime_; }

  // How many equal substeps `dt` should be split into, and their size.
  int substeps(double& stepOut) const {
    int n = static_cast<int>(dt_ / kSubstepDt) + 1;
    n = std::clamp(n, 1, kMaxSubsteps);
    stepOut = dt_ / n;
    return n;
  }

 private:
  double last_ = -1.0;
  double dt_ = 0.0;
  double raw_ = 0.0;
  double simTime_ = 0.0;
  double fpsAccum_ = 0.0;
  int fpsFrames_ = 0;
  float fps_ = 0.0f;
  float frameMs_ = 0.0f;
};

// Rate limiter for the sub-frame work the web build ran off accumulators.
//
// js/main.js:705, js/net/host.js:483 and friends all did `acc += dt; if (acc >
// T) { ...; acc = 0; }`, which resets the remainder and so couples the real
// period to the frame rate — at SNAP_MS=100 that drifts the effective snapshot
// rate between roughly 7.5 Hz and 9.5 Hz. Subtracting the period instead keeps
// the average exact, and dropping whole missed periods stops a stall from
// firing a burst of catch-up ticks.
class Interval {
 public:
  explicit Interval(double period) : period_(period) {}

  void setPeriod(double period) { period_ = period; }
  double period() const { return period_; }

  bool due(double dt) {
    acc_ += dt;
    if (acc_ < period_) return false;
    acc_ -= period_;
    if (acc_ > period_) acc_ = 0.0;  // fell far behind: skip, do not burst
    return true;
  }

  void reset() { acc_ = 0.0; }
  // Makes the next due() fire immediately, for "do this on the first frame".
  void expire() { acc_ = period_; }

 private:
  double period_;
  double acc_ = 0.0;
};

}  // namespace hr
