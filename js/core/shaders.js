// GLSL sources for the three render programs: terrain (textured voxels),
// sky (fullscreen gradient), and lines (selection outline / held wireframes).

export const TERRAIN_VS = `#version 300 es
precision highp float;
layout(location=0) in vec3 aPos;     // world-space position
layout(location=1) in vec2 aUV;      // atlas uv
layout(location=2) in float aShade;  // face brightness * ambient occlusion
layout(location=3) in float aSky;    // skylight 0..1
layout(location=4) in float aBlock;  // block light 0..1
layout(location=5) in float aWave;   // 0 static, 1 leaf sway, 2 water ripple
uniform mat4 uViewProj;
uniform float uTime;                 // seconds, for atmospheric motion
out vec2 vUV;
out float vShade;
out float vSky;
out float vBlock;
out vec3 vWorld;
void main() {
  vUV = aUV;
  vShade = aShade;
  vSky = aSky;
  vBlock = aBlock;
  vec3 pos = aPos;
  if (aWave > 0.5 && aWave < 1.5) {          // leaves: gentle multi-axis sway
    float ph = uTime * 1.6 + aPos.x * 0.7 + aPos.z * 0.7 + aPos.y * 0.3;
    pos.x += sin(ph) * 0.045;
    pos.z += cos(ph * 0.9 + 1.3) * 0.045;
    pos.y += sin(ph * 1.3) * 0.022;
  } else if (aWave > 1.5) {                  // water surface: low noisy ripple
    pos.y += (sin(uTime * 0.9 + aPos.x * 0.7) + sin(uTime * 1.27 + aPos.z * 0.6)) * 0.03 - 0.045;
  }
  vWorld = pos;
  gl_Position = uViewProj * vec4(pos, 1.0);
}`;

export const TERRAIN_FS = `#version 300 es
precision highp float;
in vec2 vUV;
in float vShade;
in float vSky;
in float vBlock;
in vec3 vWorld;
uniform sampler2D uAtlas;
uniform float uDaylight;   // 0..1 scales skylight (day/night)
uniform vec3 uFogColor;
uniform float uFogNear;
uniform float uFogFar;
uniform vec3 uCamPos;
out vec4 frag;
void main() {
  vec4 tex = texture(uAtlas, vUV);
  if (tex.a < 0.5) discard;                 // cutout edges (torch/glass/leaves)
  float light = max(vBlock, vSky * uDaylight);
  float b = 0.07 + 0.93 * light;            // ambient floor so nothing is pure black
  vec3 col = tex.rgb * vShade * b;
  float dist = distance(vWorld, uCamPos);
  float fog = clamp((dist - uFogNear) / (uFogFar - uFogNear), 0.0, 1.0);
  col = mix(col, uFogColor, fog);
  frag = vec4(col, tex.a);
}`;

// Entities reuse the terrain vertex layout but add a per-instance model matrix
// and a single location-sampled brightness (uBright) instead of baked light, so
// the same small mesh can be drawn anywhere. uTextured=0 draws a flat colour
// (uTint) for non-block item drops; =1 samples the atlas.
export const ENTITY_VS = `#version 300 es
precision highp float;
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in float aShade;
layout(location=3) in float aSky;
layout(location=4) in float aBlock;
uniform mat4 uViewProj;
uniform mat4 uModel;
out vec2 vUV;
out float vShade;
out vec3 vWorld;
out vec3 vCol;        // per-vertex tint for untextured mobs (packed in aUV + aBlock)
void main() {
  vUV = aUV;
  vShade = aShade;
  vCol = vec3(aUV, aBlock);
  vec4 wp = uModel * vec4(aPos, 1.0);
  vWorld = wp.xyz;
  gl_Position = uViewProj * wp;
}`;

export const ENTITY_FS = `#version 300 es
precision highp float;
in vec2 vUV;
in float vShade;
in vec3 vWorld;
in vec3 vCol;
uniform sampler2D uAtlas;
uniform float uTextured;   // 1 = sample atlas, 0 = flat uTint
uniform vec3 uTint;
uniform float uBright;     // 0..1 final light at the entity's cell
uniform vec3 uFogColor;
uniform float uFogNear;
uniform float uFogFar;
uniform vec3 uCamPos;
out vec4 frag;
void main() {
  vec3 base;
  if (uTextured > 0.5) {
    vec4 tex = texture(uAtlas, vUV);
    if (tex.a < 0.5) discard;
    base = tex.rgb;
  } else {
    base = uTint * vCol;   // uTint is the whole-entity tint (e.g. red hurt flash); vCol is the per-box colour
  }
  vec3 col = base * vShade * (0.1 + 0.9 * uBright);
  float dist = distance(vWorld, uCamPos);
  float fog = clamp((dist - uFogNear) / (uFogFar - uFogNear), 0.0, 1.0);
  col = mix(col, uFogColor, fog);
  frag = vec4(col, 1.0);
}`;

