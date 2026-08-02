#version 330 core

// A picture hung on a wall. Corners arrive already in world space — there are only
// four possible orientations and the CPU writes them out, so there is no model
// matrix here.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec2 aLight;  // skylight, blocklight, both 0..1

uniform mat4 uViewProj;

out vec2 vUV;
out vec2 vLight;
out vec3 vWorld;

void main() {
  vUV = aUV;
  vLight = aLight;
  vWorld = aPos;
  gl_Position = uViewProj * vec4(aPos, 1.0);
}
