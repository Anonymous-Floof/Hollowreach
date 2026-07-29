#version 330 core

// Final present: copies the lit buffer to the screen, upscaling from the internal
// render scale, adds the god-ray shafts, and applies the underwater post.

in vec2 vUv;

uniform sampler2D uTex, uGodray, uDepth;
uniform float uGodrayEnable;
uniform float uUnderwater;  // 0..1 strength of the submerged effect
uniform float uTime, uNear, uFar;
uniform vec3 uWaterTint;

out vec4 frag;

void main() {
  vec2 uv = vUv;

  // Submerged: a slow refractive wobble of the whole image.
  if (uUnderwater > 0.001) {
    uv += vec2(sin(uv.y * 16.0 + uTime * 1.6), cos(uv.x * 14.0 + uTime * 1.3)) * 0.0035 *
          uUnderwater;
    uv = clamp(uv, 0.0, 1.0);
  }

  vec3 c = texture(uTex, uv).rgb;
  c += uGodrayEnable * texture(uGodray, uv).rgb;

  // The composite is energy-bounded (diffuse never exceeds roughly the albedo), so
  // tones stay LINEAR below 1.0 — preserving the crisp textured contrast — and only
  // the rare overshoot above 1.0, from bright torches or god-ray shafts, is softly
  // rolled off instead of clipping to flat white.
  vec3 over = max(c - 1.0, 0.0);
  c = min(c, vec3(1.0)) + over / (1.0 + over);

  // Underwater: blue murk thickening with distance, a dim blue cast, and a soft
  // vignette, so being submerged reads clearly instead of looking like air.
  if (uUnderwater > 0.001) {
    float d = texture(uDepth, uv).r;
    float lin = (2.0 * uNear * uFar) / (uFar + uNear - (d * 2.0 - 1.0) * (uFar - uNear));
    float murk = clamp(1.0 - exp(-lin * 0.12), 0.0, 0.95) * uUnderwater;
    c = mix(c, uWaterTint, murk);
    c = mix(c, c * vec3(0.55, 0.82, 1.05), 0.40 * uUnderwater);
    c *= mix(1.0, 0.80, uUnderwater);
    vec2 q = vUv - 0.5;
    float vig = smoothstep(0.85, 0.30, length(q));
    c *= mix(1.0, vig, 0.45 * uUnderwater);
  }

  frag = vec4(c, 1.0);
}
