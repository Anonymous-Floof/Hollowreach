#version 330 core

// Depth-only terrain from the sun's point of view.
//
// The wave displacement must match gbuffer_terrain.vert exactly, or a swaying
// leaf's shadow detaches from the leaf.

#include "wave.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aPack;

uniform mat4 uLightVP;
uniform float uTime;

out vec2 vUV;

void main() {
  vUV = aUV;
  // Water is excluded from the shadow pass entirely, so only the leaf branch can
  // apply here; passing the flag through keeps the two shaders textually identical.
  vec3 pos = applyWave(aPos, floor(aPack.w + 0.5), uTime);
  gl_Position = uLightVP * vec4(pos, 1.0);
}
