// The Web Audio primitives the game is built out of, reimplemented.
//
// Nothing in this game plays a sample file: every sound is synthesised from noise
// buffers, band-limited oscillators, biquads and gain envelopes. The web build got
// those from the browser's Web Audio implementation, so porting them is not a matter
// of "any reasonable filter will do" — the recipes in js/audio/sfx.js were tuned by
// ear against *Chrome's* nodes, and four of those nodes do not behave the way a
// textbook (or a naive C++ reading) would suggest:
//
//  1. **BiquadFilterNode.Q is in DECIBELS for `lowpass` and `highpass`.** The Audio
//     EQ Cookbook's Q is dimensionless, and every filter tutorial uses it that way,
//     but Blink folds the parameter in as 10^(Q/20) and then uses a completely
//     different coefficient derivation (see Biquad::setLowpassParams). Copying
//     cookbook code for these two types gives every filter in the game the wrong
//     resonance — and the game sweeps them constantly. For `bandpass` the parameter
//     *is* the conventional dimensionless Q, which is exactly the trap: two of the
//     three filter types the recipes use read the same field differently.
//  2. **`sawtooth` and `triangle` are band-limited**, built as PeriodicWave tables
//     with the harmonic count culled per pitch range. `sign(sin(x))` aliases audibly
//     — and the mob voices are sawtooth larynxes swept through formants, which is
//     the worst case for aliasing.
//  3. **Connecting a node to an AudioParam SUMS into it.** The tremolo that makes a
//     sheep bleat is an LFO summed into the gain envelope, not a multiplier, so the
//     gain swings *above* the envelope peak as well as below it.
//  4. **`setTargetAtTime` is exponential**, `target + (v0 - target)·exp(-Δt/τ)`, not
//     a linear ramp to the target by time τ.
//
// Everything here is called from the audio thread and allocates nothing.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hr::audio {

// Web Audio's render quantum. Automation is evaluated at block boundaries and
// interpolated across the block; filter coefficients are recomputed once per block,
// which is what Blink does too.
inline constexpr int kBlockFrames = 128;

// ---------------------------------------------------------------------------
// Noise buffers
// ---------------------------------------------------------------------------

enum class NoiseKind : std::uint8_t { White, Pink, Crackle };

// The three 2-second buffers from AudioEngine.noise(), generated once and shared by
// every voice. `Math.random()` seeds them in the web build, so these are the third
// use of randomness described in core/prng.h — never worldgen, never reproducible.
class NoiseBank {
 public:
  void build(int sampleRate);
  const std::vector<float>& buffer(NoiseKind kind) const { return buffers_[static_cast<int>(kind)]; }

 private:
  std::vector<float> buffers_[3];
};

// ---------------------------------------------------------------------------
// Biquad
// ---------------------------------------------------------------------------

enum class FilterType : std::uint8_t { Lowpass, Highpass, Bandpass };

class Biquad {
 public:
  // Normalised coefficients, separated from the filter state so a *swept* filter can
  // interpolate them per sample. Holding them fixed for a whole render quantum and
  // stepping at the boundary is audible: a bandpass sweeping 2400 Hz to 420 Hz in
  // 0.3 s moves 1.5% per block, and 375 small coefficient discontinuities a second
  // is textbook zipper noise. It made the splash and the wade step sound gritty
  // rather than wet. Blink interpolates per sample whenever a filter parameter is
  // automated, and so does this.
  struct Coeffs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
  };

  // `q` carries whichever units the type wants: dB for lowpass/highpass, a plain
  // dimensionless Q for bandpass. That asymmetry is Web Audio's, not ours.
  static Coeffs design(FilterType type, double freqHz, double q, double sampleRate);

  void set(FilterType type, double freqHz, double q, double sampleRate) {
    c_ = design(type, freqHz, q, sampleRate);
  }
  void setCoeffs(const Coeffs& c) { c_ = c; }
  void reset() { x1_ = x2_ = y1_ = y2_ = 0.0; }

  float process(float x) { return process(x, c_); }

  float process(float x, const Coeffs& c) {
    const double y = c.b0 * x + c.b1 * x1_ + c.b2 * x2_ - c.a1 * y1_ - c.a2 * y2_;
    x2_ = x1_;
    x1_ = x;
    y2_ = y1_;
    y1_ = y;
    return static_cast<float>(y);
  }

  static Coeffs lerp(const Coeffs& a, const Coeffs& b, double u) {
    return Coeffs{a.b0 + (b.b0 - a.b0) * u, a.b1 + (b.b1 - a.b1) * u, a.b2 + (b.b2 - a.b2) * u,
                  a.a1 + (b.a1 - a.a1) * u, a.a2 + (b.a2 - a.a2) * u};
  }

 private:
  Coeffs c_;
  double x1_ = 0.0, x2_ = 0.0, y1_ = 0.0, y2_ = 0.0;
};

