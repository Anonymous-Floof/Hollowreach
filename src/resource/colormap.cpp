#include "resource/colormap.h"

#include <algorithm>

namespace hr::resource {

Rgb8 tintFor(std::int8_t tintIndex, world::Biome biome) {
  (void)biome;
  if (tintIndex == kNoTint) return kWhite;
  // Every built-in texture is painted pre-coloured, so there is nothing to
  // multiply in. A resource-pack loader would return the pack's colormap sample
  // here, and the mesher and shaders already carry the result through.
  return kWhite;
}

void ClimateTile::build(const world::NoiseSet& noise, int cx, int cz, int ver) {
  const int baseX = cx * 16 - kBorder;
  const int baseZ = cz * 16 - kBorder;
  for (int z = 0; z < kSize; ++z) {
    for (int x = 0; x < kSize; ++x) {
      biomes_[z * kSize + x] = world::biomeAt(noise, baseX + x, baseZ + z, ver);
    }
  }
  built_ = true;
}

world::Biome ClimateTile::at(int lx, int lz) const {
  if (!built_) return world::Biome::Meadow;
  const int x = std::clamp(lx + kBorder, 0, kSize - 1);
  const int z = std::clamp(lz + kBorder, 0, kSize - 1);
  return biomes_[z * kSize + x];
}

}  // namespace hr::resource
