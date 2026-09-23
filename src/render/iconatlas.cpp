#include "render/iconatlas.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "world/shapes.h"

// A texture parameter in the 3.3 core profile, but not one the loader's header
// happens to name.
#ifndef GL_TEXTURE_LOD_BIAS
#define GL_TEXTURE_LOD_BIAS 0x8501
#endif

namespace hr::render {
namespace {

int nextPowerOfTwo(int v) {
  int p = 1;
  while (p < v) p <<= 1;
  return p;
}

// Cells were 32px, and every constant below was written for that; K scales them.
constexpr int K = kIconSize / 32;

// Empty space around every cell. The sheet is minified with linear filtering and
// mipmaps, and both reach past a cell's edge: packed edge to edge, the top row of
// the icon below leaked into the bottom of the one above as a faint line under it.
// Four texels keep the first two mip levels inside their own cell.
constexpr int kIconPad = 4;
constexpr int kIconStride = kIconSize + 2 * kIconPad;

// Top-left of a cell's drawable area.
int cellX(int cell, int columns) { return (cell % columns) * kIconStride + kIconPad; }
int cellY(int cell, int columns) { return (cell / columns) * kIconStride + kIconPad; }

// The same rim the item sprites get (game::outlined), for art drawn here from block
// tiles: a plant or a door in the bag should read as the same kind of object as an
// ingot beside it, and the rim is most of what makes a sprite read as one.
constexpr Rgba kOutline {24, 18, 12, 209};

// Source-over in floating point, rounded once at the end.
//
// Image::blend does the same in integers with a truncating divide, which is right
// for the block painters — they only ever write onto empty pixels, where truncation
// and rounding agree — but an icon composites two or three layers over each other
// (the tile, then the per-side darkening, then a neighbouring sub-box's face), and
// there the truncation showed up as a systematic 1-2 level offset across whole
// faces against the browser's rounded result.
void blendRounded(Image& into, int x, int y, Rgba src) {
  if (src.a == 0 || !into.inBounds(x, y)) return;
  const Rgba dst = into.get(x, y);
  const double sa = src.a / 255.0;
  const double da = dst.a / 255.0;
  const double outA = sa + da * (1.0 - sa);
  if (outA <= 0.0) {
    into.set(x, y, Rgba {});
    return;
  }
  const auto channel = [&](std::uint8_t s, std::uint8_t d) {
    const double v = (s * sa + d * da * (1.0 - sa)) / outA;
    return static_cast<std::uint8_t>(std::lround(std::min(255.0, v)));
  };
  into.set(x, y,
           Rgba {channel(src.r, dst.r), channel(src.g, dst.g), channel(src.b, dst.b),
                 static_cast<std::uint8_t>(std::lround(outA * 255.0))});
}

// Pixel art at a whole-number scale, with the sprite rim around it. `pixels` is
// w x h, row-major.
void drawPixelArt(Image& into, int ox, int oy, const std::vector<Rgba>& pixels, int w, int h,
                  int scale, bool outline) {
  const auto filled = [&](int x, int y) {
    return x >= 0 && y >= 0 && x < w && y < h &&
           pixels[static_cast<std::size_t>(y) * w + x].a >= game::kSpriteAlphaCutoff;
  };
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      Rgba c = pixels[static_cast<std::size_t>(y) * w + x];
      if (c.a < game::kSpriteAlphaCutoff) {
        const bool touching =
            filled(x - 1, y) || filled(x + 1, y) || filled(x, y - 1) || filled(x, y + 1);
        if (!outline || !touching) continue;
        c = kOutline;
      }
      into.fillRect(ox + x * scale, oy + y * scale, scale, scale, c);
    }
  }
}

// A tile's pixels as a flat list, for drawPixelArt.
std::vector<Rgba> tilePixels(const Image& source, const resource::TileRef& t) {
  std::vector<Rgba> out;
  out.reserve(static_cast<std::size_t>(t.w) * t.h);
  for (int y = 0; y < t.h; ++y) {
    for (int x = 0; x < t.w; ++x) out.push_back(source.get(t.x + x, t.y + y));
  }
  return out;
}

