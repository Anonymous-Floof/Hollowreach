// Per-pass GPU and CPU timing.
//
// Every performance number this project has produced until now was wall-clock
// frame time with no attribution, and it has been wrong twice: M10 blamed the
// driver's VBO allocation (M12 disproved it) and a later pass read "resolution
// makes no difference" as "not fragment bound". A single number cannot tell you
// which pass owns the frame, and guessing from one has cost more time than
// measuring would have.
//
// GL_TIME_ELAPSED brackets a pass and reports nanoseconds of real GPU time. The
// result is read back several frames later, so asking for it never stalls the
// pipeline the way glFinish or an immediate GL_QUERY_RESULT would — which
// matters, because a profiler that serialises the thing it measures reports the
// serialisation.
//
// GL_TIME_ELAPSED queries CANNOT nest: only one may be active per target. The
// passes are sequential so that costs nothing, but it does mean there is no
// enclosing "whole frame" query — the total below is the sum of the parts, and
// therefore excludes any gap between them.

#pragma once

#include <array>
#include <cstdint>

#include "core/gl.h"

namespace hr::render {

// One entry per bracketed pass in Renderer::render, in submission order.
enum class GpuPass {
  Shadow,
  Sky,       // the fullscreen sky triangle into the G-buffer
  Terrain,   // opaque terrain into the G-buffer
  Entities,  // mobs, drops and boats into the G-buffer
  Ssao,
  Composite,
  Water,
  Godray,
  Present,
  Count,
};

const char* gpuPassName(GpuPass p);

// CPU-side phases, measured on the main thread with the steady clock. These sit
// alongside the GPU numbers because the answer to "where is the frame going" is
// only meaningful when both halves are visible at once.
// Note that these are NOT a partition: Submit is the whole of Renderer::render
// on the CPU, and CollectLights and Visibility are components of it. They are
// reported separately because the question is which part of submission costs
// anything, not how to make the columns add up.
enum class CpuPhase {
  WorldUpdate,   // streaming: installs, scans, uploads
  Entities,      // mob tick and AI
  CollectLights, // the emitter sweep over loaded chunks  (within Submit)
  Visibility,    // the per-chunk, per-section cull and draw loop  (within Submit)
  Submit,        // all of Renderer::render on the CPU
  Count,
};

const char* cpuPhaseName(CpuPhase p);

class FrameProfiler {
 public:
  // Allocates the query ring. Safe to call when disabled — it does nothing, so a
  // normal run pays no GL objects and no per-pass calls at all.
  void init();
  void dispose();

  void setEnabled(bool on) { enabled_ = on; }
  bool enabled() const { return enabled_; }

  // Rotates the ring and harvests whatever the driver has finished. Call once at
  // the top of the frame, before any begin().
  void beginFrame();
  void endFrame(double frameMs);

  void begin(GpuPass p);
  void end();

  void addCpu(CpuPhase p, double ms);

  // Published averages over the last window, in milliseconds. Zero for a pass
  // that did not run — a shadow pass skipped at night reads 0, not stale.
  float gpuMs(GpuPass p) const { return gpuMs_[static_cast<int>(p)]; }
  float cpuMs(CpuPhase p) const { return cpuMs_[static_cast<int>(p)]; }
  float gpuTotalMs() const;
  float frameMs() const { return frameMs_; }

  // A one-line summary for the overlay, and a multi-line block for the exit
  // report. Both return a pointer to an internal buffer, valid until the next
  // call.
  const char* summaryLine() const;
  void logReport() const;

 private:
  static constexpr int kSlots = 4;  // frames of latency before a readback
  static constexpr int kPasses = static_cast<int>(GpuPass::Count);
  static constexpr int kPhases = static_cast<int>(CpuPhase::Count);

  struct Slot {
    std::array<GLuint, kPasses> query {};
    std::array<bool, kPasses> issued {};
    bool live = false;  // has been written and not yet harvested
  };

  void harvest(Slot& s);
  void publish();

  bool enabled_ = false;
  bool ready_ = false;
  std::array<Slot, kSlots> slots_ {};
  int slot_ = 0;
  int active_ = -1;  // pass whose query is open, or -1

  // Accumulators for the current window.
  std::array<double, kPasses> gpuAccum_ {};
  std::array<double, kPhases> cpuAccum_ {};
  double frameAccum_ = 0.0;
  int gpuFrames_ = 0;
  int cpuFrames_ = 0;
  double windowStart_ = 0.0;

  std::array<float, kPasses> gpuMs_ {};
  std::array<float, kPhases> cpuMs_ {};
  float frameMs_ = 0.0f;

  mutable char line_[256] {};
};

// RAII, because several passes return early — renderShadowMap when the sun is
// down, renderSSAO when samples are zero — and an unclosed GL_TIME_ELAPSED query
// makes the next begin() a GL error rather than a wrong number.
class GpuScope {
 public:
  GpuScope(FrameProfiler& p, GpuPass pass) : p_(p) { p_.begin(pass); }
  ~GpuScope() { p_.end(); }
  GpuScope(const GpuScope&) = delete;
  GpuScope& operator=(const GpuScope&) = delete;

 private:
  FrameProfiler& p_;
};

// The CPU equivalent. Adds its own elapsed time to the phase on destruction.
class CpuScope {
 public:
  CpuScope(FrameProfiler& p, CpuPhase phase);
  ~CpuScope();
  CpuScope(const CpuScope&) = delete;
  CpuScope& operator=(const CpuScope&) = delete;

 private:
  FrameProfiler& p_;
  CpuPhase phase_;
  double start_;
};

}  // namespace hr::render
