// The mixer, ported from js/audio/engine.js.
//
//   sfx bus  -+
//   amb bus  -+- master gain -> muffle (underwater lowpass) -+
//   ui  bus  -+-------------------------------------------- +-> compressor -> out
//                    (ui joins after the muffle so menus stay crisp)
//
// The web build got this graph from Web Audio, which meant the mixing, the voice
// scheduling and the automation all lived behind the browser. Here it is a software
// mixer running on the miniaudio callback thread, so the shape has to be explicit:
//
//  * A voice is a throwaway noise burst or tuned partial, exactly as in the JS. The
//    engine owns a fixed pool and never allocates while playing.
//  * The game thread never touches a live voice. It posts commands into a
//    single-producer ring; the audio thread drains it at the top of every callback.
//  * `now()` is the Web Audio `currentTime` equivalent: the time at the END of the
//    callback currently being rendered. Scheduling against it means a sound asked
//    for "now" starts at the next block rather than being clamped into the past,
//    which is what keeps a recipe's staggered delays (the three grains of a sand
//    step, 20 ms apart) from collapsing on top of each other.
//
// Rendering runs in 128-frame blocks — Web Audio's render quantum — so automation
// and filter coefficients update on the same grid the recipes were tuned against.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio/dsp.h"
#include "core/mat4.h"

namespace hr::audio {

inline constexpr int kMaxEvents = 32;  // AudioEngine.MAX_VOICES: a cap on *events*
inline constexpr int kMaxVoices = 256;  // ...each of which can be up to ten voices

enum class Bus : std::uint8_t { Sfx, Amb, Ui };
enum class Curve : std::uint8_t { Exp, Lin };

// A filter stage. `sweepTo` of 0 means "no sweep"; `sweepT` of 0 means "over the
// whole duration", both matching the JS's `if (f.sweepTo)` / `f.sweepT || dur`.
struct FilterSpec {
  FilterType type = FilterType::Bandpass;
  float freq = 1000.0f;
  float q = 1.0f;
  float sweepTo = 0.0f;
  float sweepT = 0.0f;
};

// `filters: [...]` and `filter: {...}` in one type, so a recipe reads the same as
// the JS object literal it came from.
struct FilterChain {
  FilterSpec stages[2];
  int count = 0;
};

inline FilterChain bp(float freq, float q, float sweepTo = 0.0f, float sweepT = 0.0f) {
  return FilterChain{{{FilterType::Bandpass, freq, q, sweepTo, sweepT}}, 1};
}
inline FilterChain lp(float freq, float q, float sweepTo = 0.0f, float sweepT = 0.0f) {
  return FilterChain{{{FilterType::Lowpass, freq, q, sweepTo, sweepT}}, 1};
}
inline FilterChain hp(float freq, float q, float sweepTo = 0.0f, float sweepT = 0.0f) {
  return FilterChain{{{FilterType::Highpass, freq, q, sweepTo, sweepT}}, 1};
}

// Where a sound goes: a bus, optionally through a per-voice panner. The JS built one
// PannerNode per event and shared it across that event's voices; a panner with a
// fixed position is stateless, so giving each voice its own is equivalent.
struct Dest {
  Bus bus = Bus::Sfx;
  bool positional = false;
  bool valid = false;  // false when the voice cap refused the event
  float pos[3] = {0.0f, 0.0f, 0.0f};
};

// Field order follows the order the JS literals are written in, so designated
// initialisers at the call sites line up with the recipes they were copied from.
struct BurstOpts {
  float delay = 0.0f;
  float dur = 0.1f;
  float gain = 0.5f;
  float attack = 0.003f;
  Curve curve = Curve::Exp;
  NoiseKind noise = NoiseKind::White;
  float rate = 0.0f;  // 0 means 1.0, matching `if (o.rate)`
  FilterChain filters;
};

// A decoded clip from a resource pack. Mono, because the panner is (see
// audio/decode.h), and referenced rather than owned: the audio thread reads these
// samples directly out of the SoundBank's arena, which is why that arena never
// frees anything while the game is running.
struct SampleOpts {
  float delay = 0.0f;
  const float* samples = nullptr;
  int frameCount = 0;
  int sampleRate = 0;
  float gain = 1.0f;
  // Playback speed. 1.0 is the file's own pitch; the device's own rate conversion
  // is folded in on top, so a 44.1 kHz clip on a 48 kHz device still sounds right.
  float pitch = 1.0f;
};

struct ToneOpts {
  float delay = 0.0f;
  Wave wave = Wave::Sine;
  float freq = 440.0f;
  float sweepTo = 0.0f;
  float sweepT = 0.0f;
  float dur = 0.15f;
  float gain = 0.3f;
  float attack = 0.005f;
  float vibRate = 0.0f;
  float vibDepth = 10.0f;
  float tremRate = 0.0f;
  float tremDepth = 0.5f;
  FilterChain filter;
};

enum class Bed : std::uint8_t { Wind, Cave };

class Engine {
 public:
  Engine();
  ~Engine();

