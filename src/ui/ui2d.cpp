#include "ui/ui2d.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"

namespace hr::ui {
namespace {

// A quad's local coordinates run from -half to +half so the SDF in ui.frag is
// centred, which is what makes one rounded-rect expression cover every corner.
struct Local {
  float hw, hh;
};

}  // namespace

Rect Rect::intersect(const Rect& o) const {
  const float x0 = std::max(x, o.x);
  const float y0 = std::max(y, o.y);
  const float x1 = std::min(right(), o.right());
  const float y1 = std::min(bottom(), o.bottom());
  return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

bool Ui2D::init(ShaderCache& shaders) {
  program_ = shaders.load({
      .name = "ui2d",
      .vertAsset = "shaders/ui.vert",
      .fragAsset = "shaders/ui.frag",
      .defines = {},
      .attribs = {"aPos", "aLocal", "aShape", "aUV", "aColA", "aColB", "aMode"},
  });
  if (!program_) return false;

  glGenVertexArrays(1, &vao_);
  glGenBuffers(1, &vbo_);
  glBindVertexArray(vao_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);

  const GLsizei stride = sizeof(UiVertex);
  auto attrib = [&](int index, int size, GLenum type, GLboolean normalized, std::size_t offset) {
    glEnableVertexAttribArray(index);
    glVertexAttribPointer(index, size, type, normalized, stride,
                          reinterpret_cast<const void*>(offset));
  };
  attrib(0, 2, GL_FLOAT, GL_FALSE, offsetof(UiVertex, x));
  attrib(1, 2, GL_FLOAT, GL_FALSE, offsetof(UiVertex, lx));
  attrib(2, 4, GL_FLOAT, GL_FALSE, offsetof(UiVertex, hw));
  attrib(3, 2, GL_FLOAT, GL_FALSE, offsetof(UiVertex, u));
  attrib(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(UiVertex, ar));
  attrib(5, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(UiVertex, br));
  attrib(6, 2, GL_FLOAT, GL_FALSE, offsetof(UiVertex, mode));
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  // A 1x1 opaque white texture stands in whenever a mode that samples uTex runs
  // without a real texture bound, which keeps an undefined sampler from turning
  // one bad call into a black screen.
  const std::uint8_t white[4] = {255, 255, 255, 255};
  glGenTextures(1, &whiteTexture_);
  glBindTexture(GL_TEXTURE_2D, whiteTexture_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);

  verts_.reserve(4096 * 6);
  return true;
}

void Ui2D::destroy() {
  if (vbo_) glDeleteBuffers(1, &vbo_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
  if (whiteTexture_) glDeleteTextures(1, &whiteTexture_);
  vbo_ = vao_ = whiteTexture_ = 0;
  vboCapacity_ = 0;
  verts_.clear();
}

void Ui2D::begin(int pixelWidth, int pixelHeight, float scale) {
  pixelW_ = pixelWidth;
  pixelH_ = pixelHeight;
  scale_ = scale > 0.0f ? scale : 1.0f;
  layoutW_ = static_cast<float>(pixelWidth) / scale_;
  layoutH_ = static_cast<float>(pixelHeight) / scale_;
  verts_.clear();
  clipStack_.clear();
  clipStack_.push_back({0, 0, layoutW_, layoutH_});
  texture_ = 0;
  fontTexture_ = 0;
  boundTexture_ = 0;
  boundFont_ = 0;
  appliedClip_ = clipStack_.back();
  drawCalls_ = 0;
  quadCount_ = 0;
  inFrame_ = true;

  // The interface draws last, over a finished frame, and never reads or writes
  // depth. Blending is straight source-over: the whole chain is LDR RGBA8.
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, pixelW_, pixelH_);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  blend_ = BlendMode::Normal;
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_SCISSOR_TEST);
  applyClip();
}

void Ui2D::end() {
  setBlendMode(BlendMode::Normal);
  flush();
  glDisable(GL_SCISSOR_TEST);
  glScissor(0, 0, pixelW_, pixelH_);
  glDisable(GL_BLEND);
  inFrame_ = false;
}

void Ui2D::applyClip() {
  const Rect& c = clipStack_.back();
  // glScissor's origin is bottom-left; the interface works top-left.
  const int x = static_cast<int>(std::floor(c.x * scale_));
  const int y = static_cast<int>(std::floor(c.y * scale_));
  const int w = static_cast<int>(std::ceil(c.right() * scale_)) - x;
  const int h = static_cast<int>(std::ceil(c.bottom() * scale_)) - y;
  glScissor(x, pixelH_ - (y + h), std::max(0, w), std::max(0, h));
  appliedClip_ = c;
}

void Ui2D::pushClip(const Rect& r) {
  flush();
  clipStack_.push_back(clipStack_.back().intersect(r));
  applyClip();
}

void Ui2D::popClip() {
  if (clipStack_.size() <= 1) return;
  flush();
  clipStack_.pop_back();
  applyClip();
}

void Ui2D::setBlendMode(BlendMode mode) {
  if (mode == blend_) return;
  flush();
  blend_ = mode;
  if (mode == BlendMode::Difference) {
    // See Hud::drawCrosshair for the derivation: for a white source premultiplied by
    // its alpha, difference blending is (1 - dst) * src + (1 - srcAlpha) * dst.
    glBlendFuncSeparate(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                        GL_ONE_MINUS_SRC_ALPHA);
  } else {
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  }
}

void Ui2D::setTexture(GLuint tex) {
  if (tex == texture_) return;
  flush();
  texture_ = tex;
}

void Ui2D::setFontTexture(GLuint tex) {
  if (tex == fontTexture_) return;
  flush();
  fontTexture_ = tex;
}

void Ui2D::pushQuad(const Rect& d, UiMode mode, Rgba a, Rgba b, float hw, float hh,
                    float radius, float param, float u0, float v0, float u1, float v1,
                    float gradT0, float gradT1, const Vec2* maskCenter) {
  if (d.w <= 0.0f || d.h <= 0.0f) return;
  ++quadCount_;

  UiVertex v;
  v.hw = hw;
  v.hh = hh;
  v.radius = radius;
  v.param = param;
  v.ar = a.r;
  v.ag = a.g;
  v.ab = a.b;
  v.aa = a.a;
  v.br = b.r;
  v.bg = b.g;
  v.bb = b.b;
  v.ba = b.a;
  v.mode = static_cast<float>(static_cast<int>(mode));

  const float cx = maskCenter ? maskCenter->x : d.centerX();
  const float cy = maskCenter ? maskCenter->y : d.centerY();
  const float corners[4][2] = {{d.x, d.y}, {d.right(), d.y}, {d.right(), d.bottom()},
                               {d.x, d.bottom()}};
  const float uvs[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
  const float grads[4] = {gradT0, gradT0, gradT1, gradT1};

  UiVertex q[4];
  for (int i = 0; i < 4; ++i) {
    q[i] = v;
    q[i].x = corners[i][0];
    q[i].y = corners[i][1];
    q[i].lx = corners[i][0] - cx;
    q[i].ly = corners[i][1] - cy;
    q[i].u = uvs[i][0];
    q[i].v = uvs[i][1];
    q[i].gradT = grads[i];
  }
  const int order[6] = {0, 1, 2, 0, 2, 3};
  for (int i = 0; i < 6; ++i) verts_.push_back(q[order[i]]);
}

void Ui2D::fillRect(const Rect& r, Rgba color, float radius) {
  if (color.a == 0) return;
  const Rect d = toDevice(r);
  pushQuad(d, UiMode::Fill, color, color, d.w * 0.5f, d.h * 0.5f, radius * scale_, 0, 0, 0, 0,
           0, 0, 0);
}

void Ui2D::fillGradient(const Rect& r, Rgba top, Rgba bottom, float radius) {
  if (top.a == 0 && bottom.a == 0) return;
  const Rect d = toDevice(r);
  pushQuad(d, UiMode::Fill, top, bottom, d.w * 0.5f, d.h * 0.5f, radius * scale_, 0, 0, 0, 0, 0,
           0, 1);
}

void Ui2D::strokeRect(const Rect& r, Rgba color, float width, float radius) {
  if (color.a == 0 || width <= 0.0f) return;
  const Rect d = toDevice(r);
  pushQuad(d, UiMode::Ring, color, color, d.w * 0.5f, d.h * 0.5f, radius * scale_,
           width * scale_, 0, 0, 0, 0, 0, 0);
}

void Ui2D::fillOutsideRounded(const Rect& r, Rgba color, float radius) {
  if (color.a == 0 || radius <= 0.0f) return;
  const Rect d = toDevice(r);
  pushQuad(d, UiMode::OutsideRounded, color, color, d.w * 0.5f, d.h * 0.5f, radius * scale_, 0,
           0, 0, 0, 0, 0, 0);
}

void Ui2D::shadow(const Rect& r, const BoxShadow& s, float radius) {
  if (s.color.a == 0) return;
  // The blurred rect has to be drawn larger than the box so the falloff is not
  // clipped: box-shadow spreads roughly the blur radius past the edge.
  const float pad = s.blur + s.spread + 1.0f;
  Rect box = {r.x + s.dx - s.spread, r.y + s.dy - s.spread, r.w + s.spread * 2,
              r.h + s.spread * 2};
  Rect grown = {box.x - pad, box.y - pad, box.w + pad * 2, box.h + pad * 2};
  const Rect d = toDevice(grown);
  const float hw = box.w * 0.5f * scale_;
  const float hh = box.h * 0.5f * scale_;
  pushQuad(d, UiMode::Shadow, s.color, s.color, hw, hh, radius * scale_, s.blur * scale_, 0, 0,
           0, 0, 0, 0);
}

void Ui2D::radialGradient(const Rect& r, float cx, float cy, float rx, float ry, float stop0,
                          float stop1, Rgba from, Rgba to) {
  const Rect d = toDevice(r);
  // Local coordinates are measured from the quad's centre, so shift the ellipse
  // centre in by adjusting the half extents the vertex writer sees. Easier: emit
  // the quad with its own centre at (cx, cy) by growing it symmetrically.
  const float dcx = cx * scale_;
  const float dcy = cy * scale_;
  const float half = std::max(std::max(dcx - d.x, d.right() - dcx),
                              std::max(dcy - d.y, d.bottom() - dcy));
  Rect sym = {dcx - half, dcy - half, half * 2, half * 2};
  // Then clip to the requested rect so the symmetric quad cannot paint outside it.
  pushClip(r);
  pushQuad(sym, UiMode::Radial, from, to, rx * scale_, ry * scale_, stop0, stop1, 0, 0, 0, 0, 0,
           0);
  popClip();
}

void Ui2D::texturedRect(const Rect& r, float u0, float v0, float u1, float v1, Rgba tint) {
  const Rect d = toDevice(r);
  pushQuad(d, UiMode::Texture, tint, tint, d.w * 0.5f, d.h * 0.5f, 0, 0, u0, v0, u1, v1, 0, 0);
}

void Ui2D::texturedRectMasked(const Rect& r, float u0, float v0, float u1, float v1,
                              const Rect& mask, float maskRadius, Rgba tint) {
  const Rect d = toDevice(r);
  const Rect m = toDevice(mask);
  const Vec2 center {m.centerX(), m.centerY()};
  pushQuad(d, UiMode::MaskedTexture, tint, tint, m.w * 0.5f, m.h * 0.5f, maskRadius * scale_, 0,
           u0, v0, u1, v1, 0, 0, &center);
}

NineSlice computeNineSlice(const Rect& r, float u0, float v0, float u1, float v1, int texWidth,
                           int texHeight, float slice) {
  NineSlice out;
  // The corner size in destination pixels. Scaled down when the destination is too
  // small for two corners to fit side by side — without this, a 12px-tall row drawn
  // with an 8px slice would draw its top and bottom corners overlapping, and the
  // middle band would have negative height and come out inside out.
  const float fit = std::min(1.0f, std::min(r.w, r.h) / (slice * 2.0f));
  const float d = slice * fit;
  out.corner = d;

  // The same inset in UV space, and measured against the SPRITE's pixel size rather
  // than the destination's. That is the whole point of a nine-slice: the corners
  // keep their authored proportions at every size the box is drawn at, and taking
  // this fraction from the destination instead would stretch them again.
  const float du = (u1 - u0) * (slice / static_cast<float>(texWidth));
  const float dv = (v1 - v0) * (slice / static_cast<float>(texHeight));

  out.xs[0] = r.x;         out.xs[1] = r.x + d;
  out.xs[2] = r.right() - d; out.xs[3] = r.right();
  out.ys[0] = r.y;         out.ys[1] = r.y + d;
  out.ys[2] = r.bottom() - d; out.ys[3] = r.bottom();
  out.us[0] = u0;          out.us[1] = u0 + du;
  out.us[2] = u1 - du;     out.us[3] = u1;
  out.vs[0] = v0;          out.vs[1] = v0 + dv;
  out.vs[2] = v1 - dv;     out.vs[3] = v1;
  return out;
}

void Ui2D::ninePatch(const Rect& r, float u0, float v0, float u1, float v1, int texWidth,
                     int texHeight, float slice, Rgba tint) {
  if (r.empty() || slice <= 0.0f || texWidth <= 0 || texHeight <= 0) {
    texturedRect(r, u0, v0, u1, v1, tint);
    return;
  }

  const NineSlice n = computeNineSlice(r, u0, v0, u1, v1, texWidth, texHeight, slice);
  for (int row = 0; row < 3; ++row) {
    for (int colIndex = 0; colIndex < 3; ++colIndex) {
      const Rect cell {n.xs[colIndex], n.ys[row], n.xs[colIndex + 1] - n.xs[colIndex],
                       n.ys[row + 1] - n.ys[row]};
      // A zero-width middle band is the ordinary case for a box exactly two corners
      // wide, not an error worth drawing an empty quad for.
      if (cell.w <= 0.0f || cell.h <= 0.0f) continue;
      texturedRect(cell, n.us[colIndex], n.vs[row], n.us[colIndex + 1], n.vs[row + 1], tint);
    }
  }
}

void Ui2D::glyphQuad(const Rect& r, float u0, float v0, float u1, float v1, Rgba colorA,
                     Rgba colorB, float gradT0, float gradT1) {
  // Glyph rects arrive already in device pixels: the text layer positions them on
  // whole device pixels so the rasterised coverage is not resampled.
  pushQuad(r, UiMode::Glyph, colorA, colorB, r.w * 0.5f, r.h * 0.5f, 0, 0, u0, v0, u1, v1,
           gradT0, gradT1);
}

void Ui2D::fillPoly(const Vec2* points, int count, Rgba color) {
  if (count < 3 || color.a == 0) return;
  UiVertex v;
  v.mode = static_cast<float>(static_cast<int>(UiMode::Flat));
  v.ar = color.r;
  v.ag = color.g;
  v.ab = color.b;
  v.aa = color.a;
  for (int i = 1; i + 1 < count; ++i) {
    const int idx[3] = {0, i, i + 1};
    for (int k = 0; k < 3; ++k) {
      UiVertex out = v;
      out.x = points[idx[k]].x * scale_;
      out.y = points[idx[k]].y * scale_;
      verts_.push_back(out);
    }
  }
  ++quadCount_;
}

void Ui2D::line(Vec2 a, Vec2 b, Rgba color, float width) {
  const float dx = b.x - a.x;
  const float dy = b.y - a.y;
  const float len = std::sqrt(dx * dx + dy * dy);
  if (len < 1e-4f || color.a == 0) return;
  const float nx = -dy / len * width * 0.5f;
  const float ny = dx / len * width * 0.5f;
  const Vec2 quad[4] = {{a.x + nx, a.y + ny}, {b.x + nx, b.y + ny},
                        {b.x - nx, b.y - ny}, {a.x - nx, a.y - ny}};
  fillPoly(quad, 4, color);
}

void Ui2D::strokePoly(const Vec2* points, int count, Rgba color, float width) {
  if (count < 2) return;
  for (int i = 0; i < count; ++i) line(points[i], points[(i + 1) % count], color, width);
}

void Ui2D::flush() {
  if (verts_.empty()) {
    return;
  }
  program_->use();
  program_->set("uViewport", static_cast<float>(pixelW_), static_cast<float>(pixelH_));
  program_->set("uTex", 0);
  program_->set("uFont", 1);
  program_->set("uPremultiply", blend_ == BlendMode::Difference ? 1.0f : 0.0f);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture_ ? texture_ : whiteTexture_);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, fontTexture_ ? fontTexture_ : whiteTexture_);
  glActiveTexture(GL_TEXTURE0);

  glBindVertexArray(vao_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  const std::size_t bytes = verts_.size() * sizeof(UiVertex);
  if (bytes > vboCapacity_) {
    vboCapacity_ = bytes + bytes / 2;
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vboCapacity_), nullptr,
                 GL_STREAM_DRAW);
  }
  glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), verts_.data());
  glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts_.size()));
  glBindVertexArray(0);

  ++drawCalls_;
  verts_.clear();
}

}  // namespace hr::ui