// ---------------------------------------------------------------------------
// Band-limited oscillators
// ---------------------------------------------------------------------------

enum class Wave : std::uint8_t { Sine, Sawtooth, Triangle, Square };

// A PeriodicWave: one time-domain table per pitch range, each truncated to the
// harmonic count that keeps its range's top fundamental under Nyquist. Blink builds
// these with an inverse FFT of the Fourier coefficients; additive synthesis with a
// shared sine table produces the same samples and needs no FFT.
class PeriodicWave {
 public:
  // Blink's WaveDataForFundamentalFrequency picks two adjacent ranges and the blend
  // between them. That costs a log2 and two clamps, so it is hoisted to a Reader
  // resolved once per render block rather than per sample — a sweeping oscillator
  // crosses a range boundary a few times a second, not 48000.
  struct Reader {
    const float* high = nullptr;  // more partials
    const float* low = nullptr;   // fewer
    float blend = 0.0f;
    int size = 0;
  };

  void build(Wave shape, int sampleRate);
  Reader reader(double frequency) const;

  static float read(const Reader& r, double phase) {
    const int mask = r.size - 1;
    const int i0 = static_cast<int>(phase) & mask;
    const int i1 = (i0 + 1) & mask;
    const float frac = static_cast<float>(phase - std::floor(phase));
    const float hi = r.high[i0] + (r.high[i1] - r.high[i0]) * frac;
    const float lo = r.low[i0] + (r.low[i1] - r.low[i0]) * frac;
    return hi + (lo - hi) * r.blend;
  }

  double tableSize() const { return static_cast<double>(kSize); }

 private:
  static constexpr int kSize = 4096;              // PeriodicWaveSize() at 44.1/48 kHz
  static constexpr int kMaxPartials = kSize / 2;  // 2048
  static constexpr int kOctaveBands = 3;          // Blink's kNumberOfOctaveBands
  static constexpr int kCentsPerRange = 1200 / kOctaveBands;

  int ranges_ = 0;
  double lowestFundamental_ = 1.0;
  std::vector<float> tables_;  // ranges_ * kSize, row-major
};

// The three shapes the recipes ask for, built once per sample rate.
class WaveBank {
 public:
  void build(int sampleRate);
  const PeriodicWave& wave(Wave w) const { return waves_[static_cast<int>(w)]; }

 private:
  PeriodicWave waves_[4];
};

// ---------------------------------------------------------------------------
// AudioParam automation
// ---------------------------------------------------------------------------

// A scheduled parameter timeline. Fixed capacity because it lives inside a voice on
// the audio thread and the recipes never queue more than three events on one param
// (setValueAtTime, linearRampToValueAtTime, exponentialRampToValueAtTime — the AD
// envelope every burst and tone uses).
class AudioParam {
 public:
  // `baseTime` is what a ramp scheduled with no preceding event ramps *from*, which
  // is Chrome's behaviour: `bq.frequency.value = f` followed by a bare
  // exponentialRampToValueAtTime starts sweeping from the value the param already
  // held, at the moment the ramp was scheduled.
  void reset(float initial, double baseTime) {
    value_ = initial;
    baseTime_ = baseTime;
    count_ = 0;
  }

  void setValueAtTime(float v, double t) { push(Kind::SetValue, v, t); }
  void linearRampToValueAtTime(float v, double t) { push(Kind::LinearRamp, v, t); }
  void exponentialRampToValueAtTime(float v, double t) { push(Kind::ExpRamp, v, t); }

