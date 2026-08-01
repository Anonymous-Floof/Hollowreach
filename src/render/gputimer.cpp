#include "render/gputimer.h"

#include <chrono>
#include <cstdio>

#include "core/log.h"

namespace hr::render {
namespace {

double nowMs() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

// How often the published averages refresh. Matches FrameClock's fps window, so
// the overlay's numbers all change together instead of flickering against each
// other.
constexpr double kWindowMs = 500.0;

}  // namespace

const char* gpuPassName(GpuPass p) {
  switch (p) {
    case GpuPass::Shadow: return "shadow";
    case GpuPass::Sky: return "sky";
    case GpuPass::Terrain: return "terrain";
    case GpuPass::Entities: return "entities";
    case GpuPass::Ssao: return "ssao";
    case GpuPass::Composite: return "composite";
    case GpuPass::Water: return "water";
    case GpuPass::Godray: return "godray";
    case GpuPass::Present: return "present";
    default: return "?";
  }
}

const char* cpuPhaseName(CpuPhase p) {
  switch (p) {
    case CpuPhase::WorldUpdate: return "world";
    case CpuPhase::Entities: return "entities";
    case CpuPhase::CollectLights: return "lights";
    case CpuPhase::Visibility: return "visibility";
    case CpuPhase::Submit: return "submit";
    default: return "?";
  }
}

void FrameProfiler::init() {
  if (!enabled_ || ready_) return;
  for (Slot& s : slots_) {
    glGenQueries(kPasses, s.query.data());
    s.issued.fill(false);
    s.live = false;
  }
  windowStart_ = nowMs();
  ready_ = true;
}

void FrameProfiler::dispose() {
  if (!ready_) return;
  // An open query would be deleted mid-flight; close it first.
  if (active_ >= 0) {
    glEndQuery(GL_TIME_ELAPSED);
    active_ = -1;
  }
  for (Slot& s : slots_) {
    glDeleteQueries(kPasses, s.query.data());
    s.query.fill(0);
    s.issued.fill(false);
    s.live = false;
  }
  ready_ = false;
}

void FrameProfiler::beginFrame() {
  if (!ready_) return;
  slot_ = (slot_ + 1) % kSlots;
  // The slot we are about to overwrite is the oldest, so by now the driver has
  // almost certainly finished it. Harvest before reuse — and note this is the
  // only place a result is read, which is what keeps the readback off the hot
  // path entirely.
  harvest(slots_[slot_]);
  slots_[slot_].issued.fill(false);
  slots_[slot_].live = true;
}

void FrameProfiler::harvest(Slot& s) {
  if (!s.live) return;
  for (int i = 0; i < kPasses; ++i) {
    if (!s.issued[i]) continue;
    GLuint available = 0;
    glGetQueryObjectuiv(s.query[i], GL_QUERY_RESULT_AVAILABLE, &available);
    // Not ready after four frames would be extraordinary, but reading it anyway
    // would block — so the sample is dropped rather than allowed to stall.
    if (!available) continue;
    GLuint64 ns = 0;
    glGetQueryObjectui64v(s.query[i], GL_QUERY_RESULT, &ns);
    gpuAccum_[i] += static_cast<double>(ns) * 1e-6;
  }
  ++gpuFrames_;
  s.live = false;
}

void FrameProfiler::begin(GpuPass p) {
  if (!ready_) return;
  const int i = static_cast<int>(p);
  // Defensive rather than theoretical: a future pass that returns early between
  // begin and end would otherwise leave a query open and turn every subsequent
  // begin into a GL error. GpuScope makes that unreachable today.
  if (active_ >= 0) glEndQuery(GL_TIME_ELAPSED);
  glBeginQuery(GL_TIME_ELAPSED, slots_[slot_].query[i]);
  slots_[slot_].issued[i] = true;
  active_ = i;
}

void FrameProfiler::end() {
  if (!ready_ || active_ < 0) return;
  glEndQuery(GL_TIME_ELAPSED);
  active_ = -1;
}

void FrameProfiler::addCpu(CpuPhase p, double ms) {
  if (!enabled_) return;
  cpuAccum_[static_cast<int>(p)] += ms;
}

void FrameProfiler::endFrame(double frameMs) {
  if (!enabled_) return;
  frameAccum_ += frameMs;
  ++cpuFrames_;
  if (nowMs() - windowStart_ >= kWindowMs) publish();
}

void FrameProfiler::publish() {
  const double gpuDiv = gpuFrames_ > 0 ? static_cast<double>(gpuFrames_) : 1.0;
  const double cpuDiv = cpuFrames_ > 0 ? static_cast<double>(cpuFrames_) : 1.0;
  for (int i = 0; i < kPasses; ++i) {
    gpuMs_[i] = static_cast<float>(gpuAccum_[i] / gpuDiv);
    gpuAccum_[i] = 0.0;
  }
  for (int i = 0; i < kPhases; ++i) {
    cpuMs_[i] = static_cast<float>(cpuAccum_[i] / cpuDiv);
    cpuAccum_[i] = 0.0;
  }
  frameMs_ = static_cast<float>(frameAccum_ / cpuDiv);
  frameAccum_ = 0.0;
  gpuFrames_ = 0;
  cpuFrames_ = 0;
  windowStart_ = nowMs();
}

float FrameProfiler::gpuTotalMs() const {
  float t = 0.0f;
  for (int i = 0; i < kPasses; ++i) t += gpuMs_[i];
  return t;
}

const char* FrameProfiler::summaryLine() const {
  std::snprintf(line_, sizeof(line_),
                "GPU %.2f (sh %.2f sky %.2f ter %.2f ent %.2f ao %.2f co %.2f wa %.2f "
                "gr %.2f pr %.2f) | CPU wo %.2f en %.2f li %.2f vi %.2f su %.2f",
                gpuTotalMs(), gpuMs(GpuPass::Shadow), gpuMs(GpuPass::Sky),
                gpuMs(GpuPass::Terrain), gpuMs(GpuPass::Entities),
                gpuMs(GpuPass::Ssao), gpuMs(GpuPass::Composite), gpuMs(GpuPass::Water),
                gpuMs(GpuPass::Godray), gpuMs(GpuPass::Present),
                cpuMs(CpuPhase::WorldUpdate), cpuMs(CpuPhase::Entities),
                cpuMs(CpuPhase::CollectLights), cpuMs(CpuPhase::Visibility),
                cpuMs(CpuPhase::Submit));
  return line_;
}

void FrameProfiler::logReport() const {
  if (!enabled_) return;
  const float total = gpuTotalMs();
  log::info("--- frame breakdown (mean over the last window) ---");
  log::info("  frame        %6.2f ms  (%.0f fps)", frameMs_,
            frameMs_ > 0.0f ? 1000.0f / frameMs_ : 0.0f);
  log::info("  GPU passes   %6.2f ms", total);
  for (int i = 0; i < kPasses; ++i) {
    const float ms = gpuMs_[i];
    log::info("    %-10s %6.2f ms  %4.1f%%", gpuPassName(static_cast<GpuPass>(i)), ms,
              total > 0.0f ? 100.0f * ms / total : 0.0f);
  }
  log::info("  CPU phases");
  for (int i = 0; i < kPhases; ++i) {
    log::info("    %-10s %6.2f ms", cpuPhaseName(static_cast<CpuPhase>(i)), cpuMs_[i]);
  }
}

CpuScope::CpuScope(FrameProfiler& p, CpuPhase phase)
    : p_(p), phase_(phase), start_(p.enabled() ? nowMs() : 0.0) {}

CpuScope::~CpuScope() {
  if (p_.enabled()) p_.addCpu(phase_, nowMs() - start_);
}

}  // namespace hr::render
