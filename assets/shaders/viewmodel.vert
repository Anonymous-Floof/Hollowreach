#version 330 core

// The held item. One matrix does everything — pose, animation, framing and the
// close-up projection — because render/viewmodel.cpp composes them all before the
// draw. See render/itemmesh.h for the vertex layout, which is shared with entities.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in float aShade;
layout(location = 3) in float aBone;   // unused for items; entities skin with it
layout(location = 4) in vec4 aColor;

uniform mat4 uMVP;

out vec2 vUV;
out float vShade;
out vec4 vColor;

void main() {
  vUV = aUV;
  vShade = aShade;
  vColor = aColor;
  gl_Position = uMVP * vec4(aPos, 1.0);
}
