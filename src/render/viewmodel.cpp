#include "render/viewmodel.h"

#include <algorithm>
#include <cmath>

namespace hr::render {
namespace {

constexpr float kSwingSeconds = 0.26f;  // one swing arc
constexpr float kEquipSeconds = 0.18f;  // a newly selected item rising into frame

}  // namespace

void Viewmodel::setItem(const std::string& key) {
  if (hasItem_ && key == key_) return;
  key_ = key;
  hasItem_ = true;
  equipT_ = 0.0f;
  swingT_ = 1.0f;
  queued_ = false;
}

void Viewmodel::swing() {
  if (swingT_ >= 1.0f) {
    swingT_ = 0.0f;
  } else {
    queued_ = true;
  }
}

void Viewmodel::update(float dt, float bobPhase, float bobMagnitude) {
  clock_ += dt;
  bobPhase_ = bobPhase;
  bobMagnitude_ = bobMagnitude;
  if (equipT_ < 1.0f) equipT_ = std::min(1.0f, equipT_ + dt / kEquipSeconds);
  if (swingT_ < 1.0f) {
    swingT_ += dt / kSwingSeconds;
    if (swingT_ >= 1.0f && queued_) {
      swingT_ = 0.0f;
      queued_ = false;
    }
  }
}

Mat4 Viewmodel::modelMatrix(const HoldStyle& st, float fovRadians, float aspect) const {
  float rx = st.rot.x, ry = st.rot.y, rz = st.rot.z;
  float ax = st.anchorX, ay = st.anchorY;

  // --- swing: a chop that drops toward the crosshair, rolls the head over and
  // snaps back. `g` is the broad arc (fast out, slow back), `f` peaks later and
  // harder — the moment of contact. Both are 0 at the ends, so the pose is
  // untouched at rest no matter how the style is tuned.
  if (swingT_ < 1.0f) {
    const float t = swingT_;
    const float g = std::sin(std::sqrt(t) * 3.14159265358979323846f);
    const float f = std::sin(t * t * 3.14159265358979323846f);
    ax -= 0.13f * g;
    ay -= 0.20f * g;
    rx -= 0.78f * g;
    rz -= 0.26f * g;
    ry += 0.30f * f;
  }

  // --- equip: the item rises from below the frame when you switch slots.
  if (equipT_ < 1.0f) {
    const float e = 1.0f - equipT_;
    ay -= 0.95f * e;
    rx -= 0.35f * e;
    rz += 0.20f * e;
  }

  // --- walk sway, on the same bob phase that moves the camera.
  if (bobMagnitude_ > 0.001f) {
    const float m = bobMagnitude_;
    ax += std::cos(bobPhase_) * 0.10f * m;
    ay -= std::fabs(std::sin(bobPhase_)) * 0.10f * m;
    rz += std::sin(bobPhase_) * 0.05f * m;
  }

  // --- idle drift: a slow breath, so a still hand is never perfectly frozen.
  ax += std::sin(clock_ * 0.63f) * 0.010f;
  ay += std::sin(clock_ * 0.91f) * 0.013f;
  rz += std::sin(clock_ * 0.77f) * 0.014f;

  // --- framing: anchor and size are fractions of the frame at `dist`, so the item
  // keeps its place in the picture at any aspect ratio.
  const float halfH = st.dist * std::tan(fovRadians / 2.0f);
  const float px = ax * halfH * aspect, py = ay * halfH, pz = -st.dist;
  const float s = st.size * 2.0f * halfH;

  // --- compose T(p) . Ry . Rx . Rz . S . T(-grip) . flip.
  // Written out rather than built from four matrix products: this runs once a frame
  // for one object, and the expanded form is the whole transform in one place.
  const float cx = std::cos(rx), sx = std::sin(rx);
  const float cy = std::cos(ry), sy = std::sin(ry);
  const float cz = std::cos(rz), sz = std::sin(rz);
  const float r00 = cy * cz + sy * sx * sz, r01 = -cy * sz + sy * sx * cz, r02 = sy * cx;
  const float r10 = cx * sz, r11 = cx * cz, r12 = -sx;
  const float r20 = -sy * cz + cy * sx * sz, r21 = sy * sz + cy * sx * cz, r22 = cy * cx;

  Mat4 out;
  const float fx = st.flip ? -s : s;  // column 0 carries the mirror
  out.m[0] = r00 * fx;
  out.m[1] = r10 * fx;
  out.m[2] = r20 * fx;
  out.m[3] = 0;
  out.m[4] = r01 * s;
  out.m[5] = r11 * s;
  out.m[6] = r21 * s;
  out.m[7] = 0;
  out.m[8] = r02 * s;
  out.m[9] = r12 * s;
  out.m[10] = r22 * s;
  out.m[11] = 0;
  // The grip is given in MIRRORED model space (what you see), so it moves along the
  // un-mirrored rotated axes — hence r00 * s here, not out.m[0].
  const float gx = st.grip.x, gy = st.grip.y, gz = st.grip.z;
  out.m[12] = px - (r00 * gx + r01 * gy + r02 * gz) * s;
  out.m[13] = py - (r10 * gx + r11 * gy + r12 * gz) * s;
  out.m[14] = pz - (r20 * gx + r21 * gy + r22 * gz) * s;
  out.m[15] = 1;
  return out;
}

}  // namespace hr::render
