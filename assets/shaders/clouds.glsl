// Volumetric clouds.
//
// A single horizontal slab between CLD_BASE and CLD_TOP. Density is fbm value
// noise shaped by a coverage threshold and a rounded vertical falloff.
// renderClouds ray-marches the slab with a short sun-march per sample for
// self-shadowing, and returns PREMULTIPLIED colour and alpha, so the caller
// composites with base * (1 - a) + rgb.
//
// Self-contained hash so it can be included alongside sky.glsl without clashing.
// The same density function drives the ground cloud shadows in the composite, so
// the shadows line up with the clouds you can see.

#ifndef HR_CLOUDS_GLSL
#define HR_CLOUDS_GLSL

// The cloud deck rides with the sea, because the sea is where "up" is measured
// from and gen v3 moved it from y=46 to y=100. uCloudBase is fed the world's own
// sea level plus the 72 blocks that put the deck at 118 in a v2 world, so the sky
// looks the same from the ground whichever generation you are standing on. The
// slab is volumetric and lives in the sky pass, so it is free to sit above WH.
uniform float uCloudBase;
#define CLD_BASE uCloudBase
#define CLD_TOP (uCloudBase + 18.0)
#define CLD_MID (uCloudBase + 9.0)
const vec2 CLD_WIND = vec2(1.1, 0.4);

float chash13(vec3 p) {
  p = fract(p * 0.1031);
  p += dot(p, p.yzx + 33.33);
  return fract((p.x + p.y) * p.z);
}

float cnoise3(vec3 p) {
  vec3 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  float n000 = chash13(i), n100 = chash13(i + vec3(1, 0, 0));
  float n010 = chash13(i + vec3(0, 1, 0)), n110 = chash13(i + vec3(1, 1, 0));
  float n001 = chash13(i + vec3(0, 0, 1)), n101 = chash13(i + vec3(1, 0, 1));
  float n011 = chash13(i + vec3(0, 1, 1)), n111 = chash13(i + vec3(1, 1, 1));
  float nx00 = mix(n000, n100, f.x), nx10 = mix(n010, n110, f.x);
  float nx01 = mix(n001, n101, f.x), nx11 = mix(n011, n111, f.x);
  return mix(mix(nx00, nx10, f.y), mix(nx01, nx11, f.y), f.z);
}

float cloudFbm(vec3 p) {
  float a = 0.0, w = 0.55;
  for (int i = 0; i < 4; i++) {
    a += w * cnoise3(p);
    p = p * 2.03 + vec3(1.7, 0.0, 2.3);
    w *= 0.5;
  }
  return a;
}

// 0..1 density at a world point; `cover` raises coverage (bigger puffs).
float cloudDensity(vec3 wp, float cover, float t) {
  vec3 p = wp * 0.0125;
  p.xz += CLD_WIND * t * 0.013;
  float f = cloudFbm(p);
  float hb = smoothstep(CLD_BASE, CLD_BASE + 7.0, wp.y);
  float ht = 1.0 - smoothstep(CLD_TOP - 10.0, CLD_TOP, wp.y);
  float d = smoothstep(1.0 - cover, 1.0 - cover + 0.30, f) * hb * ht;
  return clamp(d, 0.0, 1.0);
}

// Premultiplied (rgb, a) contribution along the ray ro + rd, rd normalised.
vec4 renderClouds(vec3 ro, vec3 rd, int steps, float cover, float t, vec3 sunDir, vec3 sunCol,
                  vec3 ambCol, float dayF) {
  if (steps <= 0 || cover <= 0.0 || rd.y < 0.02) return vec4(0.0);

  float t0 = (CLD_BASE - ro.y) / rd.y, t1 = (CLD_TOP - ro.y) / rd.y;
  if (t0 > t1) { float s = t0; t0 = t1; t1 = s; }
  t0 = max(t0, 0.0);
  if (t1 <= t0) return vec4(0.0);
  t1 = min(t1, 6000.0);

  // Adaptive step count: `steps` is a MAXIMUM, spent on long near-horizon paths.
  // Looking straight up the slab is only ~18 blocks thick and the finest fbm octave
  // has ~9-block features, so a ~2.6-block step fully resolves it — marching the
  // full Ultra count there was six times oversampled for no visible gain, and made
  // full-sky views (every pixel a sky pixel) tank the frame rate.
  float seg = t1 - t0;
  int n = int(clamp(seg * 0.38 + 1.0, 6.0, float(steps)));
  float dt = seg / float(n);
  float jitter = chash13(rd * 137.3) * dt;

  float tr = 1.0;
  vec3 accum = vec3(0.0);

  // Bright cumulus: a neutral light-grey shadow side and a near-white sunlit side,
  // tinted by the sun colour so they warm at dusk. Kept bright so they read as
  // white puffs rather than storm grey, and never bluer than the sky behind them.
  // Constant per ray, so hoisted out of the march.
  vec3 shadeCol = mix(vec3(0.62, 0.64, 0.68), ambCol, 0.25);
  vec3 sunCloud = sunCol * 1.15;
  float dayK = 0.32 + 0.68 * dayF;

  for (int i = 0; i < 64; i++) {
    if (i >= n) break;
    float tt = t0 + dt * float(i) + jitter;
    vec3 wp = ro + rd * tt;
    float d = cloudDensity(wp, cover, t);
    if (d > 0.002) {
      float ld = 0.0;
      for (int j = 1; j <= 3; j++) ld += cloudDensity(wp + sunDir * float(j) * 5.0, cover, t);
      float sun = exp(-ld * 0.6);  // 0 shadowed core .. 1 sunward face
      vec3 lit = mix(shadeCol, sunCloud, sun) * dayK;
      float a = 1.0 - exp(-d * dt * 0.40);
      accum += tr * lit * a;
      tr *= 1.0 - a;
      if (tr < 0.02) break;
    }
  }

  float alpha = (1.0 - tr) * smoothstep(0.02, 0.14, rd.y);
  return vec4(accum * smoothstep(0.02, 0.14, rd.y), alpha);
}

// Ground cloud shadow: project a surface point up to the cloud mid-plane along the
// sun direction and sample the same density, giving moving shadows that match.
float cloudShadowAt(vec3 wpos, vec3 sunDir, float cover, float t, float enable) {
  if (enable < 0.5 || sunDir.y <= 0.05) return 1.0;
  float tt = (CLD_MID - wpos.y) / sunDir.y;
  if (tt <= 0.0) return 1.0;
  // Two taps through the slab, so a whole cloud casts rather than just its
  // mid-slice, then a crisp onset: wisps barely shade, real clouds shade firmly.
  vec3 base = wpos + sunDir * tt;
  float d = 0.5 * (cloudDensity(base, cover, t) + cloudDensity(base + sunDir * 6.0, cover, t));
  float s = smoothstep(0.04, 0.5, d);
  return 1.0 - s * 0.72;
}

#endif
