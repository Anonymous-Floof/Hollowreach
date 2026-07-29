#version 330 core

// Water draws forward, translucent, over the already-lit opaque scene.

#include "wave.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aPack;
layout(location = 3) in vec4 aTint;

uniform mat4 uViewProj;
uniform float uTime;

out vec2 vUV;
out float vShade;
out float vSky;
out float vBlock;
out vec3 vTint;
out vec3 vWorld;

void main() {
  vUV = aUV;
  vShade = aPack.x / 255.0;
  vSky = aPack.y / 255.0;
  vBlock = aPack.z / 255.0;
  vTint = aTint.rgb;

  vec3 pos = applyWave(aPos, floor(aPack.w + 0.5), uTime);
  vWorld = pos;
  gl_Position = uViewProj * vec4(pos, 1.0);
}