// One affine face draw, the direct equivalent of Canvas2D's
//   ctx.setTransform(a, b, c, d, e, f);
//   ctx.drawImage(tile, 0, 0, 16, 16);
//   ctx.fillStyle = `rgba(4,4,14,${dark})`; ctx.fillRect(0, 0, 16, 16);
//
// The transform maps face space (u, v) in [0,16] to canvas space. Rather than
// forward-scatter, each destination pixel is mapped back through the inverse and
// sampled with nearest — which is what `imageSmoothingEnabled = false` did.
struct Affine {
  float a, b, c, d, e, f;
};

// Which part of the tile a face shows: world::faceUv's fractions at the face's two
// opposite corners, plus the bed's quarter-turns. The icon used to stretch the
// whole tile over every face, as the mesher did; now a slab shows the half of its
// tile a placed slab shows.
struct TileWindow {
  float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  int turns = 0;
};

// Pixels to move the isometric projection right and down so its box sits in the
// middle of the cell. See the note on drawBlockIcon.
constexpr int kIsoShift = 2 * K;

void drawFace(Image& into, int ox, int oy, const Image& source, const resource::TileRef& tile,
              const TileWindow& win, const Affine& m, float dark, Image* undyed, bool dyed) {
  const float det = m.a * m.d - m.b * m.c;
  if (std::fabs(det) < 1e-6f) return;  // a degenerate box has no visible face
  const float inv = 1.0f / det;
  const float i00 = m.d * inv, i01 = -m.c * inv;
  const float i10 = -m.b * inv, i11 = m.a * inv;

  // Destination bounding box of the transformed unit square.
  float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
  constexpr float kQuad[4][2] = {{0, 0}, {16, 0}, {16, 16}, {0, 16}};
  for (const auto& q : kQuad) {
    const float x = m.a * q[0] + m.c * q[1] + m.e;
    const float y = m.b * q[0] + m.d * q[1] + m.f;
    minX = std::min(minX, x);
    maxX = std::max(maxX, x);
    minY = std::min(minY, y);
    maxY = std::max(maxY, y);
  }
  const int x0 = std::max(0, static_cast<int>(std::floor(minX)));
  const int x1 = std::min(kIconSize - 1, static_cast<int>(std::ceil(maxX)));
  const int y0 = std::max(0, static_cast<int>(std::floor(minY)));
  const int y1 = std::min(kIconSize - 1, static_cast<int>(std::ceil(maxY)));

  const float darkAlpha = dark * 255.0f;

  // 4x4 coverage supersampling. The browser antialiases the edges of a transformed
  // drawImage even with smoothing disabled, so a hard-edged rasteriser would give
  // every icon a visibly crisper silhouette than the original — and these shapes
  // are mostly silhouette. Interior pixels come out at full coverage and sample
  // exactly one texel, so this changes nothing where it should not.
  constexpr int kSub = 4;
  constexpr float kStep = 1.0f / kSub;
  constexpr float kFirst = kStep * 0.5f;

  for (int py = y0; py <= y1; ++py) {
    for (int px = x0; px <= x1; ++px) {
      // Coverage comes from the sub-samples; the COLOUR is still sampled once, with
      // nearest filtering, from the pixel centre. Averaging colour across
      // sub-samples would blur texel boundaries the browser kept crisp, and on a
      // cutout tile it would drag the meaningless RGB of transparent texels into
      // the result — which is exactly how glass and leaves went wrong.
      int covered = 0;
      for (int sy = 0; sy < kSub; ++sy) {
        for (int sx = 0; sx < kSub; ++sx) {
          const float dx = px + kFirst + sx * kStep - m.e;
          const float dy = py + kFirst + sy * kStep - m.f;
          const float u = i00 * dx + i01 * dy;
          const float v = i10 * dx + i11 * dy;
          if (u >= 0 && u < 16 && v >= 0 && v < 16) ++covered;
        }
      }
      if (covered == 0) continue;
      const float coverage = static_cast<float>(covered) / (kSub * kSub);

      // The centre can fall just outside a partly covered pixel, so clamp into the
      // quad rather than dropping the sample.
      const float cdx = px + 0.5f - m.e;
      const float cdy = py + 0.5f - m.f;
      const float cu = std::clamp(i00 * cdx + i01 * cdy, 0.0f, 15.999f) / 16.0f;
      const float cv = std::clamp(i10 * cdx + i11 * cdy, 0.0f, 15.999f) / 16.0f;
      float fu = win.u0 + cu * (win.u1 - win.u0);
      float fv = win.v0 + cv * (win.v1 - win.v0);
      world::rotateUv(win.turns, fu, fv);
      // Tiles may be larger than 16 when a resource pack raises the resolution, so
      // sample proportionally rather than assuming one texel per model unit.
      const int tx = tile.x + std::clamp(static_cast<int>(fu * tile.w), 0, tile.w - 1);
      const int ty = tile.y + std::clamp(static_cast<int>(fv * tile.h), 0, tile.h - 1);
      Rgba texel = source.get(tx, ty);
      const bool solid = texel.a >= game::kSpriteAlphaCutoff && coverage >= 0.5f;
      texel.a = static_cast<std::uint8_t>(std::lround(texel.a * coverage));
      blendRounded(into, ox + px, oy + py, texel);
      // The overlay covers the whole face quad, including wherever the tile was
      // transparent — which is what the fillRect did.
      if (dark > 0) {
        blendRounded(into, ox + px, oy + py,
                     Rgba {4, 4, 14,
                           static_cast<std::uint8_t>(std::lround(darkAlpha * coverage))});
      }
      // Whoever paints a pixel solidly last owns it: the undyed mask follows the
      // visible surface, so a frame behind the mattress is not in it and a pillow in
      // front of the mattress is.
      if (undyed && solid) {
        undyed->set(ox + px, oy + py, dyed ? Rgba {} : Rgba {255, 255, 255, 255});
      }
    }
  }
}

}  // namespace

