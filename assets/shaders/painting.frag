#version 330 core

// Writes the same three targets as gbuffer_terrain.frag, so the composite lights a
// painting on exactly the curve it lights the wall behind it — it dims indoors and
// takes shadow like anything else. That is the whole reason this is a G-buffer pass
// rather than a forward one drawn over the top.

in vec2 vUV;
in vec2 vLight;
in vec3 vWorld;

uniform sampler2D uPicture;

layout(location = 0) out vec4 oAlbedo;
layout(location = 1) out vec4 oLight;
layout(location = 2) out vec4 oNormal;

void main() {
  vec3 tex = texture(uPicture, vUV).rgb;
  // Same derivative trick the terrain uses: the quad is flat, so the cross product
  // of the screen-space world derivatives is its exact normal.
  vec3 n = normalize(cross(dFdx(vWorld), dFdy(vWorld)));

  // Shade 1.0: a painting is not a voxel face and has no per-face darkening to
  // apply. Its own picture already carries whatever light was in the photograph.
  oAlbedo = vec4(tex, 1.0);
  oLight = vec4(vLight.x, vLight.y, 0.0, 0.0);
  oNormal = vec4(n * 0.5 + 0.5, 1.0);
}
