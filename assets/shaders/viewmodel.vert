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
// The dye on the stack being held, or white. A uniform rather than baked into
// aColor: item meshes are cached per item key, so a per-vertex colour would mean a
// whole second mesh for every shade the player ever mixed, and the cache is keyed by
// a string. One vec4 per draw costs nothing and the cache stays as it was.
//
// NOT called uTint: gbuffer_entity.frag already has a `uniform vec3 uTint` for the
// red flash a mob shows when hit, and a same-named uniform of a different type in
// another stage of a sibling program is the kind of thing that links on one driver
// and not the next.
uniform vec4 uDye = vec4(1.0);

out vec2 vUV;
out float vShade;
out vec4 vColor;

void main() {
  vUV = aUV;
  vShade = aShade;
  vColor = aColor * uDye;
  gl_Position = uMVP * vec4(aPos, 1.0);
}