void bleedTransparent(Image& img, int passes) {
  for (int pass = 0; pass < passes; ++pass) {
    const Image src = img;
    for (int y = 0; y < img.height(); ++y) {
      for (int x = 0; x < img.width(); ++x) {
        if (src.get(x, y).a != 0) continue;
        int r = 0, g = 0, b = 0, n = 0;
        for (int dy = -1; dy <= 1; ++dy) {
          for (int dx = -1; dx <= 1; ++dx) {
            if ((dx == 0 && dy == 0) || !src.inBounds(x + dx, y + dy)) continue;
            const Rgba c = src.get(x + dx, y + dy);
            // Opaque neighbours only on the first pass; later passes may also borrow
            // from pixels an earlier pass filled, which carry alpha 0 but real RGB.
            const bool usable = c.a != 0 || (pass > 0 && (c.r | c.g | c.b) != 0);
            if (!usable) continue;
            r += c.r;
            g += c.g;
            b += c.b;
            ++n;
          }
        }
        if (n == 0) continue;
        img.set(x, y, Rgba {static_cast<std::uint8_t>(r / n), static_cast<std::uint8_t>(g / n),
                            static_cast<std::uint8_t>(b / n), 0});
      }
    }
  }
}

IconAtlas::~IconAtlas() { destroy(); }

