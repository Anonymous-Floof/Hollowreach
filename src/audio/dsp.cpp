#include "audio/dsp.h"

#include <algorithm>
#include <cstring>

#include "core/prng.h"

namespace hr::audio {
namespace {

constexpr double kPi = 3.14159265358979323846;

inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

// ---------------------------------------------------------------------------
// Noise
// ---------------------------------------------------------------------------

void NoiseBank::build(int sampleRate) {
  const std::size_t len = static_cast<std::size_t>(sampleRate) * 2;  // AudioEngine.noise(): 2 s

  // White.
  {
    auto& d = buffers_[static_cast<int>(NoiseKind::White)];
    d.resize(len);
    for (std::size_t i = 0; i < len; ++i) d[i] = static_cast<float>(randomUnit() * 2.0 - 1.0);
  }

  // Pink: three leaky integrators summed with a little of the raw white on top.
  // Not the Voss-McCartney generator — this is the running-average variant the web
  // build wrote out by hand, and the coefficients are part of how the wind sounds.
  {
    auto& d = buffers_[static_cast<int>(NoiseKind::Pink)];
    d.resize(len);
    double b0 = 0, b1 = 0, b2 = 0;
    for (std::size_t i = 0; i < len; ++i) {
      const double w = randomUnit() * 2.0 - 1.0;
      b0 = 0.997 * b0 + 0.029 * w;
      b1 = 0.985 * b1 + 0.032 * w;
      b2 = 0.950 * b2 + 0.048 * w;
      d[i] = static_cast<float>((b0 + b1 + b2 + w * 0.05) * 2.1);
    }
  }

  // Crackle: sparse impulses, each decaying by 0.62 per sample.
  //
  // The JS reads `d[i - 1] * 0.62 || 0`, which at i = 0 indexes off the front of a
  // Float32Array, yields undefined, multiplies to NaN, and the `|| 0` turns that
  // into 0. So the first sample is silence and every later one continues the decay
  // — which a plain `prev` carrying 0 reproduces exactly, including the `|| 0`,
  // since 0 * 0.62 is 0 either way.
  {
    auto& d = buffers_[static_cast<int>(NoiseKind::Crackle)];
    d.resize(len);
    double prev = 0.0;
    for (std::size_t i = 0; i < len; ++i) {
      const double v = randomUnit() < 0.0018 ? (randomUnit() * 2.0 - 1.0) : prev * 0.62;
      d[i] = static_cast<float>(v);
      prev = v;
    }
  }
}

// ---------------------------------------------------------------------------
// Biquad — transcribed from Blink's Biquad.cpp
// ---------------------------------------------------------------------------

namespace {

Biquad::Coeffs normalized(double b0, double b1, double b2, double a0, double a1, double a2) {
  const double inv = 1.0 / a0;
  return Biquad::Coeffs{b0 * inv, b1 * inv, b2 * inv, a1 * inv, a2 * inv};
}

Biquad::Coeffs designLowpass(double cutoff, double resonanceDb) {
  cutoff = clampd(cutoff, 0.0, 1.0);

  // cutoff == 1 is a passthrough and cutoff == 0 is silence; b0 = cutoff says both.
  if (cutoff == 1.0 || cutoff == 0.0) {
    return normalized(cutoff, 0, 0, 1, 0, 0);
  }

  // THE dB-Q correction. `resonanceDb` arrives as the value the recipe wrote into
  // `BiquadFilterNode.Q`, and for a lowpass Web Audio reads it as decibels.
  const double g = std::pow(10.0, 0.05 * std::max(0.0, resonanceDb));
  const double d = std::sqrt((4.0 - std::sqrt(16.0 - 16.0 / (g * g))) / 2.0);

  const double theta = kPi * cutoff;
  const double sn = 0.5 * d * std::sin(theta);
  const double beta = 0.5 * (1.0 - sn) / (1.0 + sn);
  const double gamma = (0.5 + beta) * std::cos(theta);
  const double alpha = 0.25 * (0.5 + beta - gamma);

  return normalized(2.0 * alpha, 4.0 * alpha, 2.0 * alpha, 1.0, -2.0 * gamma, 2.0 * beta);
}

Biquad::Coeffs designHighpass(double cutoff, double resonanceDb) {
  cutoff = clampd(cutoff, 0.0, 1.0);

  if (cutoff == 1.0) return normalized(0, 0, 0, 1, 0, 0);  // everything is below the cutoff
  if (cutoff == 0.0) return normalized(1, 0, 0, 1, 0, 0);  // nothing is

  const double g = std::pow(10.0, 0.05 * std::max(0.0, resonanceDb));
  const double d = std::sqrt((4.0 - std::sqrt(16.0 - 16.0 / (g * g))) / 2.0);

  const double theta = kPi * cutoff;
  const double sn = 0.5 * d * std::sin(theta);
  const double beta = 0.5 * (1.0 - sn) / (1.0 + sn);
  const double gamma = (0.5 + beta) * std::cos(theta);
  const double alpha = 0.25 * (0.5 + beta + gamma);

  return normalized(2.0 * alpha, -4.0 * alpha, 2.0 * alpha, 1.0, -2.0 * gamma, 2.0 * beta);
}

Biquad::Coeffs designBandpass(double cutoff, double q) {
  cutoff = std::max(0.0, cutoff);
  q = std::max(0.0, q);

  // Here — and only here of the three — `q` is the cookbook's dimensionless Q.
  if (cutoff > 0.0 && cutoff < 1.0) {
    if (q > 0.0) {
      const double w0 = kPi * cutoff;
      const double alpha = std::sin(w0) / (2.0 * q);
      const double k = std::cos(w0);
      return normalized(alpha, 0.0, -alpha, 1.0 + alpha, -2.0 * k, 1.0 - alpha);
    }
    return normalized(1, 0, 0, 1, 0, 0);  // the Q -> 0 limit of the transfer function
  }
  return normalized(0, 0, 0, 1, 0, 0);
}

}  // namespace

Biquad::Coeffs Biquad::design(FilterType type, double freqHz, double q, double sampleRate) {
  const double cutoff = freqHz / (0.5 * sampleRate);
  switch (type) {
    case FilterType::Lowpass: return designLowpass(cutoff, q);
    case FilterType::Highpass: return designHighpass(cutoff, q);
    case FilterType::Bandpass: return designBandpass(cutoff, q);
  }
  return Coeffs{};
}

// ---------------------------------------------------------------------------
// PeriodicWave
// ---------------------------------------------------------------------------

void PeriodicWave::build(Wave shape, int sampleRate) {
  const double nyquist = 0.5 * sampleRate;
  lowestFundamental_ = nyquist / kMaxPartials;
  ranges_ = static_cast<int>(0.5 + kOctaveBands * std::log2(static_cast<double>(kSize)));

  // The Fourier sine coefficients Blink's GenerateBasicWaveform writes into the
  // imaginary half of the spectrum. Every basic shape is an odd function with a
  // positive slope at t = 0, so the cosine terms are all zero.
  std::vector<double> b(kMaxPartials + 1, 0.0);
  for (int n = 1; n <= kMaxPartials; ++n) {
    const double piFactor = 2.0 / (n * kPi);
    switch (shape) {
      case Wave::Sine:
        b[n] = (n == 1) ? 1.0 : 0.0;
        break;
      case Wave::Square:  // b[n] = 4/(n*pi) for odd n
        b[n] = (n & 1) ? 2.0 * piFactor : 0.0;
        break;
      case Wave::Sawtooth:  // b[n] = (-1)^(n+1) * 2/(n*pi)
        b[n] = piFactor * ((n & 1) ? 1.0 : -1.0);
        break;
      case Wave::Triangle:  // b[n] = 8/(pi*n)^2 * (-1)^((n-1)/2) for odd n
        b[n] = (n & 1) ? 2.0 * piFactor * piFactor * ((((n - 1) >> 1) & 1) ? -1.0 : 1.0) : 0.0;
        break;
    }
  }

  // How many partials survive in each range. Non-increasing, so a single ascending
  // pass over the harmonics can snapshot every range's partial sum on the way.
  std::vector<int> partials(ranges_, 0);
  for (int r = 0; r < ranges_; ++r) {
    const double cull = std::pow(2.0, -(r * static_cast<double>(kCentsPerRange)) / 1200.0);
    partials[r] = static_cast<int>(kMaxPartials * cull);
  }

  tables_.assign(static_cast<std::size_t>(ranges_) * kSize, 0.0f);

  // sin(2*pi*k/kSize) for every k, so the additive sum below is a table lookup
  // rather than 17 million calls to sin().
  std::vector<double> sinTab(kSize);
  for (int k = 0; k < kSize; ++k) sinTab[k] = std::sin(2.0 * kPi * k / kSize);

  const int maxPartials = partials[0];
  for (int j = 0; j < kSize; ++j) {
    double sum = 0.0;
    int r = ranges_ - 1;
    while (r >= 0 && partials[r] == 0) --r;  // the top ranges keep nothing at all
    for (int n = 1; n <= maxPartials && r >= 0; ++n) {
      if (b[n] != 0.0) sum += b[n] * sinTab[(n * j) & (kSize - 1)];
      while (r >= 0 && partials[r] == n) {
        tables_[static_cast<std::size_t>(r) * kSize + j] = static_cast<float>(sum);
        --r;
      }
    }
  }

  // Blink normalises every range by the peak of range 0 — the one with the most
  // partials, and therefore the largest Gibbs overshoot.
  float peak = 0.0f;
  for (int j = 0; j < kSize; ++j) peak = std::max(peak, std::fabs(tables_[j]));
  if (peak > 0.0f) {
    const float scale = 1.0f / peak;
    for (auto& v : tables_) v *= scale;
  }
}

PeriodicWave::Reader PeriodicWave::reader(double frequency) const {
  frequency = std::fabs(frequency);

  // Blink's WaveDataForFundamentalFrequency. The "+1" rounds up into the next range
  // just early enough that partials are culled before they alias.
  const double ratio = frequency > 0.0 ? frequency / lowestFundamental_ : 0.5;
  double pitchRange = 1.0 + (std::log2(ratio) * 1200.0) / kCentsPerRange;
  pitchRange = clampd(pitchRange, 0.0, static_cast<double>(ranges_ - 1));

  const int hi = static_cast<int>(pitchRange);  // more partials
  const int lo = hi < ranges_ - 1 ? hi + 1 : hi;

  Reader r;
  r.high = &tables_[static_cast<std::size_t>(hi) * kSize];
  r.low = &tables_[static_cast<std::size_t>(lo) * kSize];
  r.blend = static_cast<float>(pitchRange - hi);
  r.size = kSize;
  return r;
}

void WaveBank::build(int sampleRate) {
  waves_[static_cast<int>(Wave::Sine)].build(Wave::Sine, sampleRate);
  waves_[static_cast<int>(Wave::Sawtooth)].build(Wave::Sawtooth, sampleRate);
  waves_[static_cast<int>(Wave::Triangle)].build(Wave::Triangle, sampleRate);
  waves_[static_cast<int>(Wave::Square)].build(Wave::Square, sampleRate);
}

// ---------------------------------------------------------------------------
// AudioParam
// ---------------------------------------------------------------------------

void AudioParam::push(Kind kind, float v, double t) {
  if (count_ >= static_cast<int>(sizeof(events_) / sizeof(events_[0]))) return;
  // The timeline has to be non-decreasing for the lookup below to be a single walk.
  // Recipes schedule in order, but `attack` can exceed `dur` on a very short sound,
  // which would otherwise put the decay ramp before the attack ramp.
  if (count_ > 0 && t < events_[count_ - 1].time) t = events_[count_ - 1].time;
  events_[count_++] = Event{kind, v, t};
}

float AudioParam::interpolate(Kind kind, float v0, float v1, double t0, double t1, double t) {
  const double span = t1 - t0;
  if (span <= 0.0) return v1;
  const double u = (t - t0) / span;
  if (kind == Kind::ExpRamp) {
    // Web Audio forbids an exponential ramp through zero, which is why every recipe
    // ramps to 0.0001 rather than to silence. Guard anyway so a bad value degrades
    // to a linear ramp instead of producing a NaN that would latch into a filter.
    if (v0 > 0.0f && v1 > 0.0f) {
      return static_cast<float>(v0 * std::pow(static_cast<double>(v1) / v0, u));
    }
  }
  return static_cast<float>(v0 + (v1 - v0) * u);
}

float AudioParam::valueAt(double t) const {
  if (count_ == 0) return value_;

  // The last event that has already happened. Note `<=`: setValueAtTime(v, T) means
  // the parameter *is* v at exactly T. Getting that boundary wrong is not a rounding
  // detail — every voice starts at exactly its first event's time, so returning the
  // GainNode's default 1.0 there instead of the envelope's 0.0001 puts one render
  // quantum of full-scale signal at the head of every single sound.
  int i = -1;
  for (int k = 0; k < count_; ++k) {
    if (events_[k].time > t) break;
    i = k;
  }

  if (i < 0) {
    // Before the first event. A ramp with nothing in front of it ramps from the
    // value the param already held, starting when it was scheduled: Chrome's
    // behaviour for `param.value = x; param.exponentialRampToValueAtTime(y, T)`.
    const Event& first = events_[0];
    if (first.kind == Kind::SetValue) return value_;
    return interpolate(first.kind, value_, first.value, baseTime_, first.time, t);
  }
  if (i == count_ - 1) return events_[i].value;

  // A ramp interpolates from the previous event; a step just holds until it fires.
  const Event& next = events_[i + 1];
  if (next.kind == Kind::SetValue) return events_[i].value;
  return interpolate(next.kind, events_[i].value, next.value, events_[i].time, next.time, t);
}

// ---------------------------------------------------------------------------
// Panning
// ---------------------------------------------------------------------------

namespace {

inline void normalize3(float v[3]) {
  const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (len > 1e-12f) {
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
  }
}

inline void cross3(const float a[3], const float b[3], float out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

inline float dot3(const float a[3], const float b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

}  // namespace

PanGains equalPowerPan(const ListenerPose& listener, const float pos[3], float refDistance,
                       float maxDistance, float rolloff) {
  float toSource[3] = {pos[0] - listener.pos[0], pos[1] - listener.pos[1],
                       pos[2] - listener.pos[2]};
  const float distance =
      std::sqrt(toSource[0] * toSource[0] + toSource[1] * toSource[1] + toSource[2] * toSource[2]);

  // Inverse distance model, with maxDistance clamping the far end.
  float gain = 1.0f;
  if (refDistance > 0.0f) {
    const float d = std::max(std::min(distance, maxDistance), refDistance);
    gain = refDistance / (refDistance + rolloff * (d - refDistance));
  }

  if (distance < 1e-6f) return {gain * 0.7071068f, gain * 0.7071068f};

  toSource[0] /= distance;
  toSource[1] /= distance;
  toSource[2] /= distance;

  float right[3];
  float forward[3] = {listener.forward[0], listener.forward[1], listener.forward[2]};
  normalize3(forward);
  cross3(forward, listener.up, right);
  normalize3(right);
  float up[3];
  cross3(right, forward, up);

  // Project the source into the listener's horizontal plane before measuring the
  // azimuth, so a sound directly overhead pans to centre instead of swinging wildly.
  const float upProjection = dot3(toSource, up);
  float projected[3] = {toSource[0] - up[0] * upProjection, toSource[1] - up[1] * upProjection,
                        toSource[2] - up[2] * upProjection};
  const float projLen =
      std::sqrt(projected[0] * projected[0] + projected[1] * projected[1] + projected[2] * projected[2]);
  if (projLen < 1e-6f) return {gain * 0.7071068f, gain * 0.7071068f};
  projected[0] /= projLen;
  projected[1] /= projLen;
  projected[2] /= projLen;

  double azimuth = std::acos(clampd(dot3(projected, right), -1.0, 1.0)) * (180.0 / kPi);
  if (dot3(projected, forward) < 0.0f) azimuth = 360.0 - azimuth;
  // Measured from "right" up to here; Web Audio wants it from "front".
  azimuth = (azimuth >= 0.0 && azimuth <= 270.0) ? 90.0 - azimuth : 450.0 - azimuth;

  // Fold the rear hemisphere onto the front one: equal-power panning has no way to
  // express "behind you", so a source at 135 degrees pans the same as one at 45.
  azimuth = clampd(azimuth, -180.0, 180.0);
  if (azimuth < -90.0) azimuth = -180.0 - azimuth;
  else if (azimuth > 90.0) azimuth = 180.0 - azimuth;

  const double panPosition = (azimuth + 90.0) / 180.0;
  return {static_cast<float>(gain * std::cos(0.5 * kPi * panPosition)),
          static_cast<float>(gain * std::sin(0.5 * kPi * panPosition))};
}

// ---------------------------------------------------------------------------
// Compressor
// ---------------------------------------------------------------------------

float Compressor::curveDb(float inputDb) const {
  // The knee runs from the threshold UPWARD by `knee` dB, not symmetrically around
  // it. That is Blink's convention (`kneeThresholdDb = thresholdDb + kneeDb`), and
  // with the game's -14 dB / 18 dB settings it is the difference between a gentle
  // curve across the whole useful range and a hard 5:1 wall above -5 dBFS.
  const float over = inputDb - thresholdDb_;
  if (over <= 0.0f) return inputDb;
  if (over >= kneeDb_) {
    // Past the knee, the slope is 1/ratio. Anchor it to the knee's own end so the
    // curve stays continuous.
    const float kneeTop = thresholdDb_ + kneeDb_;
    const float yKneeTop = kneeTop + (1.0f / ratio_ - 1.0f) * kneeDb_ * 0.5f;
    return yKneeTop + (inputDb - kneeTop) / ratio_;
  }
  // Quadratic across the knee: value and slope both continuous at each end, which is
  // what stops a compressor chattering on material sitting right at the threshold.
  // Checked against Blink's exponential KneeCurve (with its k solved for a 1/ratio
  // slope at the knee top) at -14, -6 and 0 dBFS: agreement within 0.2 dB.
  return inputDb + (1.0f / ratio_ - 1.0f) * over * over / (2.0f * kneeDb_);
}

void Compressor::configure(float thresholdDb, float kneeDb, float ratio, float attackSec,
                           float releaseSec, int sampleRate) {
  thresholdDb_ = thresholdDb;
  kneeDb_ = std::max(0.001f, kneeDb);
  ratio_ = std::max(1.0f, ratio);
  attackCoef_ = std::exp(-1.0f / std::max(1e-4f, attackSec) / sampleRate);
  releaseCoef_ = std::exp(-1.0f / std::max(1e-4f, releaseSec) / sampleRate);

  // Blink's empirical makeup: the reciprocal of the gain a full-scale input would
  // receive, raised to 0.6 so a heavy ratio does not turn into a loudness war.
  const float fullRangeGain = dbToLinear(curveDb(0.0f));
  makeup_ = std::pow(1.0f / std::max(1e-6f, fullRangeGain), 0.6f);

  delayLen_ = std::max(1, static_cast<int>(0.006f * sampleRate));  // 6 ms lookahead
  delay_[0].assign(delayLen_, 0.0f);
  delay_[1].assign(delayLen_, 0.0f);
  delayWrite_ = 0;
  envelope_ = 1.0f;
}

void Compressor::reset() {
  std::fill(delay_[0].begin(), delay_[0].end(), 0.0f);
  std::fill(delay_[1].begin(), delay_[1].end(), 0.0f);
  delayWrite_ = 0;
  envelope_ = 1.0f;
  reductionDb_ = 0.0f;
}

void Compressor::process(float* left, float* right, int frames) {
  if (bypass_ || delayLen_ <= 0) return;

  for (int i = 0; i < frames; ++i) {
    const float inL = left[i];
    const float inR = right[i];

    // The detector reads the *undelayed* signal; the output reads the delayed one.
    const float peak = std::max(std::fabs(inL), std::fabs(inR));
    const float inDb = linearToDb(peak);
    const float target = dbToLinear(curveDb(inDb) - inDb);
    const float clamped = std::min(1.0f, target);
    const float coef = clamped < envelope_ ? attackCoef_ : releaseCoef_;
    envelope_ = clamped + (envelope_ - clamped) * coef;

    const float outL = delay_[0][delayWrite_];
    const float outR = delay_[1][delayWrite_];
    delay_[0][delayWrite_] = inL;
    delay_[1][delayWrite_] = inR;
    delayWrite_ = (delayWrite_ + 1) % delayLen_;

    // Blink applies the square root of the tracked gain ("warp pre-compression gain
    // to smooth out sharp exponential transition points"), so its compressor reduces
    // far less than the static curve alone implies. Without this the mix comes out
    // audibly squashed compared with the browser — and, since the makeup gain is
    // derived from the *un*warped curve, a full-scale input can still leave this
    // node above 0 dBFS, which is also what Chrome does.
    const float g = std::sqrt(envelope_) * makeup_;
    left[i] = outL * g;
    right[i] = outR * g;
  }
  reductionDb_ = linearToDb(envelope_);
}

}  // namespace hr::audio
