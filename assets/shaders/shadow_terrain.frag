#version 330 core

// Depth-only, but cutout-accurate: discarding transparent texels is what makes a
// canopy cast dappled shadow instead of a solid quad.

in vec2 vUV;
uniform sampler2D uAtlas;

void main() {
  if (texture(uAtlas, vUV).a < 0.5) discard;
}