bool IconAtlas::build(const resource::Atlas& atlas) {
  const auto& all = game::items().all();
  count_ = static_cast<int>(all.size());
  if (count_ == 0) return false;

  // Which items get an undyed overlay: dyeable blocks with a part the dye skips.
  overlayCell_.clear();
  int cells = count_;
  for (const game::ItemDef& item : all) {
    if (item.icon != game::IconKind::Block) continue;
    const world::BlockDef& b = world::blocks().def(item.blockId);
    if (!b.dyeable) continue;
    const bool partly = std::any_of(b.parts.begin(), b.parts.end(),
                                    [](const world::BlockDef::Part& p) { return !p.dyed; });
    if (partly) overlayCell_[item.index] = cells++;
  }

  const int wanted = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(cells))));
  columns_ = nextPowerOfTwo(wanted * kIconStride) / kIconStride;
  const int rows = (cells + columns_ - 1) / columns_;
  image_.resize(nextPowerOfTwo(columns_ * kIconStride), nextPowerOfTwo(rows * kIconStride));
  image_.clear();

  for (const game::ItemDef& item : all) {
    const int ox = cellX(item.index, columns_);
    const int oy = cellY(item.index, columns_);
    const auto overlay = overlayCell_.find(item.index);
    if (overlay == overlayCell_.end()) {
      drawIcon(image_, ox, oy, item, atlas, nullptr);
      continue;
    }
    // Draw once while recording which pixels an undyed part owns, then copy those
    // pixels — the finished colours, with every face's shading — into the overlay.
    Image mask(image_.width(), image_.height());
    mask.clear();
    drawIcon(image_, ox, oy, item, atlas, &mask);
    const int cx = cellX(overlay->second, columns_);
    const int cy = cellY(overlay->second, columns_);
    for (int y = 0; y < kIconSize; ++y) {
      for (int x = 0; x < kIconSize; ++x) {
        if (mask.get(ox + x, oy + y).a == 0) continue;
        image_.set(cx + x, cy + y, image_.get(ox + x, oy + y));
      }
    }
  }

  // Bleed as far as the padding, so everything a filter can reach from inside a cell
  // carries that cell's own colours.
  bleedTransparent(image_, kIconPad);

  log::info("icon atlas: %d items (+%zu overlays) in %dx%d", count_, overlayCell_.size(),
            image_.width(), image_.height());
  return true;
}

void IconAtlas::drawIcon(Image& into, int ox, int oy, const game::ItemDef& item,
                         const resource::Atlas& atlas, Image* undyed) {
  if (item.icon == game::IconKind::Block) {
    drawBlockIcon(into, ox, oy, item, atlas, undyed);
    return;
  }
  // Non-block items: the outlined sprite grid at 4x.
  const game::SpriteGrid grid = game::outlined(game::spriteGridFor(item));
  constexpr int kScale = kIconSize / game::kSpriteSize;
  for (int y = 0; y < game::kSpriteSize; ++y) {
    for (int x = 0; x < game::kSpriteSize; ++x) {
      const Rgba c = grid[static_cast<std::size_t>(y) * game::kSpriteSize + x];
      if (c.a == 0) continue;
      into.fillRect(ox + x * kScale, oy + y * kScale, kScale, kScale, c);
    }
  }
}

