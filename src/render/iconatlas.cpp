#include "render/iconatlas.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "world/shapes.h"

namespace hr::render {
namespace {

int nextPowerOfTwo(int v) {
  int p = 1;
  while (p < v) p <<= 1;
  return p;
}

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

// One affine face draw, the direct equivalent of Canvas2D's
//   ctx.setTransform(a, b, c, d, e, f);
//   ctx.drawImage(tile, 0, 0, 16, 16);
//   ctx.fillStyle = `rgba(4,4,14,${dark})`; ctx.fillRect(0, 0, 16, 16);
//
// The transform maps tile space (u, v) in [0,16] to canvas space. Rather than
// forward-scatter, each destination pixel is mapped back through the inverse and
// sampled with nearest — which is what `imageSmoothingEnabled = false` did.
struct Affine {
  float a, b, c, d, e, f;
};

// Pixels to move the isometric projection right and down so its 24px box sits in the
// middle of the 32px cell. See the note on drawBlockIcon.
constexpr int kIsoShift = 2;

void drawFace(Image& into, int ox, int oy, const Image& source, const resource::TileRef& tile,
              const Affine& m, float dark) {
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
  // are mostly silhouette at 32px. Interior pixels come out at full coverage and
  // sample exactly one texel, so this changes nothing where it should not.
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
      const float cu = std::clamp(i00 * cdx + i01 * cdy, 0.0f, 15.999f);
      const float cv = std::clamp(i10 * cdx + i11 * cdy, 0.0f, 15.999f);
      // Tiles may be larger than 16 when a resource pack raises the resolution, so
      // sample proportionally rather than assuming one texel per model unit.
      const int tx = tile.x + std::min(tile.w - 1, static_cast<int>(cu * tile.w / 16.0f));
      const int ty = tile.y + std::min(tile.h - 1, static_cast<int>(cv * tile.h / 16.0f));
      Rgba texel = source.get(tx, ty);
      texel.a = static_cast<std::uint8_t>(std::lround(texel.a * coverage));
      blendRounded(into, ox + px, oy + py, texel);
      // The overlay covers the whole face quad, including wherever the tile was
      // transparent — which is what the fillRect did.
      if (dark > 0) {
        blendRounded(into, ox + px, oy + py,
                     Rgba {4, 4, 14,
                           static_cast<std::uint8_t>(std::lround(darkAlpha * coverage))});
      }
    }
  }
}

}  // namespace

IconAtlas::~IconAtlas() { destroy(); }

bool IconAtlas::build(const resource::Atlas& atlas) {
  const auto& all = game::items().all();
  count_ = static_cast<int>(all.size());
  if (count_ == 0) return false;

  const int wanted = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count_))));
  columns_ = nextPowerOfTwo(wanted * kIconSize) / kIconSize;
  const int rows = (count_ + columns_ - 1) / columns_;
  image_.resize(columns_ * kIconSize, nextPowerOfTwo(rows * kIconSize));
  image_.clear();

  for (const game::ItemDef& item : all) {
    const int ox = (item.index % columns_) * kIconSize;
    const int oy = (item.index / columns_) * kIconSize;
    drawIcon(image_, ox, oy, item, atlas);
  }

  log::info("icon atlas: %d items in %dx%d", count_, image_.width(), image_.height());
  return true;
}

void IconAtlas::drawIcon(Image& into, int ox, int oy, const game::ItemDef& item,
                         const resource::Atlas& atlas) {
  if (item.icon == game::IconKind::Block) {
    drawBlockIcon(into, ox, oy, item, atlas);
    return;
  }
  // Non-block items: the outlined sprite grid at exactly 2x.
  const game::SpriteGrid grid = game::outlined(game::spriteGridFor(item));
  for (int y = 0; y < game::kSpriteSize; ++y) {
    for (int x = 0; x < game::kSpriteSize; ++x) {
      const Rgba c = grid[static_cast<std::size_t>(y) * game::kSpriteSize + x];
      if (c.a == 0) continue;
      into.fillRect(ox + x * 2, oy + y * 2, 2, 2, c);
    }
  }
}

