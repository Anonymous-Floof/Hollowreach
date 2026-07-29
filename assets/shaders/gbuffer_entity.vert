#version 330 core

// Entities into the G-buffer: dropped items, boats and mobs.
//
// The vertex is the shared 24-byte layout from render/itemmesh.h, which the plan's
// seventh resource-pack enabler defined so that entity meshes carry REAL UVs. The
// web build had no room for them: js/render/entityrenderer.js:62 smuggled a mob's
// colour through the aUV slot, its bone index through aSky and the blue channel
// through aBlock, which meant a Minecraft pack replacing entity/*.png had nowhere
// to put a texture. Here colour has its own RGBA8 channel, bone has its own byte,
// and uv stays uv — untextured mobs work identically and textured ones become
// possible without touching this file.
//
// Skinning is six bones, each a pivot plus a rotation about X. Bone 0 never moves,
// so a box with no bone index rides the model matrix alone.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in float aShade;
layout(location = 3) in float aBone;
layout(location = 4) in vec4 aColor;

uniform mat4 uViewProj;
uniform mat4 uModel;
uniform vec4 uBones[6];  // xyz = pivot in model space, w = angle about X

out vec2 vUV;
out float vShade;
out vec4 vColor;
out vec3 vWorld;

vec3 skin(vec3 p) {
  int b = int(aBone + 0.5);
  if (b <= 0) return p;
  vec4 bone = uBones[b];
  float a = bone.w;
  if (a == 0.0) return p;
  vec3 local = p - bone.xyz;
  float c = cos(a), s = sin(a);
  return bone.xyz + vec3(local.x, local.y * c - local.z * s, local.y * s + local.z * c);
}

void main() {
  vUV = aUV;
  vShade = aShade;
  vColor = aColor;
  vec4 world = uModel * vec4(skin(aPos), 1.0);
  vWorld = world.xyz;
  gl_Position = uViewProj * world;
}
