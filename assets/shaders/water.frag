#version 330 core

// Water with screen-space reflections.
//
// The view ray is reflected off the rippled water normal and marched against the
// scene depth buffer; on a hit it samples a COPY of the lit scene, blitted before
// this pass so we never read the buffer we are writing. Misses — the ray escaping
// to the sky, or running off-screen — fall back to the procedural sky colour, so
// there is always a plausible reflection. Fresnel ramps reflection up at grazing
// angles, where real water turns mirror-like, and keeps it low looking straight
// down, where you see through to the bottom.

in vec2 vUV;
in float vShade;
in float vSky;
in float vBlock;
in vec3 vTint;
in vec3 vWorld;

uniform sampler2D uAtlas, uReflect, uDepth;
uniform mat4 uViewProj;
uniform vec3 uCamPos, uCamRight, uCamUp, uCamFwd;
uniform float uTanHalf, uAspect, uNear, uFar;
uniform vec3 uFogColor;
uniform float uFogNear, uFogFar;
uniform float uDaylight, uTime;
uniform vec3 uHorizon, uZenith, uSunDir, uSunColor;
uniform float uDayFactor;
uniform int uSSRSteps, uCloudSteps;
uniform float uReflStrength, uCloudCover;

out vec4 frag;

#include "sky.glsl"
#include "clouds.glsl"
#include "depthray.glsl"

void main() {
  vec4 tex = texture(uAtlas, vUV);
  if (tex.a < 0.5) discard;

  float light = max(vBlock, vSky * uDaylight);
  float b = 0.07 + 0.93 * light;
  vec3 base = tex.rgb * vTint * vShade * b;

  // Animated ripple normal. Several non-harmonic waves per axis, so the
  // perturbation shimmers organically instead of orbiting — a single sin/cos pair
  // swept every reflected feature around a little ellipse. The amplitude fades with
  // distance so far water flattens toward a mirror, which also keeps the SSR ray
  // stable and stops distant reflections wobbling.
  float t = uTime;
  float gx = sin(vWorld.x * 1.9 + t * 1.50) * 0.5 +
             sin(vWorld.x * 0.83 + vWorld.z * 1.31 + t * 1.03) * 0.3 +
             sin(vWorld.z * 2.71 + t * 0.67) * 0.2;
  float gz = cos(vWorld.z * 1.7 + t * 1.27) * 0.5 +
             cos(vWorld.x * 1.49 - vWorld.z * 0.77 + t * 0.89) * 0.3 +
             cos(vWorld.x * 2.33 + t * 1.51) * 0.2;
  float distV = distance(vWorld, uCamPos);
  float ampl = 0.05 / (1.0 + distV * 0.05);
  vec3 N = normalize(vec3(gx * ampl, 1.0, gz * ampl));

  vec3 V = normalize(vWorld - uCamPos);
  vec3 R = reflect(V, N);
  // A reflection can only come from above the surface. A ripple at a grazing view
  // can tip R below horizontal, which used to dive into the lake bed.
  if (R.y < 0.02) {
    R.y = 0.02;
    R = normalize(R);
  }

  // SSR march. When a sample lands behind above-water geometry we BISECT between
  // the previous and current sample to pin the silhouette, rather than demanding
  // the coarse sample itself sit within a thickness window. Thin features — trunks,
  // mobs, fence-width shapes — slip between the growing steps otherwise, so half the
  // pixels of a tree's mirror image miss and fall back to bright sky.
  bool hit = false;
  vec2 huv = vec2(0.0);
  float march = 0.3, stepLen = 0.35, prev = 0.3;
  for (int i = 0; i < 48; i++) {
    if (i >= uSSRSteps) break;
    vec3 sp = vWorld + R * march;
    vec4 cs = uViewProj * vec4(sp, 1.0);
    if (cs.w <= 0.0) break;
    vec2 uv = cs.xy / cs.w * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;

    float sd = texture(uDepth, uv).r;
    if (sd < 1.0) {
      vec3 scenePos = worldFromDepth(uv, sd);
      // Only geometry ABOVE the water plane can appear in a reflection. The depth
      // buffer holds the lake bed under the translucent surface, and accepting those
      // hits painted drifting sand-coloured blobs onto the water.
      if (scenePos.y > vWorld.y + 0.05) {
        float dS = distance(scenePos, uCamPos), dP = distance(sp, uCamPos);
        if (dS < dP - 0.05) {
          float lo = prev, hi = march;
          for (int r = 0; r < 5; r++) {
            float mid = (lo + hi) * 0.5;
            vec3 bp = vWorld + R * mid;
            vec4 bc = uViewProj * vec4(bp, 1.0);
            vec2 bu = bc.xy / bc.w * 0.5 + 0.5;
            float bd = texture(uDepth, bu).r;
            vec3 bs = worldFromDepth(bu, bd);
            if (bd < 1.0 && distance(bs, uCamPos) < distance(bp, uCamPos)) {
              hi = mid;
              uv = bu;
              sd = bd;
            } else {
              lo = mid;
            }
          }
          vec3 fp = vWorld + R * hi;
          vec3 fs = worldFromDepth(uv, sd);
          float fD = distance(fp, uCamPos) - distance(fs, uCamPos);
          // Accept when the refined point sits on the surface — a small residual,
          // scaled with distance for far hits — and is still above the plane.
          if (fs.y > vWorld.y + 0.05 && fD < 0.75 + distance(fp, uCamPos) * 0.02) {
            hit = true;
            huv = uv;
            break;
          }
        }
      }
    }
    prev = march;
    march += stepLen;
    stepLen *= 1.18;
  }

  vec3 skyRefl = skyColor(R, uHorizon, uZenith, uSunDir, uDayFactor, uTime);
  // Reflect the clouds too, with a cheaper march, so the sky mirror stays
  // consistent — but softened: at full strength the white cumulus fallback was
  // bright enough to visually drown the geometry hits beside it.
  vec3 amb = mix(uHorizon, uZenith, 0.5) * 1.1;
  vec4 rcl =
      renderClouds(vWorld, R, uCloudSteps, uCloudCover, uTime, uSunDir, uSunColor, amb, uDayFactor);
  rcl *= 0.55;
  skyRefl = skyRefl * (1.0 - rcl.a) + rcl.rgb;

  vec3 refl = skyRefl;
  if (hit) {
    // Fade the screen-space hit toward the sky reflection near the edges, where the
    // marched ray runs off-screen, to hide the SSR cutoff seam. The band is
    // deliberately narrow: reflected treetops land near the top of the screen, and a
    // wide fade there swapped their mirror for bright cloudy sky.
    vec2 e = smoothstep(vec2(0.0), vec2(0.05), huv) *
             (1.0 - smoothstep(vec2(0.95), vec2(1.0), huv));
    refl = mix(skyRefl, texture(uReflect, huv).rgb, e.x * e.y);
  }

  float fres = 0.04 + 0.96 * pow(1.0 - max(dot(-V, N), 0.0), 5.0);
  // Geometry mirrors get a small boost over the sky fallback, so a tree or shore
  // reflection reads clearly while open-sky glare stays as it was.
  float rs = fres * uReflStrength * (hit ? 1.35 : 1.0);
  vec3 col = mix(base, refl, clamp(rs, 0.0, 0.92));

  float dist = distance(vWorld, uCamPos);
  float fog = clamp((dist - uFogNear) / (uFogFar - uFogNear), 0.0, 1.0);
  col = mix(col, uFogColor, fog);
  frag = vec4(col, 0.85);
}
