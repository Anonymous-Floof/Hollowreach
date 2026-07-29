#include "core/mat4.h"

namespace hr {

Mat4 Mat4::identity() {
  Mat4 o;
  o.m[0] = o.m[5] = o.m[10] = o.m[15] = 1.0f;
  return o;
}

Mat4 Mat4::perspective(float fovyRadians, float aspect, float near, float far) {
  Mat4 o;
  const float f = 1.0f / std::tan(fovyRadians / 2.0f);
  o.m[0] = f / aspect;
  o.m[5] = f;
  o.m[10] = (far + near) / (near - far);
  o.m[11] = -1.0f;
  o.m[14] = (2.0f * far * near) / (near - far);
  return o;
}

Mat4 Mat4::ortho(float l, float r, float b, float t, float n, float f) {
  Mat4 o;
  o.m[0] = 2.0f / (r - l);
  o.m[5] = 2.0f / (t - b);
  o.m[10] = -2.0f / (f - n);
  o.m[12] = -(r + l) / (r - l);
  o.m[13] = -(t + b) / (t - b);
  o.m[14] = -(f + n) / (f - n);
  o.m[15] = 1.0f;
  return o;
}

Mat4 Mat4::lookAt(const Vec3& eye, const Vec3& centre, const Vec3& up) {
  Vec3 fwd = (centre - eye).normalized();
  if (fwd.length() == 0.0f) fwd = {0, 0, -1};
  Vec3 side = Vec3 {fwd.y * up.z - fwd.z * up.y, fwd.z * up.x - fwd.x * up.z,
                    fwd.x * up.y - fwd.y * up.x}
                  .normalized();
  const Vec3 u {side.y * fwd.z - side.z * fwd.y, side.z * fwd.x - side.x * fwd.z,
                side.x * fwd.y - side.y * fwd.x};

  Mat4 o;
  o.m[0] = side.x;  o.m[1] = u.x;  o.m[2] = -fwd.x;  o.m[3] = 0;
  o.m[4] = side.y;  o.m[5] = u.y;  o.m[6] = -fwd.y;  o.m[7] = 0;
  o.m[8] = side.z;  o.m[9] = u.z;  o.m[10] = -fwd.z; o.m[11] = 0;
  o.m[12] = -(side.x * eye.x + side.y * eye.y + side.z * eye.z);
  o.m[13] = -(u.x * eye.x + u.y * eye.y + u.z * eye.z);
  o.m[14] = (fwd.x * eye.x + fwd.y * eye.y + fwd.z * eye.z);
  o.m[15] = 1.0f;
  return o;
}

Mat4 Mat4::fromYawPitch(const Vec3& eye, float yaw, float pitch) {
  const Vec3 f = lookDir(yaw, pitch);
  // right = normalize(cross(forward, worldUp)) reduces to normalize(-f.z, 0, f.x).
  Vec3 right {-f.z, 0.0f, f.x};
  const float rl = right.length();
  if (rl > 0.0f) right = right * (1.0f / rl);
  // up = cross(right, forward)
  const Vec3 u {right.y * f.z - right.z * f.y, right.z * f.x - right.x * f.z,
                right.x * f.y - right.y * f.x};

  Mat4 o;
  o.m[0] = right.x; o.m[1] = u.x; o.m[2] = -f.x;  o.m[3] = 0;
  o.m[4] = right.y; o.m[5] = u.y; o.m[6] = -f.y;  o.m[7] = 0;
  o.m[8] = right.z; o.m[9] = u.z; o.m[10] = -f.z; o.m[11] = 0;
  o.m[12] = -(right.x * eye.x + right.y * eye.y + right.z * eye.z);
  o.m[13] = -(u.x * eye.x + u.y * eye.y + u.z * eye.z);
  o.m[14] = (f.x * eye.x + f.y * eye.y + f.z * eye.z);
  o.m[15] = 1.0f;
  return o;
}

Mat4 Mat4::model(float x, float y, float z, float yaw, float scale) {
  Mat4 o;
  const float c = std::cos(yaw) * scale;
  const float s = std::sin(yaw) * scale;
  o.m[0] = c;  o.m[1] = 0;     o.m[2] = -s; o.m[3] = 0;
  o.m[4] = 0;  o.m[5] = scale; o.m[6] = 0;  o.m[7] = 0;
  o.m[8] = s;  o.m[9] = 0;     o.m[10] = c; o.m[11] = 0;
  o.m[12] = x; o.m[13] = y;    o.m[14] = z; o.m[15] = 1;
  return o;
}

Mat4 Mat4::translate(float x, float y, float z) {
  Mat4 o = identity();
  o.m[12] = x;
  o.m[13] = y;
  o.m[14] = z;
  return o;
}

Mat4 Mat4::rotateX(float radians) {
  Mat4 o = identity();
  const float c = std::cos(radians), s = std::sin(radians);
  o.m[5] = c;
  o.m[6] = s;
  o.m[9] = -s;
  o.m[10] = c;
  return o;
}

Mat4 Mat4::rotateY(float radians) {
  Mat4 o = identity();
  const float c = std::cos(radians), s = std::sin(radians);
  o.m[0] = c;
  o.m[2] = -s;
  o.m[8] = s;
  o.m[10] = c;
  return o;
}

Mat4 Mat4::operator*(const Mat4& b) const {
  const auto& a = m;
  const float a00 = a[0], a01 = a[1], a02 = a[2], a03 = a[3];
  const float a10 = a[4], a11 = a[5], a12 = a[6], a13 = a[7];
  const float a20 = a[8], a21 = a[9], a22 = a[10], a23 = a[11];
  const float a30 = a[12], a31 = a[13], a32 = a[14], a33 = a[15];

  Mat4 o;
  for (int i = 0; i < 4; ++i) {
    const float b0 = b.m[i * 4], b1 = b.m[i * 4 + 1], b2 = b.m[i * 4 + 2],
                b3 = b.m[i * 4 + 3];
    o.m[i * 4] = b0 * a00 + b1 * a10 + b2 * a20 + b3 * a30;
    o.m[i * 4 + 1] = b0 * a01 + b1 * a11 + b2 * a21 + b3 * a31;
    o.m[i * 4 + 2] = b0 * a02 + b1 * a12 + b2 * a22 + b3 * a32;
    o.m[i * 4 + 3] = b0 * a03 + b1 * a13 + b2 * a23 + b3 * a33;
  }
  return o;
}

bool Mat4::invert(Mat4& out) const {
  const auto& a = m;
  const float a00 = a[0], a01 = a[1], a02 = a[2], a03 = a[3];
  const float a10 = a[4], a11 = a[5], a12 = a[6], a13 = a[7];
  const float a20 = a[8], a21 = a[9], a22 = a[10], a23 = a[11];
  const float a30 = a[12], a31 = a[13], a32 = a[14], a33 = a[15];

  const float b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10;
  const float b02 = a00 * a13 - a03 * a10, b03 = a01 * a12 - a02 * a11;
  const float b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
  const float b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30;
  const float b08 = a20 * a33 - a23 * a30, b09 = a21 * a32 - a22 * a31;
  const float b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;

  float det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
  if (det == 0.0f) return false;
  det = 1.0f / det;

  out.m[0] = (a11 * b11 - a12 * b10 + a13 * b09) * det;
  out.m[1] = (a02 * b10 - a01 * b11 - a03 * b09) * det;
  out.m[2] = (a31 * b05 - a32 * b04 + a33 * b03) * det;
  out.m[3] = (a22 * b04 - a21 * b05 - a23 * b03) * det;
  out.m[4] = (a12 * b08 - a10 * b11 - a13 * b07) * det;
  out.m[5] = (a00 * b11 - a02 * b08 + a03 * b07) * det;
  out.m[6] = (a32 * b02 - a30 * b05 - a33 * b01) * det;
  out.m[7] = (a20 * b05 - a22 * b02 + a23 * b01) * det;
  out.m[8] = (a10 * b10 - a11 * b08 + a13 * b06) * det;
  out.m[9] = (a01 * b08 - a00 * b10 - a03 * b06) * det;
  out.m[10] = (a30 * b04 - a31 * b02 + a33 * b00) * det;
  out.m[11] = (a21 * b02 - a20 * b04 - a23 * b00) * det;
  out.m[12] = (a11 * b07 - a10 * b09 - a12 * b06) * det;
  out.m[13] = (a00 * b09 - a01 * b07 + a02 * b06) * det;
  out.m[14] = (a31 * b01 - a30 * b03 - a32 * b00) * det;
  out.m[15] = (a20 * b03 - a21 * b01 + a22 * b00) * det;
  return true;
}

Vec3 lookDir(float yaw, float pitch) {
  const float cp = std::cos(pitch);
  return {-std::sin(yaw) * cp, std::sin(pitch), -std::cos(yaw) * cp};
}

Frustum Frustum::fromViewProj(const Mat4& vp) {
  const auto& m = vp.m;
  // Rows of the matrix (it is stored column-major).
  const std::array<float, 4> r0 {m[0], m[4], m[8], m[12]};
  const std::array<float, 4> r1 {m[1], m[5], m[9], m[13]};
  const std::array<float, 4> r2 {m[2], m[6], m[10], m[14]};
  const std::array<float, 4> r3 {m[3], m[7], m[11], m[15]};

  Frustum f;
  int index = 0;
  auto add = [&](const std::array<float, 4>& a, const std::array<float, 4>& b, float sign) {
    std::array<float, 4> p {a[0] + sign * b[0], a[1] + sign * b[1], a[2] + sign * b[2],
                            a[3] + sign * b[3]};
    float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (len == 0.0f) len = 1.0f;
    for (float& v : p) v /= len;
    f.planes[index++] = p;
  };
  add(r3, r0, +1);  // left
  add(r3, r0, -1);  // right
  add(r3, r1, +1);  // bottom
  add(r3, r1, -1);  // top
  add(r3, r2, +1);  // near
  add(r3, r2, -1);  // far
  return f;
}

bool Frustum::testAabb(float minX, float minY, float minZ, float maxX, float maxY,
                       float maxZ) const {
  for (const auto& p : planes) {
    // The corner furthest along the plane normal: if even that is outside, the
    // whole box is.
    const float x = p[0] >= 0 ? maxX : minX;
    const float y = p[1] >= 0 ? maxY : minY;
    const float z = p[2] >= 0 ? maxZ : minZ;
    if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0) return false;
  }
  return true;
}

}  // namespace hr
