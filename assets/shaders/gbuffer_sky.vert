#version 330 core

// The sky fills the G-buffer background with a fullscreen triangle. Rather than
// unprojecting, the world-space view ray is built directly from the camera basis,
// which is both cheaper and numerically better behaved.

layout(location = 0) in vec2 aPos;

uniform vec3 uRight, uUp, uFwd;
uniform float uTanHalf, uAspect;

out vec3 vRay;

void main() {
  vRay = uFwd + aPos.x * uTanHalf * uAspect * uRight + aPos.y * uTanHalf * uUp;
  // z = w = 1 puts it on the far plane, so any geometry overwrites it.
  gl_Position = vec4(aPos, 1.0, 1.0);
}
