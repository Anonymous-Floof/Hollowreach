#version 330 core

// Depth-aware 5x5 box blur.
//
// Averages the AO over a neighbourhood, killing the dithering grain, but weights
// down samples across a depth step so occlusion does not bleed over silhouette
// edges. Depth is linearised first, so the rejection threshold is in blocks rather
// than in the non-linear depth buffer's units.

in vec2 vUv;

uniform sampler2D uSSAO, uDepth;
uniform vec2 uTexel;
uniform float uNear, uFar;

out vec4 frag;

float lin(float d) {
  return (2.0 * uNear * uFar) / (uFar + uNear - (d * 2.0 - 1.0) * (uFar - uNear));
}

void main() {
  float dc = lin(texture(uDepth, vUv).r);
  float sum = 0.0, wsum = 0.0;
  for (int y = -2; y <= 2; y++) {
    for (int x = -2; x <= 2; x++) {
      vec2 uv = vUv + vec2(float(x), float(y)) * uTexel;
      float dn = lin(texture(uDepth, uv).r);
      float w = exp(-abs(dn - dc) * 1.5);  // rejects across roughly a one-block step
      sum += texture(uSSAO, uv).r * w;
      wsum += w;
    }
  }
  frag = vec4(sum / max(wsum, 1e-4));
}
