#version 330 core

// The plain (no-panorama) screen background.
//
// Ported from css/style.css:34-42:
//   radial-gradient(circle at 50% 30%, #1d2733, #0e1218 75%)
// The CSS circle radius defaults to farthest-corner, so the stop positions are
// fractions of the distance from (50%, 30%) to whichever corner is furthest —
// which is what uAspect is for.

#include "srgb.glsl"

in vec2 vUv;
out vec4 oColor;

uniform float uAspect;

const vec3 kInner = vec3(0.114, 0.153, 0.200);  // #1d2733
const vec3 kOuter = vec3(0.055, 0.071, 0.094);  // #0e1218

void main() {
  // CSS y grows downward; our uv grows upward, so the 30% centre is at 0.70.
  vec2 centre = vec2(0.5, 0.70);
  vec2 d = (vUv - centre) * vec2(uAspect, 1.0);

  // farthest-corner radius, in the same aspect-corrected space.
  vec2 far = max(abs(vec2(0.0, 0.0) - centre), abs(vec2(1.0, 1.0) - centre));
  float radius = length(far * vec2(uAspect, 1.0));

  float t = clamp(length(d) / max(radius * 0.75, 1e-5), 0.0, 1.0);
  vec3 col = mixGamma(kInner, kOuter, t) + bayerDither(gl_FragCoord.xy);
  oColor = vec4(col, 1.0);
}
