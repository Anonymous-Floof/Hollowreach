#include "audio/engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "miniaudio.h"

namespace hr::audio {
namespace {

constexpr double kTwoPi = 6.28318530717958647692;

// PannerNode settings from AudioEngine.out(): inaudible past about 26 blocks.
constexpr float kRefDistance = 3.0f;
constexpr float kMaxDistance = 60.0f;
constexpr float kRolloff = 1.6f;

// The cave drone's three detuned partials (js/audio/ambience.js:57-63).
constexpr double kCaveFreq[3] = {51.5, 52.2, 103.7};
constexpr float kCaveOscGain[3] = {0.16f, 0.16f, 0.06f};

// Sliders feel linear-ish in loudness when squared, matching applyVolumes().
inline float volumeCurve(float x) { return x * x; }

}  // namespace

// ---------------------------------------------------------------------------
// Cross-thread plumbing
// ---------------------------------------------------------------------------

struct Engine::Command {
  enum class Type : std::uint8_t {
    Burst,
    Tone,
    Sample,
    Volumes,
    MasterTarget,
    MuffleTarget,
    Listener,
    BedGain,
    WindFilter,
  };

  Type type = Type::Burst;
  Dest dest;
  double scheduleTime = 0.0;
  BurstOpts burst;
  ToneOpts tone;
  SampleOpts sample;
  float a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
};

// Single-producer / single-consumer. The game thread is the only producer (sfx,
// ambience and the director all run there); the audio callback is the only
// consumer. A full ring drops the command, which is the same outcome as the voice
// cap refusing the sound.
class Engine::Ring {
 public:
  static constexpr int kCapacity = 512;

  bool push(const Command& cmd) {
    const int w = write_.load(std::memory_order_relaxed);
    const int next = (w + 1) % kCapacity;
    if (next == read_.load(std::memory_order_acquire)) return false;
    items_[w] = cmd;
    write_.store(next, std::memory_order_release);
    return true;
  }

  bool pop(Command& out) {
    const int r = read_.load(std::memory_order_relaxed);
    if (r == write_.load(std::memory_order_acquire)) return false;
    out = items_[r];
    read_.store((r + 1) % kCapacity, std::memory_order_release);
    return true;
  }

 private:
  Command items_[kCapacity];
  std::atomic<int> write_{0};
  std::atomic<int> read_{0};
};

struct Engine::Voice {
  // What feeds the filter and gain chain. Was a bool while there were only two;
  // a resource pack's clip is a third, and it reads its samples from memory the
  // SoundBank owns rather than from a table this engine built.
  enum class Source : std::uint8_t { Noise, Tone, Sample };

  bool active = false;
  Source source = Source::Noise;
  std::int64_t startFrame = 0;
  std::int64_t endFrame = 0;

  Bus bus = Bus::Sfx;
  bool positional = false;
  float pos[3] = {0, 0, 0};

  // Noise source.
  NoiseKind noise = NoiseKind::White;
  double noisePos = 0.0;
  double noiseRate = 1.0;
  double loopStart = 0.0;

  // Sample source. `samples` points into SoundBank's append-only arena, which is
  // what makes holding a raw pointer across the thread boundary safe: nothing in
  // that arena is ever freed while the engine is running.
  const float* samples = nullptr;
  int sampleCount = 0;
  double samplePos = 0.0;
  double sampleStep = 1.0;

  // Oscillator source.
  Wave wave = Wave::Sine;
  double oscPhase = 0.0;
  AudioParam freq;

  AudioParam gain;
  AudioParam filterFreq[2];
  FilterType filterType[2] = {FilterType::Bandpass, FilterType::Bandpass};
  double filterQ[2] = {1.0, 1.0};
  int filterCount = 0;
  Biquad filt[2];

