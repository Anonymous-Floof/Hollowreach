#version 330 core

// Terrain into the G-buffer.
//
// The vertex is the packed 24-byte layout from src/world/mesher.h, so the
// attributes are unpacked here rather than arriving as six separate floats as they
// did in the web build.

#include "wave.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aPack;  // shade, skylight, blocklight, flags (0..255)
layout(location = 3) in vec4 aTint;

uniform mat4 uViewProj;
uniform float uTime;

out vec2 vUV;
out float vShade;
out float vSky;
out float vBlock;
out float vMat;
out vec3 vTint;
out vec3 vWorld;

void main() {
  vUV = aUV;
  vShade = aPack.x / 255.0;
  vSky = aPack.y / 255.0;
  vBlock = aPack.z / 255.0;
  vTint = aTint.rgb;

  float wave = floor(aPack.w + 0.5);
  // Leaves get their own material id so the composite can treat foliage
  // differently later; everything else is plain opaque.
  vMat = (wave > 0.5 && wave < 1.5) ? 1.0 : 0.0;

  vec3 pos = applyWave(aPos, wave, uTime);
  vWorld = pos;
  gl_Position = uViewProj * vec4(pos, 1.0);
}