// Projection of block-local (x, y, z) in [0,1]^3 onto the cell, written at the
// original 32px and scaled by K:
//   sx = 14 + 12(x - z)
//   sy = 26 - 12y - 6(x + z)
// Visible faces per box: top (y = y1), left (x = x0), right (z = z0).
//
// That box is not centred in the cell — 2px of margin at the top left against 6px
// at the bottom right, at 32px — and the browser has the same bias, since these
// constants are its. kIsoShift moves the whole projection over rather than
// reproducing the lean. Applied to the cell origin, so it shifts every box by the
// same amount: centring each shape's own bounds instead would float a slab into the
// middle of its slot and break the one thing these icons exist to show, which is how
// the shapes differ from a full cube.
void IconAtlas::drawBlockIcon(Image& into, int ox, int oy, const game::ItemDef& item,
                              const resource::Atlas& atlas, Image* undyed) {
  const world::BlockDef& block = world::blocks().def(item.blockId);
  const Image& src = atlas.image();

  // Plants and torches: their tile, flat, at the sprites' 4x and with their rim. A
  // ladder and a painting too, for the reason itemModelFor gives: in projection
  // they were a thin slab seen nearly edge-on.
  if (block.render == world::RenderKind::Cross || block.render == world::RenderKind::Ladder ||
      block.render == world::RenderKind::Painting) {
    const resource::TileRef& t = atlas.tile(block.faceTextures[0]);
    if (t.w == game::kSpriteSize && t.h == game::kSpriteSize) {
      drawPixelArt(into, ox, oy, tilePixels(src, t), t.w, t.h, kIconSize / t.w, true);
    } else {
      // A pack's larger tile: fitted to the cell by nearest sampling, unrimmed.
      for (int py = 0; py < kIconSize; ++py) {
        for (int px = 0; px < kIconSize; ++px) {
          blendRounded(into, ox + px, oy + py,
                       src.get(t.x + px * t.w / kIconSize, t.y + py * t.h / kIconSize));
        }
      }
    }
    return;
  }

  // Doors: the whole door, upper half over lower, at 2x in the middle of the cell.
  if (block.render == world::RenderKind::Door && !block.upperTexture.empty()) {
    const resource::TileRef& top = atlas.tile(block.upperTexture);
    const resource::TileRef& bottom = atlas.tile(block.faceTextures[0]);
    if (top.w == game::kSpriteSize && top.h == game::kSpriteSize && bottom.w == top.w &&
        bottom.h == top.h) {
      std::vector<Rgba> door = tilePixels(src, top);
      const std::vector<Rgba> lower = tilePixels(src, bottom);
      door.insert(door.end(), lower.begin(), lower.end());
      constexpr int kScale = kIconSize / (2 * game::kSpriteSize);
      drawPixelArt(into, ox + (kIconSize - game::kSpriteSize * kScale) / 2, oy, door,
                   game::kSpriteSize, 2 * game::kSpriteSize, kScale, true);
      return;
    }
    // A pack whose door tiles are a different size falls through to the shape.
  }

  // The flat paths above want none of this: they already sit centred, so only the
  // cube projection is moved.
  ox += kIsoShift;
  oy += kIsoShift;

  std::vector<world::Box> boxes = world::displayBoxes(block.render);
  // Painter's order: lower boxes first, then nearer ones. Near is low x+z, so the
  // secondary key is descending.
  std::stable_sort(boxes.begin(), boxes.end(), [](const world::Box& a, const world::Box& b) {
    if (a.y0 != b.y0) return a.y0 < b.y0;
    return (b.x0 + b.z0) < (a.x0 + a.z0);
  });

  // The bed's display pose is head toward +x, whose top art the world turns this
  // many quarter-turns; the icon turns it the same way so they match.
  const int topTurns = block.render == world::RenderKind::Bed ? 1 : 0;

  constexpr float S = 12.0f * K;           // one block, in pixels, along an iso axis
  constexpr float H = S / 2.0f;            // ...its vertical rise per unit of x + z
  constexpr float X0 = 14.0f * K, Y0 = 26.0f * K;
  constexpr float A = S / 16.0f;           // one tile unit along an axis
  constexpr float B = H / 16.0f;

  for (const world::Box& box : boxes) {
    const float x0 = box.x0, y0 = box.y0, z0 = box.z0;
    const float x1 = box.x1, y1 = box.y1, z1 = box.z1;
    const float dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;

    // Which tiles, and whether a dye reaches them.
    const bool ownFaces = box.part == 0 || box.part > block.parts.size();
    const ResourceId* partTex =
        ownFaces ? nullptr : &block.parts[box.part - 1].texture;
    const bool dyed = ownFaces || block.parts[box.part - 1].dyed;
    const resource::TileRef& leftT = atlas.tile(partTex ? *partTex : block.faceTextures[4]);
    const resource::TileRef& rightT = atlas.tile(partTex ? *partTex : block.faceTextures[0]);
    const resource::TileRef& topT = atlas.tile(partTex ? *partTex : block.faceTextures[2]);

    // Each face's window of its tile, by world::faceUv's rules for the face it is:
    // left is the x = x0 face (1), right the z = z0 face (5), top the y = y1 face (2).
    const TileWindow leftW {1.0f - z1, 1.0f - y1, 1.0f - z0, 1.0f - y0, 0};
    const TileWindow rightW {x0, 1.0f - y1, x1, 1.0f - y0, 0};
    const TileWindow topW {x0, 1.0f - z1, x1, 1.0f - z0, topTurns};

    // left face (x = x0): face u runs z1 -> z0, v runs y1 -> y0
    drawFace(into, ox, oy, src, leftT, leftW,
             {A * dz, B * dz, 0, A * dy, X0 + S * (x0 - z1), Y0 - S * y1 - H * (x0 + z1)},
             0.20f, undyed, dyed);
    // right face (z = z0): face u runs x0 -> x1, v runs y1 -> y0
    drawFace(into, ox, oy, src, rightT, rightW,
             {A * dx, -B * dx, 0, A * dy, X0 + S * (x0 - z0), Y0 - S * y1 - H * (x0 + z0)},
             0.38f, undyed, dyed);
    // top face (y = y1): face u runs x0 -> x1, v runs z1 -> z0 — the tile's top edge
    // toward +z, the way the world lays every top face.
    drawFace(into, ox, oy, src, topT, topW,
             {A * dx, -B * dx, A * dz, B * dz, X0 + S * (x0 - z1),
              Y0 - S * y1 - H * (x0 + z1)},
             0.0f, undyed, dyed);
  }
}

