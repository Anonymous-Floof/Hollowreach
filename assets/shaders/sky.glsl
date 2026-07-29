// Sky colour for a world-space ray direction: gradient, sun disc and glow, moon,
// and twinkling stars. Shared by the G-buffer sky pass and the water reflection,
// so a reflected sky is the same sky.

#ifndef HR_SKY_GLSL
#define HR_SKY_GLSL

float hash13(vec3 p) {
  p = fract(p * 0.1031);
  p += dot(p, p.yzx + 33.33);
  return fract((p.x + p.y) * p.z);
}

float starField(vec3 dir, float t) {
  vec3 d = dir * 90.0;
  vec3 c = floor(d), f = fract(d);
  float h = hash13(c);
  if (h < 0.975) return 0.0;
  vec3 pp = vec3(hash13(c + 1.7), hash13(c + 4.3), hash13(c + 8.1));
  float dist = length(f - pp);
  float tw = 0.6 + 0.4 * sin(t * 2.5 + h * 60.0);
  return smoothstep(0.10, 0.0, dist) * tw;
}

vec3 skyColor(vec3 dir, vec3 horizon, vec3 zenith, vec3 sunDir, float dayFactor, float t) {
  float h = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
  vec3 col = mix(horizon, zenith, pow(h, 0.8));

  float night = clamp(1.0 - dayFactor * 1.6, 0.0, 1.0);
  if (night > 0.01 && dir.y > 0.0) {
    col += vec3(starField(dir, t) * night * smoothstep(0.0, 0.15, dir.y));
  }

  float sd = dot(dir, sunDir);
  float sunDisc = smoothstep(0.9975, 0.9990, sd);
  float sunGlow = smoothstep(0.95, 1.0, sd) * 0.35 * dayFactor;
  col += vec3(1.0, 0.93, 0.74) * (sunDisc + sunGlow);

  float md = dot(dir, -sunDir);
  float moonDisc = smoothstep(0.9980, 0.9992, md);
  float mottle = 0.75 + 0.25 * hash13(floor(dir * 240.0));
  col += vec3(0.85, 0.88, 0.95) * moonDisc * mottle * (0.4 + 0.6 * night);

  return col;
}

#endif
