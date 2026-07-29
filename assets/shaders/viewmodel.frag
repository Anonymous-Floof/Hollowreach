#version 330 core

// Lit by where the player is standing, so the hand dims at night and goes
// near-black in an unlit cave — with a generous floor, since you should always be
// able to see what you are holding.

in vec2 vUV;
in float vShade;
in vec4 vColor;

uniform sampler2D uAtlas;
uniform float uLight;  // 0..1 light at the player's cell

out vec4 frag;

void main() {
  vec4 tex = texture(uAtlas, vUV);
  if (tex.a < 0.5) discard;  // sprite cutouts
  frag = vec4(tex.rgb * vColor.rgb * vShade * (0.22 + 0.78 * uLight), 1.0);
}