bool IconAtlas::cellRect(int cell, float& u0, float& v0, float& u1, float& v1) const {
  if (cell < 0 || columns_ <= 0 || image_.empty()) return false;
  const float w = static_cast<float>(image_.width());
  const float h = static_cast<float>(image_.height());
  const float x = static_cast<float>(cellX(cell, columns_));
  const float y = static_cast<float>(cellY(cell, columns_));
  u0 = x / w;
  v0 = y / h;
  u1 = (x + kIconSize) / w;
  v1 = (y + kIconSize) / h;
  return true;
}

bool IconAtlas::uvFor(int itemIndex, float& u0, float& v0, float& u1, float& v1) const {
  if (itemIndex < 0 || itemIndex >= count_) return false;
  return cellRect(itemIndex, u0, v0, u1, v1);
}

bool IconAtlas::uvFor(const std::string& key, float& u0, float& v0, float& u1,
                      float& v1) const {
  return uvFor(game::items().indexOf(key), u0, v0, u1, v1);
}

bool IconAtlas::overlayFor(const std::string& key, float& u0, float& v0, float& u1,
                           float& v1) const {
  const auto it = overlayCell_.find(game::items().indexOf(key));
  if (it == overlayCell_.end()) return false;
  return cellRect(it->second, u0, v0, u1, v1);
}

void IconAtlas::upload() {
  if (image_.empty()) return;
  if (texture_ == 0) glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image_.width(), image_.height(), 0, GL_RGBA,
               GL_UNSIGNED_BYTE, image_.data());
  // Minified with mipmaps, magnified with NEAREST. A slot smaller than a cell gets
  // an even, filtered reduction instead of a nearest-neighbour one that keeps some
  // rows and drops others; a slot larger than a cell keeps hard pixel edges. The
  // bias pulls minification toward the sharper level, since what is being shrunk
  // is pixel art that is only a little too big.
  glGenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, -0.5f);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void IconAtlas::destroy() {
  if (texture_) {
    glDeleteTextures(1, &texture_);
    texture_ = 0;
  }
}

bool IconAtlas::writeDebugPng(const std::string& path) const { return image_.writePng(path); }

}  // namespace hr::render
