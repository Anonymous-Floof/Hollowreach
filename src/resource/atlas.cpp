#include "resource/atlas.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "core/log.h"
#include "resource/painters.h"
#include "world/blocks.h"

namespace hr::resource {
namespace {

// The magenta/black checker an unresolvable id gets, so a typo is loud.
Image missingTile(int res) {
  Image img(res, res);
  const int half = res / 2;
  for (int y = 0; y < res; ++y) {
    for (int x = 0; x < res; ++x) {
      const bool m = (x < half) == (y < half);
      img.set(x, y, m ? Rgba {230, 20, 230, 255} : Rgba {20, 20, 20, 255});
    }
  }
  return img;
}

int nextPowerOfTwo(int v) {
  int p = 1;
  while (p < v) p <<= 1;
  return p;
}

}  // namespace

Atlas::~Atlas() { destroy(); }

std::vector<ResourceId> collectBlockTextureIds() {
  // A set so the same texture referenced by several blocks is stored once, and
  // ordered so the atlas layout is stable between runs — which matters for
  // diffing debug PNGs.
  std::set<ResourceId> unique;
  for (const world::BlockDef& def : world::blocks().all()) {
    for (const auto& [slot, ref] : def.textures.entries()) {
      (void)slot;
      // A #variable is resolved by the model layer, not here; concrete ids only.
      if (!ref.isVariable() && !ref.id().empty()) unique.insert(ref.id());
    }
  }
  return std::vector<ResourceId>(unique.begin(), unique.end());
}

bool Atlas::build(const PackStack& stack, const std::vector<ResourceId>& wanted,
                  const AtlasSettings& settings) {
  settings_ = settings;

  // --- 1. pick the tile resolution -------------------------------------------
  // The largest any provider offers, rounded up to a power of two and capped.
  // With only procedural painters in the stack this lands on 16, reproducing the
  // original exactly.
  int nativeMax = kPainterTile;
  for (const ResourceId& id : wanted) {
    if (auto info = stack.info(id)) {
      nativeMax = std::max(nativeMax, std::max(info->width, info->height / std::max(1, info->frames)));
    }
  }
  tileRes_ = std::min(nextPowerOfTwo(nativeMax), std::max(16, settings.maxTileResolution));

  // --- 2. mip levels and gutter ---------------------------------------------
  // A tile must survive being halved mipLevels_ - 1 times without a neighbour
  // bleeding in, so the gutter is one half-tile-chain wide: 1 << (levels - 1).
  mipLevels_ = 1;
  if (settings.mipmaps) {
    // Stop before the tile would shrink below 4x4, where a voxel face's texture
    // is a solid colour anyway and further levels only cause cross-tile bleed.
    int res = tileRes_;
    while (res > 4 && mipLevels_ < 5) {
      res /= 2;
      ++mipLevels_;
    }
  }
  const int gutter = settings.mipmaps ? (1 << (mipLevels_ - 1)) : 1;
  const int cell = tileRes_ + gutter * 2;

  // --- 3. layout -------------------------------------------------------------
  // A square-ish grid, as the original used, but sized to a power of two so the
  // mip chain divides cleanly.
  const int count = static_cast<int>(wanted.size()) + 1;  // +1 for the missing tile
  const int cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
  const int rows = (count + cols - 1) / cols;
  const int width = nextPowerOfTwo(cols * cell);
  const int height = nextPowerOfTwo(rows * cell);
  image_.resize(width, height);

  log::info("atlas: %d tiles at %dpx (gutter %d, %d mip level%s) -> %dx%d", count, tileRes_,
            gutter, mipLevels_, mipLevels_ == 1 ? "" : "s", width, height);

  // --- 4. paint --------------------------------------------------------------
  tiles_.clear();
  tiles_.reserve(wanted.size() * 2);

  auto place = [&](int index, const Image& source, int frames, double frameTime) -> TileRef {
    const int col = index % cols;
    const int row = index / cols;
    const int ox = col * cell + gutter;
    const int oy = row * cell + gutter;

    Image scaled = (source.width() == tileRes_ && source.height() == tileRes_)
                       ? source
                       : source.scaledNearest(tileRes_, tileRes_);
    image_.blitRegion(scaled, 0, 0, tileRes_, tileRes_, ox, oy, /*skipTransparent=*/false);

    // Gutter: clamp-extend the tile's edge pixels outward. Without this a mip
    // level or a grazing-angle sample picks up the neighbouring tile.
    for (int g = 1; g <= gutter; ++g) {
      for (int i = 0; i < tileRes_; ++i) {
        image_.set(ox + i, oy - g, scaled.get(i, 0));
        image_.set(ox + i, oy + tileRes_ - 1 + g, scaled.get(i, tileRes_ - 1));
        image_.set(ox - g, oy + i, scaled.get(0, i));
        image_.set(ox + tileRes_ - 1 + g, oy + i, scaled.get(tileRes_ - 1, i));
      }
    }
    // ...and the four corner squares.
    for (int gy = 1; gy <= gutter; ++gy) {
      for (int gx = 1; gx <= gutter; ++gx) {
        image_.set(ox - gx, oy - gy, scaled.get(0, 0));
        image_.set(ox + tileRes_ - 1 + gx, oy - gy, scaled.get(tileRes_ - 1, 0));
        image_.set(ox - gx, oy + tileRes_ - 1 + gy, scaled.get(0, tileRes_ - 1));
        image_.set(ox + tileRes_ - 1 + gx, oy + tileRes_ - 1 + gy,
                   scaled.get(tileRes_ - 1, tileRes_ - 1));
      }
    }

    // Half-texel inset, as in the original: with NEAREST sampling an exact tile
    // edge can otherwise pick up the adjacent texel at grazing angles.
    const float e = 0.5f;
    TileRef ref;
    ref.u0 = (ox + e) / width;
    ref.v0 = (oy + e) / height;
    ref.u1 = (ox + tileRes_ - e) / width;
    ref.v1 = (oy + tileRes_ - e) / height;
    ref.x = ox;
    ref.y = oy;
    ref.w = tileRes_;
    ref.h = tileRes_;
    ref.frames = frames;
    ref.frameTimeSeconds = static_cast<float>(frameTime);
    return ref;
  };

  int index = 0;
  int missingCount = 0;
  for (const ResourceId& id : wanted) {
    Image source = stack.load(id);
    int frames = 1;
    double frameTime = 0.0;
    if (auto info = stack.info(id)) {
      frames = std::max(1, info->frames);
      frameTime = info->frameTimeSeconds;
    }
    if (source.empty()) {
      log::warn("atlas: nothing provides %s", id.str().c_str());
      source = missingTile(kPainterTile);
      frames = 1;
      ++missingCount;
    } else if (frames > 1) {
      // Animated sources arrive as a vertical strip. Only frame 0 goes into the
      // atlas; a future animator will glTexSubImage2D the others in over time,
      // which is why the CPU pixels are kept.
      source = source.subImage(0, 0, source.width(), source.height() / frames);
    }
    tiles_.emplace(id, place(index++, source, frames, frameTime));
  }
  missing_ = place(index++, missingTile(kPainterTile), 1, 0.0);

  if (missingCount > 0) {
    log::warn("atlas: %d texture(s) had no provider and show as magenta", missingCount);
  }

  // --- 5. mip chain ----------------------------------------------------------
  // Generated by halving the whole atlas image. That is safe *because* of the
  // gutter: at level n a tile is (tileRes >> n) with (gutter >> n) of bleed still
  // around it, so no level ever averages two different tiles together.
  mips_.clear();
  if (mipLevels_ > 1) {
    Image prev = image_;
    for (int level = 1; level < mipLevels_; ++level) {
      prev = prev.downsampleHalfAlphaWeighted();
      mips_.push_back(prev);
    }
  }
  return true;
}

void Atlas::upload() {
  if (image_.empty()) return;
  if (texture_ == 0) glGenTextures(1, &texture_);

  glBindTexture(GL_TEXTURE_2D, texture_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image_.width(), image_.height(), 0, GL_RGBA,
               GL_UNSIGNED_BYTE, image_.data());
  for (std::size_t i = 0; i < mips_.size(); ++i) {
    const Image& m = mips_[i];
    glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(i + 1), GL_RGBA8, m.width(), m.height(), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, m.data());
  }
  applyFilters();
}

void Atlas::applyFilters() const {
  const bool mips = mipLevels_ > 1 && !mips_.empty();
  // Magnification stays NEAREST regardless: this is pixel art, and the whole look
  // depends on crisp texels up close.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  mips ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mips ? mipLevels_ - 1 : 0);
  if (mips && settings_.anisotropy > 1) {
    // Core since GL 4.6 and an extension before it; harmless to attempt, since a
    // driver without it ignores the unknown parameter.
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY,
                    static_cast<GLfloat>(settings_.anisotropy));
  }
}

void Atlas::destroy() {
  if (texture_) {
    glDeleteTextures(1, &texture_);
    texture_ = 0;
  }
}

const TileRef& Atlas::tile(const ResourceId& id) const {
  auto it = tiles_.find(id);
  return it == tiles_.end() ? missing_ : it->second;
}

bool Atlas::hasTile(const ResourceId& id) const { return tiles_.count(id) != 0; }

bool Atlas::writeDebugPng(const std::string& path) const { return image_.writePng(path); }

}  // namespace hr::resource
