#version 330 core

// Forward-shaded terrain.
//
// A stand-in for the deferred pipeline while the world is brought up: it applies
// the baked vertex shade and light directly, with distance fog. The deferred G-buffer
// path replaces it, and this stays as a fallback for driver trouble and as a
// reference for what the baked vertex data means on its own.

in vec2 vUV;
in float vShade;
in float vSky;
in float vBlock;
in vec3 vTint;
in vec3 vWorld;

out vec4 oColor;

uniform sampler2D uAtlas;
uniform float uDaylight;   // 0.12 .. 1.0
uniform vec3 uFogColor;
uniform float uFogNear;
uniform float uFogFar;
uniform vec3 uCamPos;
uniform float uAlphaCutoff;

void main() {
  vec4 tex = texture(uAtlas, vUV);
  // Cutout transparency. Leaves, glass, plants and torches all live in the opaque
  // pass and resolve here, which is why none of them need depth sorting.
  if (tex.a < uAlphaCutoff) discard;

  // Skylight scales with the time of day; block light does not.
  float baseLight = max(vBlock, vSky * uDaylight);
  float lit = 0.06 + 0.94 * baseLight;

  vec3 albedo = tex.rgb * vTint;
  vec3 col = albedo * vShade * lit;

  float dist = length(vWorld - uCamPos);
  float fog = clamp((dist - uFogNear) / max(uFogFar - uFogNear, 1e-4), 0.0, 1.0);
  col = mix(col, uFogColor, fog);

  oColor = vec4(col, 1.0);
}