// Projection of block-local (x, y, z) in [0,1]^3 onto a 32px cell, with the cube
// spanning (2,2)-(26,26):
//   sx = 14 + 12(x - z)
//   sy = 26 - 12y - 6(x + z)
// Visible faces per box: top (y = y1), left (x = x0), right (z = z0).
//
// That box is not centred in the cell — 2px of margin at the top left against 6px
// at the bottom right — and the browser has the same bias, since these constants are
// its. It was easy to miss at the CSS slot size and is not at any interface scale
// above 100%, so kIsoShift moves the whole projection over by the two pixels rather
// than reproducing the lean. Applied to the cell origin, so it shifts every box by
// the same amount: centring each shape's own bounds instead would float a slab into
// the middle of its slot and break the one thing these icons exist to show, which is
// how the shapes differ from a full cube.
void IconAtlas::drawBlockIcon(Image& into, int ox, int oy, const game::ItemDef& item,
                              const resource::Atlas& atlas) {
  const world::BlockDef& block = world::blocks().def(item.blockId);
  const Image& src = atlas.image();

  // Sprite blocks (torches, plants) draw as a flat 2D tile, not a cube.
  if (block.render == world::RenderKind::Cross) {
    const resource::TileRef& t = atlas.tile(block.faceTextures[0]);
    // drawImage(src, tx, ty, 16, 16, 6, 3, 20, 26) — a plain axis-aligned scale.
    // Sampled from the destination pixel's CENTRE, which is what nearest-neighbour
    // means and what the browser did; sampling from the corner shifts the whole
    // sprite by half a source texel and shows up as a visibly different tile.
    for (int py = 0; py < 26; ++py) {
      for (int px = 0; px < 20; ++px) {
        const int sx = t.x + std::min(t.w - 1, (px * 2 + 1) * t.w / 40);
        const int sy = t.y + std::min(t.h - 1, (py * 2 + 1) * t.h / 52);
        blendRounded(into, ox + 6 + px, oy + 3 + py, src.get(sx, sy));
      }
    }
    return;
  }

  // The cross path above wants none of this: its drawImage(..., 6, 3, 20, 26) already
  // sits centred, so only the cube projection is moved.
  ox += kIsoShift;
  oy += kIsoShift;

  std::vector<world::Box> boxes = world::displayBoxes(block.render);
  // Painter's order: lower boxes first, then nearer ones. Near is low x+z, so the
  // secondary key is descending.
  std::stable_sort(boxes.begin(), boxes.end(), [](const world::Box& a, const world::Box& b) {
    if (a.y0 != b.y0) return a.y0 < b.y0;
    return (b.x0 + b.z0) < (a.x0 + a.z0);
  });

  const resource::TileRef& leftT = atlas.tile(block.faceTextures[4]);   // x0 face art
  const resource::TileRef& rightT = atlas.tile(block.faceTextures[0]);  // z0 face art
  const resource::TileRef& topT = atlas.tile(block.faceTextures[2]);

  for (const world::Box& box : boxes) {
    const float x0 = box.x0, y0 = box.y0, z0 = box.z0;
    const float x1 = box.x1, y1 = box.y1, z1 = box.z1;
    const float ex = 14 + 12 * (x0 - z0);
    const float dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;

    // left face (x = x0): tile u runs z1 -> z0, v runs y1 -> y0
    drawFace(into, ox, oy, src, leftT,
             {0.75f * dz, 0.375f * dz, 0, 0.75f * dy, 14 + 12 * (x0 - z1),
              26 - 12 * y1 - 6 * (x0 + z1)},
             0.20f);
    // right face (z = z0): tile u runs x0 -> x1, v runs y1 -> y0
    drawFace(into, ox, oy, src, rightT,
             {0.75f * dx, -0.375f * dx, 0, 0.75f * dy, ex, 26 - 12 * y1 - 6 * (x0 + z0)},
             0.38f);
    // top face (y = y1): tile u runs x0 -> x1, v runs z0 -> z1
    drawFace(into, ox, oy, src, topT,
             {0.75f * dx, -0.375f * dx, -0.75f * dz, -0.375f * dz, ex,
              26 - 12 * y1 - 6 * (x0 + z0)},
             0.0f);
  }
}

bool IconAtlas::uvFor(int itemIndex, float& u0, float& v0, float& u1, float& v1) const {
  if (itemIndex < 0 || itemIndex >= count_ || columns_ <= 0 || image_.empty()) return false;
  const float w = static_cast<float>(image_.width());
  const float h = static_cast<float>(image_.height());
  const float x = static_cast<float>((itemIndex % columns_) * kIconSize);
  const float y = static_cast<float>((itemIndex / columns_) * kIconSize);
  u0 = x / w;
  v0 = y / h;
  u1 = (x + kIconSize) / w;
  v1 = (y + kIconSize) / h;
  return true;
}

bool IconAtlas::uvFor(const std::string& key, float& u0, float& v0, float& u1,
                      float& v1) const {
  return uvFor(game::items().indexOf(key), u0, v0, u1, v1);
}

void IconAtlas::upload() {
  if (image_.empty()) return;
  if (texture_ == 0) glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image_.width(), image_.height(), 0, GL_RGBA,
               GL_UNSIGNED_BYTE, image_.data());
  // Icons are drawn at their native size or an integer multiple of it, and the
  // whole look depends on hard pixel edges.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
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
