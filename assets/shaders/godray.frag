#version 330 core

// Volumetric light scattering as a post-process.
//
// For each pixel, march toward the sun's screen position sampling the depth buffer:
// sky texels along the path scatter light, geometry occludes it. Accumulated with
// exponential decay this gives radial shafts streaming past silhouettes.
// Run at half resolution; the bilinear upscale softens it.
//
// (Mitchell, GPU Gems 3, "Volumetric Light Scattering as a Post-Process".)

in vec2 vUv;

uniform sampler2D uDepth;
uniform vec2 uSunScreen;     // sun position in uv space; may lie outside 0..1
uniform vec3 uSunColor;
uniform float uGodrayValid;  // 0..1: sun in front of the camera, above the horizon
uniform float uGodrayStrength;
uniform int uGodraySamples;

out vec4 frag;

void main() {
  if (uGodrayValid <= 0.0 || uGodraySamples <= 0) {
    frag = vec4(0.0, 0.0, 0.0, 1.0);
    return;
  }

  vec2 delta = (uSunScreen - vUv) / float(uGodraySamples) * 0.92;
  vec2 uv = vUv;
  float illum = 1.0, accum = 0.0;

  for (int i = 0; i < 96; i++) {
    if (i >= uGodraySamples) break;
    uv += delta;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;
    float d = texture(uDepth, uv).r;
    accum += (d >= 0.9999 ? 1.0 : 0.0) * illum;
    illum *= 0.955;  // decay along the ray
  }

  accum /= float(uGodraySamples);
  frag = vec4(uSunColor * (accum * uGodrayStrength * uGodrayValid), 1.0);
}
