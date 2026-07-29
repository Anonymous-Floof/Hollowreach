#include "core/camera.h"

namespace hr {
namespace {
constexpr float kPi = 3.14159265358979323846f;
}

void Camera::setProjection(float aspect, float fovDegrees) {
  fov_ = fovDegrees;
  proj_ = Mat4::perspective(fovDegrees * kPi / 180.0f, aspect, near_, far_);
}

void Camera::update(const Vec3& eye, float yaw, float pitch) {
  pos_ = eye;
  yaw_ = yaw;
  pitch_ = pitch;
  view_ = Mat4::fromYawPitch(eye, yaw, pitch);
  viewProj_ = proj_ * view_;
  frustum_ = Frustum::fromViewProj(viewProj_);
}

void Camera::setLook(const Vec3& eye, const Vec3& dir, const Vec3& up, float fovDegrees,
                     float aspect) {
  pos_ = eye;
  fov_ = fovDegrees;
  proj_ = Mat4::perspective(fovDegrees * kPi / 180.0f, aspect, near_, far_);
  view_ = Mat4::lookAt(eye, eye + dir, up);
  viewProj_ = proj_ * view_;
  frustum_ = Frustum::fromViewProj(viewProj_);
}

Vec3 Camera::right() const {
  const Vec3 f = forward();
  return Vec3 {-f.z, 0.0f, f.x}.normalized();
}

Vec3 Camera::up() const {
  const Vec3 f = forward();
  const Vec3 r = right();
  return {r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x};
}

float Camera::tanHalfFov() const { return std::tan(fov_ * kPi / 180.0f * 0.5f); }

}  // namespace hr
