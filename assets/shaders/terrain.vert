#version 330 core

// Terrain vertex shader.
//
// Positions arrive in world space, so there is no model matrix — every chunk is
// one draw call against the same view-projection. The packed attributes are
// unpacked here; see src/world/mesher.h for the 24-byte layout.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;     // normalised ushort
layout(location = 2) in vec4 aPack;   // shade, skylight, blocklight, flags (0..255)
layout(location = 3) in vec4 aTint;   // normalised ubyte, white today

uniform mat4 uViewProj;
uniform float uTime;

out vec2 vUV;
out float vShade;
out float vSky;
out float vBlock;
out vec3 vTint;
out vec3 vWorld;

// Atmospheric motion, from the low two bits of the flags byte.
const float WAVE_NONE = 0.0;
const float WAVE_LEAF = 1.0;
const float WAVE_WATER = 2.0;

void main() {
  vUV = aUV;
  vShade = aPack.x / 255.0;
  vSky = aPack.y / 255.0;
  vBlock = aPack.z / 255.0;
  vTint = aTint.rgb;

  vec3 world = aPos;

  // Leaf sway and the water ripple share one displacement, differing only in
  // amplitude and axis. Driving it from world position rather than a per-vertex
  // phase keeps neighbouring blocks in step, so a canopy moves as one mass.
  float wave = floor(aPack.w + 0.5);
  if (wave == WAVE_LEAF) {
    float phase = uTime * 1.4 + world.x * 0.35 + world.z * 0.27;
    world.x += sin(phase) * 0.045;
    world.z += cos(phase * 0.85) * 0.035;
    world.y += sin(phase * 1.3) * 0.015;
  } else if (wave == WAVE_WATER) {
    float phase = uTime * 1.1 + world.x * 0.6 + world.z * 0.45;
    world.y += sin(phase) * 0.022 + cos(phase * 1.7) * 0.012;
  }

  vWorld = world;
  gl_Position = uViewProj * vec4(world, 1.0);
}
