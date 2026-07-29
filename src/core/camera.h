// View and projection camera, ported from js/core/camera.js.
//
// Holds the matrices and exposes frustum planes so the renderer can cull chunks.

#pragma once

#include "core/mat4.h"

namespace hr {

class Camera {
 public:
  void setProjection(float aspect, float fovDegrees);

  // `eye` is the world position of the player's eye.
  void update(const Vec3& eye, float yaw, float pitch);

  // Points along an explicit direction with an explicit up vector — a full lookAt
  // rather than yaw/pitch. Used to render the six 90-degree cube faces of a
  // panorama capture, where the vertical faces need a non-world up vector.
  void setLook(const Vec3& eye, const Vec3& dir, const Vec3& up, float fovDegrees,
               float aspect);

  const Mat4& proj() const { return proj_; }
  const Mat4& view() const { return view_; }
  const Mat4& viewProj() const { return viewProj_; }
  const Frustum& frustum() const { return frustum_; }

  const Vec3& pos() const { return pos_; }
  float yaw() const { return yaw_; }
  float pitch() const { return pitch_; }
  float fov() const { return fov_; }
  float near() const { return near_; }
  float far() const { return far_; }

  Vec3 forward() const { return lookDir(yaw_, pitch_); }
  // The basis the deferred composite pass uses to rebuild world position from
  // depth without inverting the view-projection.
  Vec3 right() const;
  Vec3 up() const;
  float tanHalfFov() const;

 private:
  Mat4 proj_ = Mat4::identity();
  Mat4 view_ = Mat4::identity();
  Mat4 viewProj_ = Mat4::identity();
  Frustum frustum_;

  Vec3 pos_;
  float yaw_ = 0.0f;
  float pitch_ = 0.0f;
  float fov_ = 70.0f;
  float near_ = 0.1f;
  // Far is deliberately modest. The deferred pass reconstructs world position from
  // the depth buffer, and a large far plane crushes perspective-depth precision
  // near 1.0 — float32 reconstruction then loses several blocks in the near field,
  // which visibly broke the shadow and fog lookups. 512 covers the maximum render
  // distance.
  float far_ = 512.0f;
};

}  // namespace hr
