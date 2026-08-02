#include "render/viewmodel.h"

#include <algorithm>
#include <cmath>

namespace hr::render {
namespace {

constexpr float kSwingSeconds = 0.26f;  // one swing arc
constexpr float kEquipSeconds = 0.18f;  // a newly selected item rising into frame

// Ry * Rx * Rz, row-major, matching the convention HoldStyle::rot documents.
struct Rot3 {
  float m[3][3];
};

Rot3 euler(float rx, float ry, float rz) {
  const float cx = std::cos(rx), sx = std::sin(rx);
  const float cy = std::cos(ry), sy = std::sin(ry);
  const float cz = std::cos(rz), sz = std::sin(rz);
  Rot3 r;
  r.m[0][0] = cy * cz + sy * sx * sz;
  r.m[0][1] = -cy * sz + sy * sx * cz;
  r.m[0][2] = sy * cx;
  r.m[1][0] = cx * sz;
  r.m[1][1] = cx * cz;
  r.m[1][2] = -sx;
  r.m[2][0] = -sy * cz + cy * sx * sz;
  r.m[2][1] = sy * sz + cy * sx * cz;
  r.m[2][2] = cy * cx;
  return r;
}

Rot3 operator*(const Rot3& a, const Rot3& b) {
  Rot3 o;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      o.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    }
  }
  return o;
}

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

void Viewmodel::cancelSwing() {
  swingT_ = 1.0f;
  queued_ = false;
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
  // The animation is kept OUT of the pose and applied outside it, about the view's
  // own axes: ax swings the item down the screen, ay turns it across the screen,
  // az rolls it in the picture plane.
  //
  // These used to be added straight into the hold style's Euler angles, which is
  // wrong for a reason that stayed hidden while every style pointed roughly the
  // same way: the axis a style's `rot.x` turns about is itself rotated by that
  // style's `rot.y`. Yaw a tool a quarter turn to put its edge into the block —
  // which is what a tool wants — and the "swing down" term becomes a rotation
  // about the view's Z instead, so the tool spun in the picture plane and dug
  // sideways while looking perfectly correct at rest. Held out here, the arm
  // swings down whatever the hand happens to be holding or how it grips it.
  float sx = 0.0f, sy = 0.0f, sz = 0.0f;
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
    sx -= 0.78f * g;  // the head drops toward the crosshair
    sz -= 0.26f * g;  // and leans over as it goes
    sy += 0.30f * f;  // a turn into the blow, at the moment of contact
  }

  // --- equip: the item rises from below the frame when you switch slots.
  if (equipT_ < 1.0f) {
    const float e = 1.0f - equipT_;
    ay -= 0.95f * e;
    sx -= 0.35f * e;
    sz += 0.20f * e;
  }

  // --- walk sway, on the same bob phase that moves the camera.
  if (bobMagnitude_ > 0.001f) {
    const float m = bobMagnitude_;
    ax += std::cos(bobPhase_) * 0.10f * m;
    ay -= std::fabs(std::sin(bobPhase_)) * 0.10f * m;
    sz += std::sin(bobPhase_) * 0.05f * m;
  }

  // --- idle drift: a slow breath, so a still hand is never perfectly frozen.
  ax += std::sin(clock_ * 0.63f) * 0.010f;
  ay += std::sin(clock_ * 0.91f) * 0.013f;
  sz += std::sin(clock_ * 0.77f) * 0.014f;

  // --- framing: anchor and size are fractions of the frame at `dist`, so the item
  // keeps its place in the picture at any aspect ratio.
  const float halfH = st.dist * std::tan(fovRadians / 2.0f);
  const float px = ax * halfH * aspect, py = ay * halfH, pz = -st.dist;
  const float s = st.size * 2.0f * halfH;

  // --- compose T(p) . Rswing . Rpose . S . T(-grip) . flip.
  const Rot3 rot = euler(sx, sy, sz) * euler(st.rot.x, st.rot.y, st.rot.z);
  const float r00 = rot.m[0][0], r01 = rot.m[0][1], r02 = rot.m[0][2];
  const float r10 = rot.m[1][0], r11 = rot.m[1][1], r12 = rot.m[1][2];
  const float r20 = rot.m[2][0], r21 = rot.m[2][1], r22 = rot.m[2][2];

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
