#version 330 core

// Shared fullscreen pass vertex shader.
//
// Every post/composite pass in the web build drew one oversized triangle rather
// than a quad — three vertices, no diagonal seam, and the rasteriser clips the
// overhang for free. The attribute is the clip-space position directly.

layout(location = 0) in vec2 aPos;

out vec2 vUv;

void main() {
  vUv = aPos * 0.5 + 0.5;
  gl_Position = vec4(aPos, 0.0, 1.0);
}
