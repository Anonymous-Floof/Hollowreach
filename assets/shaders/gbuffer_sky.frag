#version 330 core

#include "sky.glsl"

in vec3 vRay;

uniform vec3 uHorizon, uZenith, uSunDir;
uniform float uDayFactor, uTime;

layout(location = 0) out vec4 oAlbedo;
layout(location = 1) out vec4 oLight;
layout(location = 2) out vec4 oNormal;

void main() {
  vec3 dir = normalize(vRay);
  vec3 col = skyColor(dir, uHorizon, uZenith, uSunDir, uDayFactor, uTime);

  oAlbedo = vec4(col, 1.0);
  // matId 255/255 marks sky: the composite passes it through unlit.
  oLight = vec4(0.0, 0.0, 0.0, 1.0);
  oNormal = vec4(0.5, 0.5, 0.5, 0.0);

  // Clouds are marched later, in the composite's sky-pixel branch — only for the
  // pixels that actually stay sky, not the many that terrain overwrites.
}
