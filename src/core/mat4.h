// Column-major 4x4 matrices and the frustum helpers, ported from js/core/mat4.js.
//
// Ported rather than replaced with a library on purpose. The renderer depends on
// these exact conventions — right-handed, depth in [-1, 1], no reverse-Z, camera
// looking down -Z, translation in the last column — and the deferred lighting pass
// leans on them further still, reconstructing world position from a camera basis
// instead of inverting the view-projection. Swapping in a library's conventions
// would mean re-deriving all of that.

#pragma once

#include <array>
#include <cmath>

namespace hr {

struct Vec3 {
  float x = 0, y = 0, z = 0;

  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  Vec3& operator+=(const Vec3& o) {
    x += o.x;
    y += o.y;
    z += o.z;
    return *this;
  }

  float length() const { return std::sqrt(x * x + y * y + z * z); }
  Vec3 normalized() const {
    const float l = length();
    return l > 0 ? Vec3 {x / l, y / l, z / l} : Vec3 {0, 0, 0};
  }
  float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
  Vec3 cross(const Vec3& o) const {
    return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
  }
  const float* data() const { return &x; }
};

// Column-major, so m[12..14] is the translation — the layout glUniformMatrix4fv
// expects with transpose = GL_FALSE.
struct Mat4 {
  std::array<float, 16> m {};

  static Mat4 identity();

  // Right-handed perspective with depth in [-1, 1].
  static Mat4 perspective(float fovyRadians, float aspect, float near, float far);
  // Orthographic, depth in [-1, 1]. Used for the sun's shadow camera.
  static Mat4 ortho(float l, float r, float b, float t, float n, float f);
  // Right-handed lookAt: the camera looks from eye toward centre.
  static Mat4 lookAt(const Vec3& eye, const Vec3& centre, const Vec3& up);
  // A view matrix from eye plus yaw/pitch, with an explicit right/up/forward basis.
  static Mat4 fromYawPitch(const Vec3& eye, float yaw, float pitch);
  // translate(x, y, z) * rotateY(yaw) * scale(s). Places entity meshes.
  static Mat4 model(float x, float y, float z, float yaw, float scale = 1.0f);
  static Mat4 translate(float x, float y, float z);
  static Mat4 rotateX(float radians);
  static Mat4 rotateY(float radians);

  // this * other, in that order.
  Mat4 operator*(const Mat4& other) const;

  // Returns false and leaves `out` untouched when the matrix is singular.
  bool invert(Mat4& out) const;

  const float* data() const { return m.data(); }
  float* data() { return m.data(); }
  float operator[](int i) const { return m[i]; }
  float& operator[](int i) { return m[i]; }
};

// The direction the camera looks for a given yaw and pitch. yaw = 0 looks
// toward -Z.
Vec3 lookDir(float yaw, float pitch);

// Six frustum planes from a combined projection * view matrix, each [a,b,c,d] with
// a*x + b*y + c*z + d >= 0 meaning inside. Order: left, right, bottom, top, near, far.
struct Frustum {
  std::array<std::array<float, 4>, 6> planes {};

  static Frustum fromViewProj(const Mat4& viewProj);

  // Is an axis-aligned box at least partly inside?
  bool testAabb(float minX, float minY, float minZ, float maxX, float maxY,
                float maxZ) const;
};

}  // namespace hr