  // Opens the output device and builds the noise buffers and wave tables. Failing
  // here is not fatal — every entry point below no-ops when the engine is not
  // running, which is the same contract the JS had while the context was locked.
  bool start();
  void shutdown();

  bool ready() const { return running_.load(std::memory_order_relaxed); }
  // Context time in seconds: the end of the callback currently being rendered.
  double now() const { return scheduleTime_.load(std::memory_order_relaxed); }
  int sampleRate() const { return sampleRate_; }

  // ---- mixer -------------------------------------------------------------
  void setVolumes(float master, float sfx, float ambient, float ui);
  void setDucked(bool on);
  void setUnderwater(float t);
  void updateListener(const Vec3& pos, float yaw);

  // ---- voice accounting --------------------------------------------------
  // Returns false when 32 events are already in flight, so callers can just skip
  // the sound. `dur` is the expected life of the event in seconds.
  bool tryVoice(float dur);
  Dest out(Bus bus);
  Dest out(Bus bus, const Vec3& pos);

  // ---- one-shots ---------------------------------------------------------
  void burst(const Dest& dest, const BurstOpts& opts);
  void tone(const Dest& dest, const ToneOpts& opts);
  // A resource pack's clip, through the same voice pool, buses and panner as
  // everything else — so a pack's footstep is ducked, muffled underwater and
  // placed in the world exactly like the synthesised one it replaced.
  void sample(const Dest& dest, const SampleOpts& opts);

  // ---- ambience beds -----------------------------------------------------
  // Persistent chains the ambience controller eases per frame, rather than voices.
  void setBedGain(Bed bed, float value, float tau);
  void setWindFilter(float freq, float tau);
  void quietBeds();

  // ---- diagnostics -------------------------------------------------------
  int activeVoices() const { return activeVoices_.load(std::memory_order_relaxed); }
  int activeEvents() const { return static_cast<int>(eventExpiry_.size()); }

  // Renders the mix into an interleaved stereo buffer without a device, so the
  // recipes can be checked in --selftest and dumped to a wav for an A/B listen.
  void startOffline(int sampleRate);
  void renderOffline(float* interleaved, int frames);

 private:
  struct Voice;
  struct Command;
  class Ring;

  void buildTables(int sampleRate);
  void post(const Command& cmd);
  void drainCommands();
  void renderBlock(float* outL, float* outR, int frames);
  void renderVoice(Voice& v, float* sfxL, float* sfxR, float* ambL, float* ambR, float* uiL,
                   float* uiR, int frames);
  void renderBeds(float* ambL, float* ambR, int frames);
  int allocateVoice();

  // Shared by the device callback and renderOffline.
  void renderInterleaved(float* interleaved, int frames);
  void startBurst(const Command& cmd);
  void startTone(const Command& cmd);
  void startSample(const Command& cmd);

  // --- audio thread state -------------------------------------------------
  int sampleRate_ = 48000;
  std::int64_t renderedFrames_ = 0;
  // How far ahead of the render head `now()` is published. One device period, so a
  // command posted at any moment during one callback still lands in the future when
  // the next one drains it. Zero offline, where the caller posts between
  // renderOffline() calls and there is nothing to race with — a period there is the
  // whole render, which would schedule every event past the end of the buffer.
  int lookaheadFrames_ = 0;
  std::vector<Voice> voices_;
  NoiseBank noise_;
  WaveBank waves_;
  ListenerPose listener_;
  Smoother listenerSmooth_[6];  // position xyz, forward xyz
  Smoother masterGain_, muffleFreq_;
  Smoother windGain_, windFreq_, caveGain_;
  float busGain_[3] = {0.64f, 0.64f, 0.36f};
  Biquad muffle_[2];
  Compressor comp_;

  // The two persistent ambience chains.
  double windPos_ = 0.0;
  Biquad windFilter_;
  double cavePos_ = 0.7;
  Biquad caveFilter_;
  double caveOscPhase_[3] = {0.0, 0.0, 0.0};

  std::vector<float> scratch_[8];

  // --- shared -------------------------------------------------------------
  std::atomic<double> scheduleTime_{0.0};
  std::atomic<bool> running_{false};
  std::atomic<int> activeVoices_{0};
  std::unique_ptr<Ring> ring_;

  // --- game thread state --------------------------------------------------
  std::vector<double> eventExpiry_;  // AudioEngine._voices, as deadlines
  float volumes_[4] = {0.8f, 0.8f, 0.6f, 0.5f};
  float duck_ = 1.0f;
  float underwater_ = 0.0f;

  void* device_ = nullptr;  // ma_device, opaque so miniaudio.h stays out of headers
};

Engine& engine();

}  // namespace hr::audio
