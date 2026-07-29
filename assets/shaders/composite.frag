#version 330 core

// Deferred lighting.
//
// Reads the G-buffer and lights the surface from: a directional sun or moon with
// cast shadows, a warm baked block-light fill, screen-space AO multiplied into the
// baked face shade, and a list of coloured dynamic point lights. Sky pixels take a
// separate branch that marches the volumetric clouds.
//
// Colours are first-class throughout, so future coloured glass or fire only has to
// feed a different light colour.

in vec2 vUv;

uniform sampler2D uAlbedo, uLight, uNormal, uDepth;
uniform sampler2D uShadowMap;
uniform sampler2D uSSAO;  // blurred screen-space AO, 1 = no occlusion

uniform mat4 uViewProj, uLightVP;
uniform vec3 uCamPos, uFogColor;
uniform vec3 uCamRight, uCamUp, uCamFwd;
uniform float uTanHalf, uAspect, uNear, uFar;
uniform float uFogNear, uFogFar;

uniform vec3 uSunDir, uSunColor;
uniform float uSunStrength;
uniform float uDaylight;  // 0.12 .. 1.0, the day/night skylight exposure

uniform float uShadowEnable;
uniform float uShadowTexel;       // 1 / shadowMapSize, the PCF tap spacing
uniform float uShadowTexelWorld;  // world size of one shadow texel, for normal offset
uniform float uShadowBias;

uniform int uShadowSteps;   // point-light contact-shadow steps, 0 = off
uniform float uShadowDist;  // world-space march length for point lights
uniform float uLightShadow;

uniform int uLightCount;
uniform vec3 uLightPos[MAX_LIGHTS];
uniform vec3 uLightColor[MAX_LIGHTS];
uniform float uLightRad[MAX_LIGHTS];

uniform float uTime, uCloudCover, uCloudShadow;
uniform int uCloudSteps;
uniform vec3 uCloudSunDir, uCloudAmb;
uniform float uCloudDay;

uniform float uDebug;  // 1 = show the AO term, 2 = show the sun shadow term

out vec4 frag;

#include "depthray.glsl"
#include "clouds.glsl"

// Marches from a surface point toward a light in world space, projecting each step
// to screen and comparing camera distance against the stored depth: if real
// geometry sits in front of the marched point, within a thickness, it occludes.
// Returns a SOFT factor: occluders hit close to the surface shadow more, and the
// shadow fades along the ray, so the result is a contact shadow rather than a hard
// binary edge.
float contactShadow(vec3 wpos, vec3 L, float maxDist, int steps) {
  float dt = maxDist / float(steps);
  float t = dt * 0.5 + 0.03;
  for (int i = 0; i < 32; i++) {
    if (i >= steps) break;
    vec3 p = wpos + L * t;
    vec4 cs = uViewProj * vec4(p, 1.0);
    if (cs.w <= 0.0) break;
    vec2 uv = (cs.xy / cs.w) * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;
    float sd = texture(uDepth, uv).r;
    if (sd < 1.0) {
      vec3 sPos = worldFromDepth(uv, sd);
      float dP = distance(p, uCamPos);
      float dS = distance(sPos, uCamPos);
      if (dS < dP - 0.04 && (dP - dS) < 1.0) {
        float closeness = 1.0 - t / maxDist;
        return 1.0 - 0.7 * closeness * closeness;
      }
    }
    t += dt;
  }
  return 1.0;
}

// Sun cast-shadow from the depth map. The reconstructed world position is pushed
// off the surface along the normal by a couple of texels (normal-offset bias) so
// flat lit ground does not self-shadow; a slope-scaled depth bias handles the rest.
// 3x3 PCF softens the edge, and the term fades to fully lit at the map border so the
// finite shadow range has no visible boundary.
float sunShadow(vec3 wpos, vec3 N, float ndl) {
  if (uShadowEnable < 0.5 || ndl <= 0.0) return 1.0;

  float off = uShadowTexelWorld * (3.0 + 4.0 * (1.0 - ndl));
  vec4 lc = uLightVP * vec4(wpos + N * off, 1.0);
  vec3 p = lc.xyz / lc.w * 0.5 + 0.5;
  if (p.z > 1.0) return 1.0;

  vec2 f = smoothstep(vec2(0.0), vec2(0.08), p.xy) *
           (1.0 - smoothstep(vec2(0.92), vec2(1.0), p.xy));
  float edge = f.x * f.y;
  if (edge <= 0.0) return 1.0;

  float slope = sqrt(max(0.0, 1.0 - ndl * ndl)) / max(ndl, 0.2);
  float bias = clamp(uShadowBias * slope, uShadowBias * 0.5, uShadowBias * 4.0);
  float zref = p.z - bias;

  float sh = 0.0;
  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      sh += zref <= texture(uShadowMap, p.xy + vec2(float(x), float(y)) * uShadowTexel).r
                ? 1.0
                : 0.0;
    }
  }
  return mix(1.0, sh / 9.0, edge);
}

