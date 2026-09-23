#version 330 core

// Depth-only, cutout-accurate for textured drops: a dropped pickaxe should throw a
// pickaxe-shaped shadow, not the shadow of the quad it is painted on. Mob surface
// tiles are opaque, so the test passes everywhere on a mob; uCutout still switches it
// off for a mesh built without UVs, rather than sampling a texture they do not point
// into.

in vec2 vUV;

uniform sampler2D uAtlas;
uniform float uCutout;

void main() {
  if (uCutout > 0.5 && texture(uAtlas, vUV).a < 0.5) discard;
}