  double vibPhase = 0.0, vibRate = 0.0, vibDepth = 0.0;
  double tremPhase = 0.0, tremRate = 0.0, tremGain = 0.0;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Engine::Engine() : ring_(std::make_unique<Ring>()) {}

Engine::~Engine() { shutdown(); }

Engine& engine() {
  static Engine instance;
  return instance;
}

void Engine::buildTables(int sampleRate) {
  sampleRate_ = sampleRate;
  noise_.build(sampleRate);
  waves_.build(sampleRate);
  // A fresh clock, an empty mix and an empty event ledger, so starting the engine
  // twice — which only the offline renderer does — really is a fresh start.
  renderedFrames_ = 0;
  scheduleTime_.store(0.0, std::memory_order_relaxed);
  eventExpiry_.clear();
  voices_.assign(kMaxVoices, Voice{});
  for (auto& s : scratch_) s.assign(kBlockFrames, 0.0f);

  comp_.configure(-14.0f, 18.0f, 5.0f, 0.004f, 0.18f, sampleRate);
  caveFilter_.set(FilterType::Lowpass, 160.0, 0.6, sampleRate);

  masterGain_.snap(volumeCurve(volumes_[0]));
  muffleFreq_.snap(19000.0f);
  windGain_.snap(0.0f);
  windFreq_.snap(400.0f);
  caveGain_.snap(0.0f);
  cavePos_ = 0.7 * sampleRate;  // the cave murmur's loopStart
  for (auto& s : listenerSmooth_) s.snap(0.0f);
  listenerSmooth_[5].snap(-1.0f);
  setVolumes(volumes_[0], volumes_[1], volumes_[2], volumes_[3]);
}

bool Engine::start() {
  if (running_.load()) return true;

  auto* device = new ma_device();
  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.format = ma_format_f32;
  cfg.playback.channels = 2;
  cfg.sampleRate = 48000;
  // Small periods keep `now()` close to real time, which is what preserves the
  // millisecond-scale stagger the recipes are built out of.
  cfg.periodSizeInFrames = 256;
  cfg.periods = 3;
  cfg.pUserData = this;
  cfg.dataCallback = [](ma_device* dev, void* output, const void*, ma_uint32 frames) {
    static_cast<Engine*>(dev->pUserData)->renderInterleaved(static_cast<float*>(output),
                                                            static_cast<int>(frames));
  };

  if (ma_device_init(nullptr, &cfg, device) != MA_SUCCESS) {
    log::warn("audio: no output device; the game runs silent");
    delete device;
    return false;
  }

  buildTables(static_cast<int>(device->sampleRate));
  lookaheadFrames_ = static_cast<int>(device->playback.internalPeriodSizeInFrames);
  device_ = device;

  if (ma_device_start(device) != MA_SUCCESS) {
    log::warn("audio: failed to start the output device");
    ma_device_uninit(device);
    delete device;
    device_ = nullptr;
    return false;
  }

  running_.store(true, std::memory_order_release);
  log::info("audio: %s, %d Hz, %d-frame periods", device->playback.name, sampleRate_,
            static_cast<int>(device->playback.internalPeriodSizeInFrames));
  return true;
}

void Engine::shutdown() {
  if (!device_) {
    running_.store(false);
    return;
  }
  running_.store(false, std::memory_order_release);
  auto* device = static_cast<ma_device*>(device_);
  ma_device_uninit(device);
  delete device;
  device_ = nullptr;
}

void Engine::startOffline(int sampleRate) {
  buildTables(sampleRate);
  lookaheadFrames_ = 0;
  running_.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Mixer controls (game thread)
// ---------------------------------------------------------------------------

void Engine::post(const Command& cmd) { ring_->push(cmd); }

void Engine::setVolumes(float master, float sfx, float ambient, float ui) {
  volumes_[0] = master;
  volumes_[1] = sfx;
  volumes_[2] = ambient;
  volumes_[3] = ui;

  Command c;
  c.type = Command::Type::Volumes;
  c.a = volumeCurve(master) * duck_;
  c.b = volumeCurve(sfx);
  c.c = volumeCurve(ambient);
  // The UI bus bypasses the master node entirely, so it folds master in itself.
  c.d = volumeCurve(ui) * volumeCurve(master);
  post(c);
}

void Engine::setDucked(bool on) {
  const float duck = on ? 0.35f : 1.0f;
  if (duck == duck_) return;  // called every frame from the state machine
  duck_ = duck;
  Command c;
  c.type = Command::Type::MasterTarget;
  c.a = volumeCurve(volumes_[0]) * duck_;
  c.b = 0.15f;
  post(c);
}

void Engine::setUnderwater(float t) {
  // The same deadband the JS used, so surface bobbing does not retrigger the sweep
  // every frame.
  if (std::fabs(t - underwater_) < 0.01f) return;
  underwater_ = t;
  Command c;
  c.type = Command::Type::MuffleTarget;
  c.a = static_cast<float>(19000.0 * std::pow(750.0 / 19000.0, t));  // 19k -> 750 Hz
  c.b = 0.12f;
  post(c);
}

void Engine::updateListener(const Vec3& pos, float yaw) {
  Command c;
  c.type = Command::Type::Listener;
  c.a = pos.x;
  c.b = pos.y;
  c.c = pos.z;
  c.d = -std::sin(yaw);
  c.e = 0.0f;
  c.f = -std::cos(yaw);
  post(c);
}

void Engine::setBedGain(Bed bed, float value, float tau) {
  Command c;
  c.type = Command::Type::BedGain;
  c.a = static_cast<float>(bed);
  c.b = value;
  c.c = tau;
  post(c);
}

void Engine::setWindFilter(float freq, float tau) {
  Command c;
  c.type = Command::Type::WindFilter;
  c.a = freq;
  c.b = tau;
  post(c);
}

void Engine::quietBeds() {
  setBedGain(Bed::Wind, 0.0f, 0.3f);
  setBedGain(Bed::Cave, 0.0f, 0.3f);
}

bool Engine::tryVoice(float dur) {
  // The JS reaped a voice with `setTimeout(..., dur * 1000 + 120)`, i.e. against
  // wall time. Using the engine's own clock instead is the same thing while a device
  // is running, and it is the only version that behaves when the mix is being
  // rendered offline faster than real time.
  const double t = now();
  eventExpiry_.erase(std::remove_if(eventExpiry_.begin(), eventExpiry_.end(),
                                    [t](double deadline) { return deadline <= t; }),
                     eventExpiry_.end());
  if (static_cast<int>(eventExpiry_.size()) >= kMaxEvents) return false;
  eventExpiry_.push_back(t + dur + 0.12);
  return true;
}

Dest Engine::out(Bus bus) {
  Dest d;
  d.bus = bus;
  d.valid = true;
  return d;
}

Dest Engine::out(Bus bus, const Vec3& pos) {
  Dest d;
  d.bus = bus;
  d.positional = true;
  d.valid = true;
  d.pos[0] = pos.x;
  d.pos[1] = pos.y;
  d.pos[2] = pos.z;
  return d;
}

void Engine::burst(const Dest& dest, const BurstOpts& opts) {
  if (!dest.valid || !ready()) return;
  Command c;
  c.type = Command::Type::Burst;
  c.dest = dest;
  c.scheduleTime = now();
  c.burst = opts;
  post(c);
}

void Engine::sample(const Dest& dest, const SampleOpts& opts) {
  if (!dest.valid || !ready()) return;
  if (!opts.samples || opts.frameCount <= 0 || opts.sampleRate <= 0) return;
  Command c;
  c.type = Command::Type::Sample;
  c.dest = dest;
  c.scheduleTime = now();
  c.sample = opts;
  post(c);
}

void Engine::tone(const Dest& dest, const ToneOpts& opts) {
  if (!dest.valid || !ready()) return;
  Command c;
  c.type = Command::Type::Tone;
  c.dest = dest;
  c.scheduleTime = now();
  c.tone = opts;
  post(c);
}

// ---------------------------------------------------------------------------
// Voice construction (audio thread)
// ---------------------------------------------------------------------------

int Engine::allocateVoice() {
  int oldest = 0;
  std::int64_t oldestEnd = INT64_MAX;
  for (int i = 0; i < static_cast<int>(voices_.size()); ++i) {
    if (!voices_[i].active) return i;
    if (voices_[i].endFrame < oldestEnd) {
      oldestEnd = voices_[i].endFrame;
      oldest = i;
    }
  }
  return oldest;  // steal the one closest to finishing
}

void Engine::startBurst(const Command& cmd) {
  const BurstOpts& o = cmd.burst;
  const double dur = o.dur > 0.0f ? o.dur : 0.1;
  const double t0 = cmd.scheduleTime + o.delay;

  Voice& v = voices_[allocateVoice()];
  v = Voice{};
  v.active = true;
  v.source = Voice::Source::Noise;
  v.bus = cmd.dest.bus;
  v.positional = cmd.dest.positional;
  std::memcpy(v.pos, cmd.dest.pos, sizeof(v.pos));

  // CEIL, not round. A voice's first rendered frame is where its envelope is
  // evaluated, and the envelope's first event sits at exactly t0 — so a start frame
  // that rounds *down* below t0 asks the parameter for its value before the
  // timeline begins, which is correctly the GainNode's default of 1.0, and lands one
  // render quantum of full-scale signal at the head of the sound. It is a coin flip
  // per voice on whether t0 * sampleRate has a fractional part, which is why it only
  // showed on recipes whose delays come out of R(): the splash's droplets, the
  // stone-break rubble, the three bites of eat.
  v.startFrame = static_cast<std::int64_t>(std::ceil(t0 * sampleRate_));
  v.endFrame = static_cast<std::int64_t>(std::ceil((t0 + dur + 0.05) * sampleRate_));
  if (v.startFrame < renderedFrames_) v.startFrame = renderedFrames_;

  v.noise = o.noise;
  v.noiseRate = o.rate > 0.0f ? o.rate : 1.0;
  // `src.loopStart = Math.random() * 1.2` in the JS is a no-op for a one-shot: the
  // source is started with no offset, so it plays from sample 0 and a burst is far
  // too short to ever reach the end of the 2-second buffer and wrap. Reproduced
  // rather than "fixed", because the browser is the A/B reference — starting each
  // burst at a random offset (the comment's stated intent) would make the native
  // build audibly more varied than the thing it is supposed to match.
  v.noisePos = 0.0;
  v.loopStart = 0.0;

  const float peak = o.gain;
  v.gain.reset(1.0f, cmd.scheduleTime);
  v.gain.setValueAtTime(0.0001f, t0);
  v.gain.linearRampToValueAtTime(peak, t0 + (o.attack > 0.0f ? o.attack : 0.003f));
  if (o.curve == Curve::Lin) v.gain.linearRampToValueAtTime(0.0001f, t0 + dur);
  else v.gain.exponentialRampToValueAtTime(0.0001f, t0 + dur);

  v.filterCount = std::min(2, o.filters.count);
  for (int k = 0; k < v.filterCount; ++k) {
    const FilterSpec& f = o.filters.stages[k];
    v.filterType[k] = f.type;
    v.filterQ[k] = f.q;
    // No setValueAtTime here: the JS assigns `bq.frequency.value` and then schedules
    // a bare ramp, which Chrome starts from that value at the moment it was
    // scheduled — before the burst's own delay, not at it.
    v.filterFreq[k].reset(f.freq, cmd.scheduleTime);
    if (f.sweepTo > 0.0f) {
      v.filterFreq[k].exponentialRampToValueAtTime(
          std::max(20.0f, f.sweepTo), t0 + (f.sweepT > 0.0f ? f.sweepT : dur));
    }
    v.filt[k].reset();
  }
}

void Engine::startTone(const Command& cmd) {
  const ToneOpts& o = cmd.tone;
  const double dur = o.dur > 0.0f ? o.dur : 0.15;
  const double t0 = cmd.scheduleTime + o.delay;

  Voice& v = voices_[allocateVoice()];
  v = Voice{};
  v.active = true;
  v.source = Voice::Source::Tone;
  v.bus = cmd.dest.bus;
  v.positional = cmd.dest.positional;
  std::memcpy(v.pos, cmd.dest.pos, sizeof(v.pos));

  // CEIL, not round. A voice's first rendered frame is where its envelope is
  // evaluated, and the envelope's first event sits at exactly t0 — so a start frame
  // that rounds *down* below t0 asks the parameter for its value before the
  // timeline begins, which is correctly the GainNode's default of 1.0, and lands one
  // render quantum of full-scale signal at the head of the sound. It is a coin flip
  // per voice on whether t0 * sampleRate has a fractional part, which is why it only
  // showed on recipes whose delays come out of R(): the splash's droplets, the
  // stone-break rubble, the three bites of eat.
  v.startFrame = static_cast<std::int64_t>(std::ceil(t0 * sampleRate_));
  v.endFrame = static_cast<std::int64_t>(std::ceil((t0 + dur + 0.05) * sampleRate_));
  if (v.startFrame < renderedFrames_) v.startFrame = renderedFrames_;

  v.wave = o.wave;
  v.freq.reset(o.freq, cmd.scheduleTime);
  v.freq.setValueAtTime(o.freq, t0);
  if (o.sweepTo > 0.0f) {
    v.freq.exponentialRampToValueAtTime(std::max(20.0f, o.sweepTo),
                                        t0 + (o.sweepT > 0.0f ? o.sweepT : dur));
  }

  const float peak = o.gain;
  v.gain.reset(1.0f, cmd.scheduleTime);
  v.gain.setValueAtTime(0.0001f, t0);
  v.gain.linearRampToValueAtTime(peak, t0 + (o.attack > 0.0f ? o.attack : 0.005f));
  v.gain.exponentialRampToValueAtTime(0.0001f, t0 + dur);

  // An LFO connected to an AudioParam SUMS into it. So the tremolo that turns a
  // sawtooth into a bleat swings the gain both above and below the envelope, and
  // the vibrato adds hertz rather than scaling them.
  v.vibRate = o.vibRate;
  v.vibDepth = o.vibDepth;
  v.tremRate = o.tremRate;
  v.tremGain = peak * o.tremDepth;

  v.filterCount = std::min(1, o.filter.count);
  if (v.filterCount > 0) {
    const FilterSpec& f = o.filter.stages[0];
    v.filterType[0] = f.type;
    v.filterQ[0] = f.q;
    v.filterFreq[0].reset(f.freq, cmd.scheduleTime);
    v.filterFreq[0].setValueAtTime(f.freq, t0);
    if (f.sweepTo > 0.0f) {
      v.filterFreq[0].exponentialRampToValueAtTime(
          std::max(20.0f, f.sweepTo), t0 + (f.sweepT > 0.0f ? f.sweepT : dur));
    }
    v.filt[0].reset();
  }
}

void Engine::startSample(const Command& cmd) {
  const SampleOpts& o = cmd.sample;
  if (!o.samples || o.frameCount <= 0 || o.sampleRate <= 0) return;
  const double t0 = cmd.scheduleTime + o.delay;

  // The device's rate conversion and the requested pitch are one multiply. A
  // 44.1 kHz clip on a 48 kHz device steps at 0.919 per output frame; asking for
  // 1.2x pitch makes it 1.103. Both are the same operation, so neither needs a
  // resampling pass at load.
  const double step =
      (static_cast<double>(o.sampleRate) / sampleRate_) * (o.pitch > 0.0f ? o.pitch : 1.0f);
  const double dur = (o.frameCount / step) / sampleRate_;

  Voice& v = voices_[allocateVoice()];
  v = Voice{};
  v.active = true;
  v.source = Voice::Source::Sample;
  v.bus = cmd.dest.bus;
  v.positional = cmd.dest.positional;
  std::memcpy(v.pos, cmd.dest.pos, sizeof(v.pos));

  v.startFrame = static_cast<std::int64_t>(std::ceil(t0 * sampleRate_));
  // No +0.05 tail here, unlike a burst or a tone: those two ramp their gain down
  // to 0.0001 rather than to zero and need room to finish, while a clip ends when
  // its last sample has been read and anything past that is silence.
  v.endFrame = static_cast<std::int64_t>(std::ceil((t0 + dur) * sampleRate_));
  if (v.startFrame < renderedFrames_) v.startFrame = renderedFrames_;

  v.samples = o.samples;
  v.sampleCount = o.frameCount;
  v.samplePos = 0.0;
  v.sampleStep = step;

  // A clip plays at a constant gain — it is a recording, and re-enveloping it
  // would be shaping a sound the pack author already shaped. The only automation
  // is a short release, because a file that does not end at zero crossing clicks
  // when it stops, and plenty of hand-trimmed files do not.
  const float peak = o.gain;
  const double release = std::min(0.005, dur * 0.25);
  v.gain.reset(peak, cmd.scheduleTime);
  v.gain.setValueAtTime(peak, t0);
  v.gain.linearRampToValueAtTime(peak, t0 + dur - release);
  v.gain.linearRampToValueAtTime(0.0f, t0 + dur);

  v.filterCount = 0;
}

void Engine::drainCommands() {
  Command cmd;
  while (ring_->pop(cmd)) {
    switch (cmd.type) {
      case Command::Type::Burst: startBurst(cmd); break;
      case Command::Type::Tone: startTone(cmd); break;
      case Command::Type::Sample: startSample(cmd); break;
      case Command::Type::Volumes:
        masterGain_.setTarget(cmd.a, 0.0f);
        busGain_[0] = cmd.b;
        busGain_[1] = cmd.c;
        busGain_[2] = cmd.d;
        break;
      case Command::Type::MasterTarget: masterGain_.setTarget(cmd.a, cmd.b); break;
      case Command::Type::MuffleTarget: muffleFreq_.setTarget(cmd.a, cmd.b); break;
      case Command::Type::Listener:
        listenerSmooth_[0].setTarget(cmd.a, 0.03f);
        listenerSmooth_[1].setTarget(cmd.b, 0.03f);
        listenerSmooth_[2].setTarget(cmd.c, 0.03f);
        listenerSmooth_[3].setTarget(cmd.d, 0.03f);
        listenerSmooth_[4].setTarget(cmd.e, 0.03f);
        listenerSmooth_[5].setTarget(cmd.f, 0.03f);
        break;
      case Command::Type::BedGain:
        (static_cast<Bed>(static_cast<int>(cmd.a)) == Bed::Wind ? windGain_ : caveGain_)
            .setTarget(cmd.b, cmd.c);
        break;
      case Command::Type::WindFilter: windFreq_.setTarget(cmd.a, cmd.b); break;
    }
  }
}

// ---------------------------------------------------------------------------
// Rendering (audio thread)
// ---------------------------------------------------------------------------

void Engine::renderVoice(Voice& v, float* sfxL, float* sfxR, float* ambL, float* ambR, float* uiL,
                         float* uiR, int frames) {
  const std::int64_t blockStart = renderedFrames_;
  if (blockStart >= v.endFrame) {
    v.active = false;
    return;
  }
  const std::int64_t s0 = std::max(v.startFrame, blockStart);
  const std::int64_t s1 = std::min(v.endFrame, blockStart + frames);
  if (s1 <= s0) return;  // scheduled, but not this block

  const int i0 = static_cast<int>(s0 - blockStart);
  const int i1 = static_cast<int>(s1 - blockStart);
  const double sr = sampleRate_;
  const double ta = s0 / sr;
  const double tb = s1 / sr;

  // Automation is evaluated at the block edges and interpolated across it, which is
  // Web Audio's grid: a-rate params are computed a render quantum at a time.
  const float gainA = v.gain.valueAt(ta);
  const float gainB = v.gain.valueAt(tb);
  float freqA = 0.0f, freqB = 0.0f;
  PeriodicWave::Reader reader;
  const PeriodicWave* wave = nullptr;
  if (v.source == Voice::Source::Tone) {
    freqA = v.freq.valueAt(ta);
    freqB = v.freq.valueAt(tb);
    wave = &waves_.wave(v.wave);
    reader = wave->reader(0.5 * (freqA + freqB));
  }
  // A swept filter gets its coefficients interpolated across the block; a static one
  // is designed once. Holding a sweeping filter's coefficients fixed for a whole
  // quantum steps them 375 times a second, and that is what turned the splash and
  // the wade step from wet into gritty.
  Biquad::Coeffs coefA[2], coefB[2];
  bool sweeping[2] = {false, false};
  for (int k = 0; k < v.filterCount; ++k) {
    coefA[k] = Biquad::design(v.filterType[k], v.filterFreq[k].valueAt(ta), v.filterQ[k], sr);
    sweeping[k] = v.filterFreq[k].automated();
    if (sweeping[k]) {
      coefB[k] = Biquad::design(v.filterType[k], v.filterFreq[k].valueAt(tb), v.filterQ[k], sr);
    }
  }

  PanGains pan{1.0f, 1.0f};
  if (v.positional) {
    pan = equalPowerPan(listener_, v.pos, kRefDistance, kMaxDistance, kRolloff);
  }

  float* dstL = sfxL;
  float* dstR = sfxR;
  if (v.bus == Bus::Amb) {
    dstL = ambL;
    dstR = ambR;
  } else if (v.bus == Bus::Ui) {
    dstL = uiL;
    dstR = uiR;
  }

  const std::vector<float>* buffer =
      v.source == Voice::Source::Noise ? &noise_.buffer(v.noise) : nullptr;
  const double bufferLen = buffer ? static_cast<double>(buffer->size()) : 0.0;
  const double tableScale = wave ? wave->tableSize() / sr : 0.0;
  const double tableSize = wave ? wave->tableSize() : 1.0;
  const double invSpan = (i1 > i0) ? 1.0 / static_cast<double>(i1 - i0) : 0.0;

  for (int i = i0; i < i1; ++i) {
    const double u = (i - i0) * invSpan;
    float x;
    if (v.source == Voice::Source::Tone) {
      double f = freqA + (freqB - freqA) * u;
      if (v.vibRate > 0.0) {
        f += v.vibDepth * std::sin(kTwoPi * v.vibPhase);
        v.vibPhase += v.vibRate / sr;
        if (v.vibPhase >= 1.0) v.vibPhase -= 1.0;
      }
      x = PeriodicWave::read(reader, v.oscPhase);
      v.oscPhase += f * tableScale;
      while (v.oscPhase >= tableSize) v.oscPhase -= tableSize;
      while (v.oscPhase < 0.0) v.oscPhase += tableSize;
    } else if (v.source == Voice::Source::Sample) {
      // Linearly interpolated, not nearest-neighbour. The step is almost never
      // 1.0 — a 44.1 kHz clip on a 48 kHz device runs at 0.919, and any pitch
      // at all moves it further — so nearest-neighbour would resample every clip
      // in the pack with audible aliasing on the transients, which for a pack of
      // short percussive hits is the whole content.
      const int i_ = static_cast<int>(v.samplePos);
      if (i_ >= v.sampleCount) {
        x = 0.0f;
      } else {
        const float a = v.samples[i_];
        const float b = (i_ + 1 < v.sampleCount) ? v.samples[i_ + 1] : 0.0f;
        x = a + (b - a) * static_cast<float>(v.samplePos - i_);
      }
      v.samplePos += v.sampleStep;
    } else {
      auto idx = static_cast<std::size_t>(v.noisePos);
      if (idx >= buffer->size()) idx = buffer->size() - 1;
      x = (*buffer)[idx];
      v.noisePos += v.noiseRate;
      if (v.noisePos >= bufferLen) v.noisePos -= (bufferLen - v.loopStart);
    }

    for (int k = 0; k < v.filterCount; ++k) {
      x = sweeping[k] ? v.filt[k].process(x, Biquad::lerp(coefA[k], coefB[k], u))
                      : v.filt[k].process(x, coefA[k]);
    }

    float g = gainA + (gainB - gainA) * static_cast<float>(u);
    if (v.tremRate > 0.0) {
      g += static_cast<float>(v.tremGain * std::sin(kTwoPi * v.tremPhase));
      v.tremPhase += v.tremRate / sr;
      if (v.tremPhase >= 1.0) v.tremPhase -= 1.0;
    }

    const float s = x * g;
    dstL[i] += s * pan.left;
    dstR[i] += s * pan.right;
  }
}

void Engine::renderBeds(float* ambL, float* ambR, int frames) {
  // Every smoothed value is read at both edges of the block and interpolated across
  // it. The beds are sustained noise, which is where a stepped gain or a stepped
  // filter is most obvious — a bed that eases over 0.8 s still moves enough per
  // quantum to buzz if it is applied as 375 steps a second rather than a ramp.
  const float dt = static_cast<float>(frames) / sampleRate_;
  const float windG0 = windGain_.value();
  const float windF0 = windFreq_.value();
  const float caveG0 = caveGain_.value();
  const float windG1 = windGain_.advance(dt);
  const float windF1 = windFreq_.advance(dt);
  const float caveG1 = caveGain_.advance(dt);

  const std::vector<float>& pink = noise_.buffer(NoiseKind::Pink);
  if (pink.empty()) return;
  const double len = static_cast<double>(pink.size());
  const double caveLoopStart = 0.7 * sampleRate_;

  const Biquad::Coeffs windA = Biquad::design(FilterType::Bandpass, windF0, 0.45, sampleRate_);
  const Biquad::Coeffs windB = Biquad::design(FilterType::Bandpass, windF1, 0.45, sampleRate_);
  const float invFrames = 1.0f / static_cast<float>(frames);

  const PeriodicWave& tri = waves_.wave(Wave::Triangle);
  PeriodicWave::Reader caveReader[3];
  double caveInc[3];
  for (int k = 0; k < 3; ++k) {
    caveReader[k] = tri.reader(kCaveFreq[k]);
    caveInc[k] = kCaveFreq[k] * tri.tableSize() / sampleRate_;
  }

  for (int i = 0; i < frames; ++i) {
    const float u = i * invFrames;

    // Wind: pink noise through a slowly wandering bandpass.
    float wind = pink[static_cast<std::size_t>(windPos_)];
    windPos_ += 1.0;
    if (windPos_ >= len) windPos_ -= len;
    wind = windFilter_.process(wind, Biquad::lerp(windA, windB, u)) * (windG0 + (windG1 - windG0) * u);

    // Cave: a detuned triangle drone over a lowpassed murmur.
    float cave = 0.0f;
    for (int k = 0; k < 3; ++k) {
      cave += kCaveOscGain[k] * PeriodicWave::read(caveReader[k], caveOscPhase_[k]);
      caveOscPhase_[k] += caveInc[k];
      if (caveOscPhase_[k] >= tri.tableSize()) caveOscPhase_[k] -= tri.tableSize();
    }
    float murmur = pink[static_cast<std::size_t>(cavePos_)];
    cavePos_ += 1.0;
    if (cavePos_ >= len) cavePos_ -= (len - caveLoopStart);
    cave += caveFilter_.process(murmur) * 0.4f;
    cave *= caveG0 + (caveG1 - caveG0) * u;

    // Both beds are mono, and a mono signal fans out to both channels at full gain
    // — no 0.707 here, unlike anything that went through a panner.
    ambL[i] += wind + cave;
    ambR[i] += wind + cave;
  }
}

void Engine::renderBlock(float* outL, float* outR, int frames) {
  float* sfxL = scratch_[0].data();
  float* sfxR = scratch_[1].data();
  float* ambL = scratch_[2].data();
  float* ambR = scratch_[3].data();
  float* uiL = scratch_[4].data();
  float* uiR = scratch_[5].data();
  for (int b = 0; b < 6; ++b) std::fill_n(scratch_[b].data(), frames, 0.0f);

  const float dt = static_cast<float>(frames) / sampleRate_;
  listener_.pos[0] = listenerSmooth_[0].advance(dt);
  listener_.pos[1] = listenerSmooth_[1].advance(dt);
  listener_.pos[2] = listenerSmooth_[2].advance(dt);
  listener_.forward[0] = listenerSmooth_[3].advance(dt);
  listener_.forward[1] = listenerSmooth_[4].advance(dt);
  listener_.forward[2] = listenerSmooth_[5].advance(dt);

  int live = 0;
  for (auto& v : voices_) {
    if (!v.active) continue;
    renderVoice(v, sfxL, sfxR, ambL, ambR, uiL, uiR, frames);
    if (v.active) ++live;
  }
  activeVoices_.store(live, std::memory_order_relaxed);

  renderBeds(ambL, ambR, frames);

  // Both of these are swept, so both are interpolated across the block rather than
  // stepped at its edge: the duck eases over 150 ms and the underwater muffle over
  // 120 ms, which is fast enough that a per-quantum step would buzz.
  const float master0 = masterGain_.value();
  const float muffle0 = muffleFreq_.value();
  const float master1 = masterGain_.advance(dt);
  const float muffle1 = muffleFreq_.advance(dt);
  const Biquad::Coeffs muffleA = Biquad::design(FilterType::Lowpass, muffle0, 0.5, sampleRate_);
  const Biquad::Coeffs muffleB = Biquad::design(FilterType::Lowpass, muffle1, 0.5, sampleRate_);
  const float invFrames = 1.0f / static_cast<float>(frames);

  for (int i = 0; i < frames; ++i) {
    const float u = i * invFrames;
    const float master = master0 + (master1 - master0) * u;
    const Biquad::Coeffs c = Biquad::lerp(muffleA, muffleB, u);
    const float l = (sfxL[i] * busGain_[0] + ambL[i] * busGain_[1]) * master;
    const float r = (sfxR[i] * busGain_[0] + ambR[i] * busGain_[1]) * master;
    outL[i] = muffle_[0].process(l, c) + uiL[i] * busGain_[2];
    outR[i] = muffle_[1].process(r, c) + uiR[i] * busGain_[2];
  }

  comp_.process(outL, outR, frames);
}

void Engine::renderInterleaved(float* interleaved, int frames) {
  drainCommands();

  float* outL = scratch_[6].data();
  float* outR = scratch_[7].data();

  int done = 0;
  while (done < frames) {
    const int n = std::min(kBlockFrames, frames - done);
    renderBlock(outL, outR, n);
    for (int i = 0; i < n; ++i) {
      interleaved[(done + i) * 2 + 0] = outL[i];
      interleaved[(done + i) * 2 + 1] = outR[i];
    }
    renderedFrames_ += n;
    done += n;
  }

  // Publish, at the end, the time a command posted from here on should be scheduled
  // against — the render head plus the lookahead. That lookahead is what keeps a
  // recipe's relative delays intact: without it a command posted mid-callback comes
  // back with a start time already behind the render head, gets clamped to "now",
  // and collapses onto everything else that was clamped alongside it. (Its start
  // gets clamped, note, but not its end, so a badly stale schedule drops the sound
  // rather than replaying it late — which is what Web Audio does too.)
  scheduleTime_.store(static_cast<double>(renderedFrames_ + lookaheadFrames_) / sampleRate_,
                      std::memory_order_relaxed);
}

void Engine::renderOffline(float* interleaved, int frames) { renderInterleaved(interleaved, frames); }

}  // namespace hr::audio