void main() {
  vec4 A = texture(uAlbedo, vUv);
  vec3 albedo = A.rgb;
  vec4 L = texture(uLight, vUv);

  if (L.a >= 0.99) {
    // Sky. Clouds are marched only here — for pixels that survived the depth test —
    // so the layer costs nothing over the part of the screen terrain covers.
    vec2 ndc = vUv * 2.0 - 1.0;
    vec3 dir = normalize(uCamFwd + ndc.x * uTanHalf * uAspect * uCamRight +
                         ndc.y * uTanHalf * uCamUp);
    vec4 cl = renderClouds(uCamPos, dir, uCloudSteps, uCloudCover, uTime, uCloudSunDir,
                           uSunColor, uCloudAmb, uCloudDay);
    frag = vec4(albedo * (1.0 - cl.a) + cl.rgb, 1.0);
    return;
  }

  float depth = texture(uDepth, vUv).r;
  float shade = A.a;  // baked face shade times vertex AO
  float sky = L.r, block = L.g;
  vec3 N = normalize(texture(uNormal, vUv).rgb * 2.0 - 1.0);
  vec3 wpos = worldFromDepth(vUv, depth);

  float ssao = texture(uSSAO, vUv).r;
  if (uDebug > 0.5) {
    if (uDebug < 1.5) {
      frag = vec4(vec3(ssao), 1.0);
      return;
    }
    float ndlD = max(dot(N, uSunDir), 0.0);
    frag = vec4(vec3(sunShadow(wpos, N, ndlD)), 1.0);
    return;
  }

  // The baked voxel face shade — top bright, sides darker, plus vertex AO — is the
  // PRIMARY contrast, exactly as in the original forward look. Screen-space AO
  // multiplies into it to deepen corners the baked term cannot see. Kept at full
  // strength rather than softened, so blocks read crisp and dimensional.
  float occ = shade * ssao;

  // Ambient-floored max(blocklight, skylight * daylight), so dawn, dusk and night
  // dim and the scene never washes past the texture colour.
  float baseLight = max(block, sky * uDaylight);
  float b = 0.06 + 0.94 * baseLight;
  vec3 col = albedo * occ * b;

  // Warm directional sun on sky-lit faces turned toward it, attenuated by the cast
  // shadow — so a shadow reads as a loss of direct sun while the ambient still fills
  // it. Kept moderate: it adds shape and shadow contrast without the blowout of a
  // full relight.
  float ndl = max(dot(N, uSunDir), 0.0);
  float sunLit = ndl * smoothstep(0.05, 0.45, sky) * uSunStrength;
  // Skipped entirely when the pixel receives no direct sun anyway (faces turned
  // away, cave interiors, sun below the horizon): the term multiplies to exactly
  // zero, but the 3x3 PCF fetches and the two cloud-density fbms behind it are the
  // most expensive per-pixel work in this shader.
  if (sunLit > 0.0) {
    float shadow = sunShadow(wpos, N, ndl) *
                   cloudShadowAt(wpos, uCloudSunDir, uCloudCover, uTime, uCloudShadow);
    col += albedo * occ * uSunColor * (sunLit * 0.30 * shadow);
  }

  // Coloured point lights: the held torch plus nearby emitters. Additive, tinted by
  // the surface albedo, attenuated, and AO-respecting. Damped on surfaces already in
  // bright daylight — a torch should not visibly over-brighten noon grass, the same
  // way the baked max(block, sky) term saturates — while night and underground are
  // unchanged.
  float dayDamp = 1.0 - 0.72 * uDaylight * smoothstep(0.35, 0.9, sky);
  for (int i = 0; i < MAX_LIGHTS; i++) {
    if (i >= uLightCount) break;
    vec3 d = uLightPos[i] - wpos;
    float dist = length(d);
    float r = uLightRad[i];
    if (dist >= r) continue;
    vec3 Ld = d / max(dist, 1e-3);
    float at = 1.0 - dist / r;
    at *= at;
    // Wrapped, so a face turned away from a nearby torch is dim rather than black.
    float ndlP = max(dot(N, Ld), 0.0) * 0.75 + 0.25;
    float sh = 1.0;
    if (uLightShadow > 0.5 && uShadowSteps > 0) {
      sh = contactShadow(wpos, Ld, min(r, dist), uShadowSteps / 2);
    }
    col += albedo * occ * uLightColor[i] * (at * ndlP * sh * dayDamp);
  }

  float dist = distance(wpos, uCamPos);
  float fog = clamp((dist - uFogNear) / (uFogFar - uFogNear), 0.0, 1.0);
  col = mix(col, uFogColor, fog);
  frag = vec4(col, 1.0);
}
