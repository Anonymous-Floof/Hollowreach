#version 330 core

// Entities depth-only from the sun's point of view, so mobs and drops cast
// shadows. The skinning must match gbuffer_entity.vert exactly, or a walking
// sheep's legs shadow where they were rather than where they are.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in float aShade;
layout(location = 3) in float aBone;
layout(location = 4) in vec4 aColor;

uniform mat4 uLightVP;
uniform mat4 uModel;
uniform vec4 uBones[6];

out vec2 vUV;

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
  gl_Position = uLightVP * uModel * vec4(skin(aPos), 1.0);
}
