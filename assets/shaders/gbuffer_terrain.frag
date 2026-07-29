#version 330 core

in vec2 vUV;
in float vShade;
in float vSky;
in float vBlock;
in float vMat;
in vec3 vTint;
in vec3 vWorld;

uniform sampler2D uAtlas;

layout(location = 0) out vec4 oAlbedo;
layout(location = 1) out vec4 oLight;
layout(location = 2) out vec4 oNormal;

void main() {
  vec4 tex = texture(uAtlas, vUV);
  // Cutout transparency resolves here, in the opaque pass — which is why leaves,
  // glass, plants and torches never need depth sorting.
  if (tex.a < 0.5) discard;

  // Normals come from screen-space derivatives of the interpolated world position,
  // so the mesher never has to emit them. A voxel face is flat, so the derivative
  // is exact rather than an approximation.
  vec3 n = normalize(cross(dFdx(vWorld), dFdy(vWorld)));

  oAlbedo = vec4(tex.rgb * vTint, vShade);
  oLight = vec4(vSky, vBlock, 0.0, vMat / 255.0);
  oNormal = vec4(n * 0.5 + 0.5, 1.0);
}
