// Deterministic world generation, ported from js/world/worldgen.js.
//
// Given (chunk, seed, version) it fills the voxels identically every time, so a
// save only stores the seed plus the blocks the player changed. There is no
// sequential RNG anywhere in the terrain path — only Perlin fields and stateless
// coordinate hashes — which is what makes a chunk generated on any thread, in any
// order, bit-identical.
//
// GENERATION IS VERSIONED. Saves record the version they were made with and every
// entry point takes it, so a world's terrain never shifts under existing builds.
// v1 is the original meadow-only generator; v2 adds climate biomes, ridged
// mountains, ravines, flooded deep caverns, gravel/dirt pockets and shore papyrus.
//
// Determinism here is not cosmetic: multiplayer guests generate terrain locally
// from the host's seed, so two machines must agree. See noise.cpp for the
// floating-point pragmas that back that up.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "world/chunk.h"
#include "world/noise.h"

namespace hr::world {

inline constexpr int kSeaLevel = 46;
inline constexpr int kGenVersion = 2;

enum class Biome : std::uint8_t { Meadow = 0, Forest = 1, Birch = 2, Desert = 3, Snow = 4 };
inline constexpr int kBiomeCount = 5;

const char* biomeName(Biome biome);

// The twelve Perlin fields a seed implies. Built once and shared read-only across
// worker threads — the JS cached a single seed's worth in a module-level slot,
// which cannot work once generation is threaded.
class NoiseSet {
 public:
  explicit NoiseSet(std::uint32_t seed);

  std::uint32_t seed() const { return seed_; }

  const Noise terrain, hills, cave, cave2, ore, stonevar, flora;
  // ---- v2 fields ----
  const Noise temp;    // climate temperature
  const Noise moist;   // climate moisture
  const Noise mount;   // where mountain ranges live (mask)
  const Noise ridge;   // ridged peaks inside the mask
  const Noise ravine;  // thin surface canyons

 private:
  std::uint32_t seed_;
};

// Everything a column needs from the 2D fields, computed once.
struct ColumnInfo {
  int h = 0;
  Biome biome = Biome::Meadow;
  double temperature = 0.0;
};

ColumnInfo columnInfo(const NoiseSet& n, int wx, int wz, int ver);

int heightAt(const NoiseSet& n, int wx, int wz, int ver = kGenVersion);
Biome biomeAt(const NoiseSet& n, int wx, int wz, int ver = kGenVersion);

// Fills chunk.data->voxels. Metadata, lighting and entities are not produced here,
// exactly as in the JS — which is why this function is safe to run off-thread.
void generate(Chunk& chunk, const NoiseSet& n, int ver = kGenVersion);

// Map support: predict a column's surface without generating the chunk. Cheap
// (2D fields plus a handful of hashes) and close enough that the Atlas can sketch
// unexplored terrain.
struct SurfacePreview {
  std::string key;  // block key, for colouring
  int h = 0;
};
SurfacePreview surfacePreview(const NoiseSet& n, int wx, int wz, int ver = kGenVersion);

}  // namespace hr::world
