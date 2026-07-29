// Colour helpers shared by the UI and backdrop shaders.
//
// The whole pipeline is LDR RGBA8 with no framebuffer linearisation, exactly as
// in the web build, so "gamma space" here means the raw 8-bit values a browser
// would have blended. CSS gradients interpolate in that same non-linear space,
// which is why mixGamma is a plain mix and not a linearising one — matching the
// original's look matters more than being physically correct.

#ifndef HR_SRGB_GLSL
#define HR_SRGB_GLSL

vec3 mixGamma(vec3 a, vec3 b, float t) { return mix(a, b, t); }

// Ordered 8x8 Bayer dither, returning roughly [-0.5, 0.5] of one 8-bit step.
//
// A long, shallow gradient across an RGBA8 target bands into visible rings.
// Browsers dither their CSS gradients, so adding sub-LSB noise is what keeps the
// ported backdrop looking like the original rather than introducing an artefact
// the web build did not have.
float bayerDither(vec2 fragCoord) {
  int x = int(mod(fragCoord.x, 8.0));
  int y = int(mod(fragCoord.y, 8.0));
  // Bit-reversal interleave of x and y — the standard 8x8 Bayer construction.
  int index = 0;
  int bit = 0;
  for (int i = 0; i < 3; i++) {
    int xb = (x >> i) & 1;
    int yb = (y >> i) & 1;
    index |= (xb ^ yb) << bit;
    bit++;
    index |= yb << bit;
    bit++;
  }
  return (float(index) / 64.0 - 0.5) / 255.0;
}

// #rrggbb literals appear throughout the ported CSS theme.
vec3 hexToRgb(int hex) {
  return vec3(float((hex >> 16) & 255), float((hex >> 8) & 255), float(hex & 255)) / 255.0;
}

#endif