  bool automated() const { return count_ > 0; }
  float valueAt(double t) const;

 private:
  enum class Kind : std::uint8_t { SetValue, LinearRamp, ExpRamp };
  struct Event {
    Kind kind;
    float value;
    double time;
  };

  void push(Kind kind, float v, double t);
  static float interpolate(Kind kind, float v0, float v1, double t0, double t1, double t);

  Event events_[4];
  int count_ = 0;
  float value_ = 0.0f;
  double baseTime_ = 0.0;
};

// `setTargetAtTime` on its own, for the parameters that only ever get that treatment
// — the bed gains, the master duck, the underwater sweep, the listener pose. One
// pole per block, which is the same exponential the spec defines.
class Smoother {
 public:
  void snap(float v) {
    value_ = v;
    target_ = v;
  }
  void setTarget(float v, float tau) {
    target_ = v;
    tau_ = tau;
  }
  float value() const { return value_; }
  float advance(float dt) {
    if (tau_ <= 0.0f) {
      value_ = target_;
    } else {
      value_ = target_ + (value_ - target_) * std::exp(-dt / tau_);
    }
    return value_;
  }

 private:
  float value_ = 0.0f;
  float target_ = 0.0f;
  float tau_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Panning
// ---------------------------------------------------------------------------

struct ListenerPose {
  float pos[3] = {0.0f, 0.0f, 0.0f};
  float forward[3] = {0.0f, 0.0f, -1.0f};
  float up[3] = {0.0f, 1.0f, 0.0f};
};

struct PanGains {
  float left = 1.0f, right = 1.0f;
};

// PannerNode with panningModel "equalpower" and distanceModel "inverse", for a mono
// source. Two things a hand-rolled panner usually gets wrong and this does not:
// the azimuth is measured in the plane perpendicular to the listener's up vector
// (so a sound directly overhead pans to centre rather than jittering), and a source
// dead ahead comes out at cos(45 degrees) = 0.707 per channel — 3 dB below the same
// sound routed to the bus without a panner, because an unpanned mono signal is
// up-mixed to both channels at full gain.
PanGains equalPowerPan(const ListenerPose& listener, const float pos[3], float refDistance,
                       float maxDistance, float rolloff);

// ---------------------------------------------------------------------------
// Compressor
// ---------------------------------------------------------------------------

// A soft-knee limiter standing in for DynamicsCompressorNode.
//
// Blink's version has a 6 ms lookahead pre-delay, a knee whose curvature is solved
// by binary search, a three-stage release and an empirical makeup gain of
// pow(1/gainAt0dBFS, 0.6). Reproducing it exactly is a few hundred lines of
// reverse-engineered curve fitting for a node the game only uses as a safety net,
// so this is the documented approximation the plan called for: the same pre-delay,
// the standard quadratic-interpolated dB-domain knee, and the same makeup-gain
// formula evaluated against *this* curve. `bypass` exists so the difference can be
// listened to directly.
class Compressor {
 public:
  void configure(float thresholdDb, float kneeDb, float ratio, float attackSec, float releaseSec,
                 int sampleRate);
  void reset();
  void process(float* left, float* right, int frames);

  void setBypass(bool on) { bypass_ = on; }
  float reductionDb() const { return reductionDb_; }

 private:
  float curveDb(float inputDb) const;

  float thresholdDb_ = -14.0f, kneeDb_ = 18.0f, ratio_ = 5.0f;
  float attackCoef_ = 0.0f, releaseCoef_ = 0.0f;
  float makeup_ = 1.0f;
  float envelope_ = 1.0f;  // current gain reduction, linear
  float reductionDb_ = 0.0f;
  bool bypass_ = false;

  // 6 ms of lookahead, so the gain is already down when the transient arrives.
  std::vector<float> delay_[2];
  int delayWrite_ = 0;
  int delayLen_ = 0;
};

// ---------------------------------------------------------------------------
// Small shared helpers
// ---------------------------------------------------------------------------

inline float linearToDb(float x) { return 20.0f * std::log10(x <= 1e-9f ? 1e-9f : x); }
inline float dbToLinear(float db) { return std::pow(10.0f, db * 0.05f); }

}  // namespace hr::audio