// The sky is a fullscreen triangle. The VS reconstructs a world-space view ray
// per fragment from the camera basis, so the gradient follows the true horizon
// and the sun/moon/stars sit at real sky directions regardless of where you look.
export const SKY_VS = `#version 300 es
precision highp float;
layout(location=0) in vec2 aPos;
uniform vec3 uRight;
uniform vec3 uUp;
uniform vec3 uFwd;
uniform float uTanHalf;
uniform float uAspect;
out vec3 vRay;
void main() {
  vRay = uFwd + aPos.x * uTanHalf * uAspect * uRight + aPos.y * uTanHalf * uUp;
  gl_Position = vec4(aPos, 1.0, 1.0);  // z=1 -> far plane, behind everything
}`;

export const SKY_FS = `#version 300 es
precision highp float;
in vec3 vRay;
uniform vec3 uHorizon;
uniform vec3 uZenith;
uniform vec3 uSunDir;       // world-space direction to the sun
uniform float uDayFactor;   // 0 night .. 1 day
uniform float uTime;        // seconds, for star twinkle
out vec4 frag;

float hash13(vec3 p) {
  p = fract(p * 0.1031);
  p += dot(p, p.yzx + 33.33);
  return fract((p.x + p.y) * p.z);
}

// Sparse round stars at random sub-cell positions, with a gentle twinkle.
float starField(vec3 dir) {
  vec3 d = dir * 90.0;
  vec3 c = floor(d), f = fract(d);
  float h = hash13(c);
  if (h < 0.975) return 0.0;
  vec3 pp = vec3(hash13(c + 1.7), hash13(c + 4.3), hash13(c + 8.1));
  float dist = length(f - pp);
  float tw = 0.6 + 0.4 * sin(uTime * 2.5 + h * 60.0);
  return smoothstep(0.10, 0.0, dist) * tw;
}

void main() {
  vec3 dir = normalize(vRay);
  float h = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
  vec3 col = mix(uHorizon, uZenith, pow(h, 0.8));

  // stars at night, fading in above the horizon
  float night = clamp(1.0 - uDayFactor * 1.6, 0.0, 1.0);
  if (night > 0.01 && dir.y > 0.0) {
    col += vec3(starField(dir) * night * smoothstep(0.0, 0.15, dir.y));
  }

  // sun: warm disc + soft glow on its side of the sky
  float sd = dot(dir, uSunDir);
  float sunDisc = smoothstep(0.9975, 0.9990, sd);
  float sunGlow = smoothstep(0.95, 1.0, sd) * 0.35 * uDayFactor;
  col += vec3(1.0, 0.93, 0.74) * (sunDisc + sunGlow);

  // moon: pale disc opposite the sun, with a faint mare mottle, only at night
  float md = dot(dir, -uSunDir);
  float moonDisc = smoothstep(0.9980, 0.9992, md);
  float mottle = 0.75 + 0.25 * hash13(floor(dir * 240.0));
  col += vec3(0.85, 0.88, 0.95) * moonDisc * mottle * (0.4 + 0.6 * night);

  frag = vec4(col, 1.0);
}`;

// The first-person held item. It lives in its own little near-field world (see
// render/viewmodel.js), so the pose and the close-up projection arrive already
// multiplied together as uMVP and there is no fog or world position to speak of.
// uLight is the light level where the player is standing, so the hand dims in a
// cave or at night — with a generous floor, since you should always be able to
// see what you are holding.
export const VIEWMODEL_VS = `#version 300 es
precision highp float;
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aUV;
layout(location=2) in float aShade;
uniform mat4 uMVP;
out vec2 vUV;
out float vShade;
void main() {
  vUV = aUV;
  vShade = aShade;
  gl_Position = uMVP * vec4(aPos, 1.0);
}`;

export const VIEWMODEL_FS = `#version 300 es
precision highp float;
in vec2 vUV;
in float vShade;
uniform sampler2D uAtlas;
uniform float uLight;      // 0..1 light where the player stands
out vec4 frag;
void main() {
  vec4 tex = texture(uAtlas, vUV);
  if (tex.a < 0.5) discard;                  // sprite cutouts
  frag = vec4(tex.rgb * vShade * (0.22 + 0.78 * uLight), 1.0);
}`;

export const LINE_VS = `#version 300 es
precision highp float;
layout(location=0) in vec3 aPos;
uniform mat4 uViewProj;
void main() { gl_Position = uViewProj * vec4(aPos, 1.0); }`;

export const LINE_FS = `#version 300 es
precision highp float;
uniform vec4 uColor;
out vec4 frag;
void main() { frag = uColor; }`;
