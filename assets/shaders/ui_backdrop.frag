#version 330 core

// `backdrop-filter: blur(10px) saturate(118%)` — used on exactly one element, the menu
// card, over a slowly rotating panorama.
//
// The plan's call was that a downsample plus a blur refreshed once per frame is plenty
// here, and that is what this is: the backbuffer is copied to a quarter-size target and
// this shader runs as a separable Gaussian over it, so the effective radius is four times
// the sampling radius. Two passes, horizontal then vertical, selected by uDirection.

in vec2 vUV;
out vec4 frag;

uniform sampler2D uSource;
uniform vec2 uTexelSize;
uniform vec2 uDirection;   // (1,0) horizontal, (0,1) vertical
uniform float uSaturate;   // 1.0 = no change; the final pass applies it

void main() {
  // Nine taps of a sigma ~= 2.2 kernel, which at quarter resolution is CSS's blur(10px).
  const float weights[5] = float[5](0.2270270270, 0.1945945946, 0.1216216216,
                                    0.0540540541, 0.0162162162);
  vec2 step = uDirection * uTexelSize;
  vec3 sum = texture(uSource, vUV).rgb * weights[0];
  for (int i = 1; i < 5; ++i) {
    vec2 offset = step * float(i);
    sum += texture(uSource, vUV + offset).rgb * weights[i];
    sum += texture(uSource, vUV - offset).rgb * weights[i];
  }

  if (uSaturate != 1.0) {
    // The same luma weights the CSS filter uses.
    float luma = dot(sum, vec3(0.2126, 0.7152, 0.0722));
    sum = mix(vec3(luma), sum, uSaturate);
  }
  frag = vec4(sum, 1.0);
}
