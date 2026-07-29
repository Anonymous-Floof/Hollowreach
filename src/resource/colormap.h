// Biome colour tinting.
//
// The web build had no tinting at all: grass green is baked into the painter
// (js/render/texatlas.js:75), snow is a separate texture rather than a tinted one,
// and wool has a single white variant. That works because the textures are
// procedural — but Minecraft resource packs ship *greyscale* grass and foliage and
// expect the game to multiply a biome colour in. Without a tint channel, such a
// pack renders grey grass, and adding one later means changing the vertex format
// and every terrain shader.
//
// So the channel exists now and resolves to white. `kNoTint` on a block means the
// mesher skips the lookup entirely, which is every block today.

#pragma once

#include <cstdint>

#include "world/worldgen.h"

namespace hr::resource {

// Tint slots, matching Minecraft's `tintindex` values in spirit.
inline constexpr std::int8_t kNoTint = -1;
inline constexpr std::int8_t kTintGrass = 0;
inline constexpr std::int8_t kTintFoliage = 1;
inline constexpr std::int8_t kTintWater = 2;

struct Rgb8 {
  std::uint8_t r = 255, g = 255, b = 255;
};

inline constexpr Rgb8 kWhite {255, 255, 255};

// The colour for `tintIndex` in `biome`. White for every slot today.
Rgb8 tintFor(std::int8_t tintIndex, world::Biome biome);

// Per-chunk climate tile.
//
// Vanilla averages the colormap over a radius — the "Biome Blend" setting — rather
// than sampling per block, and biomeAt costs two fbm2 evaluations per column. So
// when tinting does arrive, the mesher should sample this precomputed tile rather
// than call into worldgen per vertex. Sized to the chunk plus a one-cell border,
// matching the mesher's neighbourhood reach.
class ClimateTile {
 public:
  static constexpr int kBorder = 1;
  static constexpr int kSize = 16 + kBorder * 2;

  // Fills from the world generator for the chunk at (cx, cz).
  void build(const world::NoiseSet& noise, int cx, int cz, int ver);

  // Local chunk coordinates, which may be -1 or 16.
  world::Biome at(int lx, int lz) const;

 private:
  world::Biome biomes_[kSize * kSize] {};
  bool built_ = false;
};

}  // namespace hr::resource
