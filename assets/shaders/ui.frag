#version 330 core

// Fragment side of the 2D interface. Six primitives, chosen by vMode:
//
//   0  filled rounded rect, linear-gradient(180deg, A, B)
//   1  rounded-rect ring — the CSS border, painted over the background
//   2  textured quad, tinted by A (icons, map tiles, the pre-rendered title)
//   3  glyph: R8 coverage from the font atlas, colour mix(A, B, gradT)
//   4  soft shadow: the same rounded rect blurred, for box-shadow/drop-shadow
//   5  flat vertex-coloured triangle, for the map's markers
//   6  radial-gradient(ellipse at ..., A stop0, B stop1)
//   7  fill OUTSIDE a rounded rect — the inverse mask, for painting over corner spill
//   8  textured quad masked by an EXTERNAL rounded rect: `overflow: hidden` plus a
//      border-radius, which is how the minimap's square tiles and the gallery's
//      thumbnails get rounded corners. A scissor box cannot express it, and covering the
//      corners afterwards is wrong because the radius should reveal what is *behind*.
//
// Everything is in pixel units, so antialiasing is an exact one-pixel ramp on the
// signed distance and needs no fwidth().

in vec2 vLocal;
in vec4 vShape;
in vec2 vUV;
in vec4 vColA;
in vec4 vColB;
flat in int vMode;
in float vGradT;

out vec4 frag;

uniform sampler2D uTex;   // RGBA: icons, map tiles, title
uniform sampler2D uFont;  // R8: glyph coverage
// 1 while the difference blend is active. That blend multiplies the source colour by
// (1 - dst), so a source that is not premultiplied by its own alpha paints its full
// colour even where coverage is zero — which turned the crosshair into a solid box.
uniform float uPremultiply;

// Signed distance to a rounded rectangle centred on the origin. Negative inside.
float sdRoundRect(vec2 p, vec2 halfSize, float r) {
  r = min(r, min(halfSize.x, halfSize.y));
  vec2 q = abs(p) - (halfSize - vec2(r));
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// CSS interpolates gradients with premultiplied alpha, so a fully transparent
// stop contributes no colour. Straight mixing would tint the ramp toward whatever
// RGB happened to sit behind that zero alpha.
vec4 mixPremul(vec4 a, vec4 b, float t) {
  vec4 pa = vec4(a.rgb * a.a, a.a);
  vec4 pb = vec4(b.rgb * b.a, b.a);
  vec4 p = mix(pa, pb, t);
  return p.a > 0.0001 ? vec4(p.rgb / p.a, p.a) : vec4(0.0);
}

// Every return path goes through this, so premultiplication cannot be forgotten for one
// primitive and not another.
void emit(vec4 c) {
  frag = uPremultiply > 0.5 ? vec4(c.rgb * c.a, c.a) : c;
}

void main() {
  if (vMode == 5) {
    emit(vColA);
    return;
  }

  if (vMode == 2) {
    emit(texture(uTex, vUV) * vColA);
    return;
  }

  if (vMode == 8) {
    // vLocal and vShape describe the MASK, not this quad, so a 16x16 tile in the corner
    // of a 168px rounded box gets exactly the coverage that box's corner allows.
    float mask = clamp(0.5 - sdRoundRect(vLocal, vShape.xy, vShape.z), 0.0, 1.0);
    if (mask <= 0.0) discard;
    vec4 tex = texture(uTex, vUV) * vColA;
    emit(vec4(tex.rgb, tex.a * mask));
    return;
  }

  if (vMode == 3) {
    float cov = texture(uFont, vUV).r;
    vec4 c = mixPremul(vColA, vColB, clamp(vGradT, 0.0, 1.0));
    emit(vec4(c.rgb, c.a * cov));
    return;
  }

  if (vMode == 6) {
    float d = length(vLocal / max(vShape.xy, vec2(1e-4)));
    float t = clamp((d - vShape.z) / max(1e-5, vShape.w - vShape.z), 0.0, 1.0);
    emit(mixPremul(vColA, vColB, t));
    return;
  }

  float d = sdRoundRect(vLocal, vShape.xy, vShape.z);

  if (vMode == 7) {
    float outside = clamp(0.5 + d, 0.0, 1.0);
    if (outside <= 0.0) discard;
    emit(vec4(vColA.rgb, vColA.a * outside));
    return;
  }

  if (vMode == 4) {
    // box-shadow's blur radius is twice the Gaussian sigma; a smoothstep across
    // the full radius is within a level or two of the real error function and
    // costs one instruction.
    float blur = max(vShape.w, 0.5);
    float a = 1.0 - smoothstep(-blur, blur, d);
    emit(vec4(vColA.rgb, vColA.a * a));
    return;
  }

  // Coverage of a one-pixel band centred on the edge.
  float cov = clamp(0.5 - d, 0.0, 1.0);
  if (cov <= 0.0) discard;

  if (vMode == 1) {
    // Offsetting a signed distance field inward by t is just d + t, so the inner
    // edge of the border is the same field shifted by the border width. CSS puts
    // the border inside the element box (box-sizing: border-box), which is why the
    // ring grows inward from the outer edge rather than straddling it.
    float inner = clamp(0.5 - (d + max(vShape.w, 0.0)), 0.0, 1.0);
    float a = cov - inner;
    if (a <= 0.0) discard;
    emit(vec4(vColA.rgb, vColA.a * a));
    return;
  }

  vec4 c = mixPremul(vColA, vColB, clamp(vGradT, 0.0, 1.0));
  emit(vec4(c.rgb, c.a * cov));
}
