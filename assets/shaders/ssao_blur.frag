#version 330 core

// Depth-aware blur, one axis per pass.
//
// Averages the AO over a neighbourhood, killing the dithering grain, but weights
// down samples across a depth step so occlusion does not bleed over silhouette
// edges. Depth is linearised first, so the rejection threshold is in blocks rather
// than in the non-linear depth buffer's units.
//
// This was a single 5x5 pass: 25 positions, each fetching both the AO and the
// depth, plus 25 exp(). Run separably as 5 horizontal then 5 vertical it is 10
// positions for the same radius. A depth-WEIGHTED blur is not strictly separable
// — the weight at (x,y) is not the product of the weights at (x,0) and (0,y) —
// so this is an approximation rather than an identity. It is the standard one,
// and at a five-tap radius on a term that exists to be soft, the difference does
// not survive being looked at.

in vec2 vUv;

uniform sampler2D uSSAO, uDepth;
uniform vec2 uTexel;
uniform vec2 uDir;  // (1,0) horizontal pass, (0,1) vertical
uniform float uNear, uFar;

out vec4 frag;

float lin(float d) {
  return (2.0 * uNear * uFar) / (uFar + uNear - (d * 2.0 - 1.0) * (uFar - uNear));
}

void main() {
  float dc = lin(texture(uDepth, vUv).r);
  float sum = 0.0, wsum = 0.0;
  for (int i = -2; i <= 2; i++) {
    vec2 uv = vUv + uDir * float(i) * uTexel;
    float dn = lin(texture(uDepth, uv).r);
    float w = exp(-abs(dn - dc) * 1.5);  // rejects across roughly a one-block step
    sum += texture(uSSAO, uv).r * w;
    wsum += w;
  }
  frag = vec4(sum / max(wsum, 1e-4));
}
