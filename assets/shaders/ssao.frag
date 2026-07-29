#version 330 core

// Screen-space ambient occlusion, in its own pass so the result can be blurred.
// An inline un-blurred SSAO leaves per-pixel rotation grain; writing it to a buffer
// and cleaning it with a depth-aware blur removes that.

in vec2 vUv;

uniform sampler2D uDepth, uNormal;
uniform mat4 uViewProj;
uniform vec3 uCamPos, uCamRight, uCamUp, uCamFwd;
uniform float uTanHalf, uAspect, uNear, uFar;
uniform int uSamples;
uniform float uRadius, uStrength;

out vec4 frag;

#include "depthray.glsl"

float hash12(vec2 p) {
  vec3 q = fract(vec3(p.xyx) * 0.1031);
  q += dot(q, q.yzx + 33.33);
  return fract((q.x + q.y) * q.z);
}

void main() {
  float d = texture(uDepth, vUv).r;
  if (d >= 1.0) {
    frag = vec4(1.0);  // sky occludes nothing
    return;
  }

  vec3 N = normalize(texture(uNormal, vUv).rgb * 2.0 - 1.0);
  vec3 wpos = worldFromDepth(vUv, d);

  // A per-pixel random tangent basis, so the sample pattern rotates and the
  // under-sampling shows up as grain the blur can remove rather than banding.
  vec3 rv = normalize(vec3(hash12(vUv * 131.7) * 2.0 - 1.0, hash12(vUv * 71.3 + 5.1) * 2.0 - 1.0,
                           hash12(vUv * 43.9 + 9.7)));
  vec3 T = normalize(rv - N * dot(rv, N));
  vec3 B = cross(N, T);

  float occ = 0.0;
  for (int i = 0; i < 32; i++) {
    if (i >= uSamples) break;
    float fi = float(i) + 0.5;
    vec3 h = normalize(vec3(hash12(vec2(fi, 1.3)) * 2.0 - 1.0, hash12(vec2(fi, 2.7)) * 2.0 - 1.0,
                            hash12(vec2(fi, 3.9)) * 0.85 + 0.15));
    // Quadratic distribution: more samples close to the surface, where contact
    // occlusion actually lives.
    float scale = fi / float(uSamples);
    scale = mix(0.12, 1.0, scale * scale);
    vec3 sp = wpos + (T * h.x + B * h.y + N * h.z) * uRadius * scale;

    vec4 cs = uViewProj * vec4(sp, 1.0);
    if (cs.w <= 0.0) continue;
    vec2 suv = cs.xy / cs.w * 0.5 + 0.5;
    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;
    float sd = texture(uDepth, suv).r;
    if (sd >= 1.0) continue;

    vec3 op = worldFromDepth(suv, sd);
    // Fades out occluders far outside the radius, so a distant wall behind a gap
    // does not darken the near surface.
    float rangeCheck = 1.0 - smoothstep(uRadius * 0.5, uRadius, distance(op, wpos));
    if (distance(op, uCamPos) < distance(sp, uCamPos) - 0.03) occ += rangeCheck;
  }

  float ao = 1.0 - (occ / float(uSamples)) * uStrength;
  frag = vec4(clamp(ao, 0.0, 1.0));
}
