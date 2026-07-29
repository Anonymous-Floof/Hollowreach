#version 330 core

// Entity surfaces into the G-buffer, writing the same three targets as
// gbuffer_terrain.frag so the composite pass lights a sheep exactly as it lights
// the grass under it — shade in albedo.a, the baked sky/block split in oLight,
// and a world-space normal.
//
// Surface light is one sample at the entity's own cell, passed as a uniform rather
// than per-vertex: a mob is small enough that one sample is indistinguishable from
// four, and it saves re-lighting the mesh every time it moves.

in vec2 vUV;
in float vShade;
in vec4 vColor;
in vec3 vWorld;

uniform sampler2D uAtlas;
uniform float uSky;       // 0..1 skylight at the entity's cell
uniform float uBlock;     // 0..1 block light at the entity's cell
uniform float uTextured;  // 1 for item drops, 0 for the untextured mobs
uniform vec3 uTint;       // white normally, red while a mob flashes from a hit

layout(location = 0) out vec4 oAlbedo;
layout(location = 1) out vec4 oLight;
layout(location = 2) out vec4 oNormal;

void main() {
  vec3 albedo = vColor.rgb;
  if (uTextured > 0.5) {
    vec4 tex = texture(uAtlas, vUV);
    if (tex.a < 0.5) discard;  // sprite cutouts: a dropped sword is sword-shaped
    albedo = tex.rgb * vColor.rgb;
  }
  // Same derivative trick as the terrain: every box face is flat, so the
  // cross product of the world-position derivatives is the exact normal.
  vec3 n = normalize(cross(dFdx(vWorld), dFdy(vWorld)));

  oAlbedo = vec4(albedo * uTint, vShade);
  oLight = vec4(uSky, uBlock, 0.0, 0.0);
  oNormal = vec4(n * 0.5 + 0.5, 1.0);
}
