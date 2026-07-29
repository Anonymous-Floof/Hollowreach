// The 2D drawing layer: everything the interface is painted with.
//
// This replaces the browser's compositor. The web build declared its interface in
// HTML and let CSS rasterise rounded corners, gradients, borders and shadows; here
// each of those is a primitive that appends six vertices to one buffer, flushed in
// as few draw calls as the texture and clip changes allow.
//
// Coordinates are screen pixels with the origin at the top-left, so a ported
// `left: 14px; top: 8px` needs no conversion. All geometry is in *layout* pixels;
// Ui2D multiplies by the ui scale on the way in, which is the one improvement over
// the original (the stylesheet is entirely fixed-px, so the browser's zoom was its
// only scaler).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/gl.h"
#include "core/shader.h"
#include "resource/image.h"
#include "ui/theme.h"

namespace hr::ui {

struct Vec2 {
  float x = 0, y = 0;
};

// A CSS box. `x`/`y` are the top-left corner.
struct Rect {
  float x = 0, y = 0, w = 0, h = 0;

  float right() const { return x + w; }
  float bottom() const { return y + h; }
  float centerX() const { return x + w * 0.5f; }
  float centerY() const { return y + h * 0.5f; }
  bool contains(float px, float py) const {
    return px >= x && py >= y && px < x + w && py < y + h;
  }
  bool empty() const { return w <= 0.0f || h <= 0.0f; }
  Rect inset(float d) const { return {x + d, y + d, w - d * 2, h - d * 2}; }
  Rect offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
  Rect intersect(const Rect& o) const;
};

// One vertex of the interface. 56 bytes; a full screen of interface is a few
// thousand quads, so the cost of the wide format is irrelevant next to the cost of
// not being able to express a CSS box in one primitive.
struct UiVertex {
  float x = 0, y = 0;
  float lx = 0, ly = 0;
  float hw = 0, hh = 0, radius = 0, param = 0;
  float u = 0, v = 0;
  std::uint8_t ar = 255, ag = 255, ab = 255, aa = 255;
  std::uint8_t br = 255, bg = 255, bb = 255, ba = 255;
  float mode = 0;
  float gradT = 0;
};
static_assert(sizeof(UiVertex) == 56, "ui vertex must stay tightly packed");

// Mirrors the modes in assets/shaders/ui.frag.
enum class UiMode : int {
  Fill = 0,
  Ring = 1,
  Texture = 2,
  Glyph = 3,
  Shadow = 4,
  Flat = 5,
  Radial = 6,
  OutsideRounded = 7,
  MaskedTexture = 8,
};

// A CSS box-shadow. `inset` shadows are handled separately (only one element uses
// one, and it is a 1px highlight line).
struct BoxShadow {
  float dx = 0, dy = 0, blur = 0, spread = 0;
  Rgba color = kTransparent;
};

// Everything composites source-over except the crosshair, which the stylesheet gives
// `mix-blend-mode: difference` so it stays visible against any background.
enum class BlendMode { Normal, Difference };

class Ui2D {
 public:
  bool init(ShaderCache& shaders);
  void destroy();

  // Starts a frame. `scale` multiplies every layout coordinate.
  void begin(int pixelWidth, int pixelHeight, float scale);
  void end();

  float scale() const { return scale_; }
  // Viewport in layout pixels — what a screen lays itself out inside.
  float width() const { return layoutW_; }
  float height() const { return layoutH_; }

  // --- state -------------------------------------------------------------
  // Switching either texture flushes, so batch icon draws together.
  void setTexture(GLuint tex);
  void setFontTexture(GLuint tex);

  // Clip stack. Rects intersect, so a scroll region inside a card clips to both.
  void pushClip(const Rect& r);
  void popClip();
  const Rect& clip() const { return clipStack_.back(); }

  void setBlendMode(BlendMode mode);

  // --- primitives --------------------------------------------------------
  void fillRect(const Rect& r, Rgba color, float radius = 0.0f);
  void fillGradient(const Rect& r, Rgba top, Rgba bottom, float radius = 0.0f);
  void strokeRect(const Rect& r, Rgba color, float width, float radius = 0.0f);
  // Fills the area of `r` that falls OUTSIDE its own rounded corners. This is how a
  // border-radius clips square content — the scissor box is rectangular, so the corner
  // spill has to be painted over in the backdrop colour instead.
  void fillOutsideRounded(const Rect& r, Rgba color, float radius);
  void shadow(const Rect& r, const BoxShadow& s, float radius = 0.0f);

  // radial-gradient(<rx> <ry> at cx cy, from stop0, to stop1) where the stops are
  // fractions of the ellipse radius, matching the CSS percentages.
  void radialGradient(const Rect& r, float cx, float cy, float rx, float ry, float stop0,
                      float stop1, Rgba from, Rgba to);

  // Textured quad from the bound texture. `tint` multiplies; white leaves it alone.
  void texturedRect(const Rect& r, float u0, float v0, float u1, float v1,
                    Rgba tint = color::white);
  // The same, clipped to `mask`'s rounded corners — `overflow: hidden` on a box with a
  // border-radius. Anything the radius cuts away shows what was already behind, which is
  // what makes it different from painting over the corners.
  void texturedRectMasked(const Rect& r, float u0, float v0, float u1, float v1,
                          const Rect& mask, float maskRadius, Rgba tint = color::white);

  // Convex polygon fill and its outline, in layout pixels. Used by the Atlas for
  // waypoint diamonds and the heading arrow, which were Canvas2D paths.
  void fillPoly(const Vec2* points, int count, Rgba color);
  void strokePoly(const Vec2* points, int count, Rgba color, float width);
  void line(Vec2 a, Vec2 b, Rgba color, float width);

  // Emitted by ui::Text, which owns the glyph atlas.
  void glyphQuad(const Rect& r, float u0, float v0, float u1, float v1, Rgba colorA,
                 Rgba colorB, float gradT0, float gradT1);

  // --- stats -------------------------------------------------------------
  int drawCalls() const { return drawCalls_; }
  int quadCount() const { return quadCount_; }

 private:
  void flush();
  // `maskCenter`, when given, is what the per-vertex local coordinates are measured
  // from, so a quad can carry a signed-distance field that is not its own box.
  void pushQuad(const Rect& device, UiMode mode, Rgba a, Rgba b, float hw, float hh,
                float radius, float param, float u0, float v0, float u1, float v1,
                float gradT0, float gradT1, const Vec2* maskCenter = nullptr);
  void applyClip();
  Rect toDevice(const Rect& r) const {
    return {r.x * scale_, r.y * scale_, r.w * scale_, r.h * scale_};
  }

  Program* program_ = nullptr;
  GLuint vao_ = 0, vbo_ = 0;
  std::size_t vboCapacity_ = 0;
  std::vector<UiVertex> verts_;

  GLuint texture_ = 0;
  GLuint fontTexture_ = 0;
  GLuint boundTexture_ = 0;
  GLuint boundFont_ = 0;
  GLuint whiteTexture_ = 0;

  std::vector<Rect> clipStack_;  // layout pixels
  Rect appliedClip_ {};
  BlendMode blend_ = BlendMode::Normal;

  int pixelW_ = 0, pixelH_ = 0;
  float layoutW_ = 0, layoutH_ = 0;
  float scale_ = 1.0f;
  bool inFrame_ = false;
  int drawCalls_ = 0;
  int quadCount_ = 0;
};

}  // namespace hr::ui
