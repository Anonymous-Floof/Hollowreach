// CPU-side RGBA8 images.
//
// This is the workhorse of the texture path. The web build painted every block
// texture with Canvas2D 1x1 fillRect calls (js/render/texatlas.js) and then read
// pixels back off the canvas to extrude item models (js/render/itemmesh.js:86).
// Both of those become operations on an Image here, which is also why the atlas
// keeps its pixels in memory rather than uploading and forgetting them.
//
// Origin is top-left, matching Canvas2D and PNG, so the ported painters index
// pixels with the same coordinates as the JS.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hr {

struct Rgba {
  std::uint8_t r = 0, g = 0, b = 0, a = 0;

  static Rgba fromHex(std::uint32_t rgb, std::uint8_t alpha = 255) {
    return {static_cast<std::uint8_t>((rgb >> 16) & 0xFF),
            static_cast<std::uint8_t>((rgb >> 8) & 0xFF),
            static_cast<std::uint8_t>(rgb & 0xFF), alpha};
  }
};

class Image {
 public:
  Image() = default;
  Image(int width, int height, Rgba fill = {}) { resize(width, height, fill); }

  void resize(int width, int height, Rgba fill = {});
  void clear(Rgba fill = {});

  int width() const { return width_; }
  int height() const { return height_; }
  bool empty() const { return width_ <= 0 || height_ <= 0; }

  std::uint8_t* data() { return pixels_.data(); }
  const std::uint8_t* data() const { return pixels_.data(); }
  std::size_t byteSize() const { return pixels_.size(); }

  bool inBounds(int x, int y) const { return x >= 0 && y >= 0 && x < width_ && y < height_; }

  Rgba get(int x, int y) const {
    if (!inBounds(x, y)) return {};
    const std::uint8_t* p = &pixels_[(static_cast<std::size_t>(y) * width_ + x) * 4];
    return {p[0], p[1], p[2], p[3]};
  }

  // Writes the pixel outright. The painters draw onto a transparent canvas and
  // never rely on blending, so replace is the correct primitive — and it is what
  // makes a straight port of `ctx.fillRect(x, y, 1, 1)` exact even for the two
  // painters that use a fractional alpha.
  void set(int x, int y, Rgba c) {
    if (!inBounds(x, y)) return;
    std::uint8_t* p = &pixels_[(static_cast<std::size_t>(y) * width_ + x) * 4];
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = c.a;
  }

  // Source-over composite, for the few places that genuinely layer.
  void blend(int x, int y, Rgba c);

  void fillRect(int x, int y, int w, int h, Rgba c);

  // Copies `src` in at (dx, dy). Pixels with alpha 0 are skipped when
  // `skipTransparent`, which is how item sprite grids are blitted.
  void blit(const Image& src, int dx, int dy, bool skipTransparent = true);
  void blitRegion(const Image& src, int sx, int sy, int sw, int sh, int dx, int dy,
                  bool skipTransparent = true);

  // Nearest-neighbour scale. Used to bring 16px procedural art up to the atlas
  // tile resolution when a higher-resolution resource pack sets the size.
  Image scaledNearest(int newWidth, int newHeight) const;

  // Box-filtered half-size step with alpha-weighted colour, for mip generation.
  // Averaging straight RGBA darkens the fringes of cutout textures, because
  // fully transparent pixels drag their (meaningless) colour into the result.
  Image downsampleHalfAlphaWeighted() const;

  Image subImage(int x, int y, int w, int h) const;

  bool writePng(const std::string& path) const;
  static bool loadPng(const std::string& path, Image& out, std::string* errorOut = nullptr);
  static bool loadPngFromMemory(const std::uint8_t* bytes, std::size_t size, Image& out,
                                std::string* errorOut = nullptr);

 private:
  int width_ = 0, height_ = 0;
  std::vector<std::uint8_t> pixels_;
};

}  // namespace hr
