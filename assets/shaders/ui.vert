#version 330 core

// The 2D interface layer. One vertex format covers every primitive the CSS
// needs; aMode selects which one the fragment shader evaluates.
//
// Positions are in screen pixels with the origin at the TOP-LEFT, matching CSS
// and Canvas2D so ported coordinates need no flipping. uViewport converts.

in vec2 aPos;     // screen px, top-left origin
in vec2 aLocal;   // px offset from the primitive's centre (SDF modes)
in vec4 aShape;   // halfW, halfH, radius, param  (param = border px | blur px)
in vec2 aUV;
in vec4 aColA;
in vec4 aColB;
in vec2 aMode;    // .x = primitive mode, .y = gradient position 0..1

out vec2 vLocal;
out vec4 vShape;
out vec2 vUV;
out vec4 vColA;
out vec4 vColB;
flat out int vMode;
out float vGradT;

uniform vec2 uViewport;

void main() {
  vLocal = aLocal;
  vShape = aShape;
  vUV = aUV;
  vColA = aColA;
  vColB = aColB;
  vMode = int(aMode.x + 0.5);
  vGradT = aMode.y;

  vec2 ndc = vec2(aPos.x / uViewport.x * 2.0 - 1.0,
                  1.0 - aPos.y / uViewport.y * 2.0);
  gl_Position = vec4(ndc, 0.0, 1.0);
}
