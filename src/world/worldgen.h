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
// v3 raises the sea to y=100 in a 192-tall world, which leaves a hundred blocks of
// underground instead of forty-six. The surface is deliberately UNCHANGED: every
// height is measured from the sea, so the same seed grows the same coastline and
// the same mountains, just translated up. What that buys is somewhere to put the
// rare ore where nothing open to the sky can reach it.
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

// Sea level is per-version, and it is the datum every other height is measured
// from — which is what lets v3 be "the same world, deeper" rather than a new one.
inline constexpr int kSeaLevelLegacy = 46;  // gen v1, v2
inline constexpr int kSeaLevelDeep = 100;   // gen v3
inline constexpr int seaLevel(int ver) { return ver >= 3 ? kSeaLevelDeep : kSeaLevelLegacy; }

// The height clamp is versioned too. It never actually bound in v1/v2 — those
// worlds top out near y=84 against a limit of 114 — but pinning it keeps the
// guarantee that raising WH cannot move a single block of an existing world.
inline constexpr int topClamp(int ver) { return (ver >= 3 ? WH : 128) - 14; }

// The deepest a ravine floor can land, and the ceiling for ore that must never be
// visible from the sky. Ravines are the only feature that opens the deeps to open
// air, so this is the whole of the "rare ore stays buried" guarantee, and
// --selftest asserts the gap rather than trusting the arithmetic here.
inline constexpr int ravineFloorMin(int ver) { return seaLevel(ver) - 38; }
inline constexpr int deepOreCeiling(int ver) { return seaLevel(ver) - 45; }

// Underground water, and why v4 exists.
//
// v2 flooded every carved cell at or below `seaLevel - 34`. With sea level at 46
// that was y<=12 — a ten-layer sump sitting on the bedrock, which is where a sump
// belongs. v3 moved sea level to 100 and the water line came with it, to y=66.
// That is sixty-five layers, very nearly the whole of the mineable underground,
// and all four of the ores worth going to find (aetherite, gloamite, sparkstone,
// sunbrass) top out at or below y=55 — inside it, every one of them. Mining the
// only tier that rewards mining meant mining underwater.
//
// v4 removes the line. Underground water becomes lakes instead: a low-frequency
// field decides where a body of water sits, and how far that field rises above its
// threshold decides how high the water fills, so a lake thins away to nothing at
// its edges rather than stopping at a wall of water. A cell is wet only when it is
// below its own column's level, so water is always filled from the floor upward
// and never left hanging in mid-air.
inline constexpr int kPocketCeilingDrop = 30;   // no pocket reaches above sea - this
inline constexpr double kPocketThreshold = 0.10;  // below this the column is dry
inline constexpr double kPocketSpan = 0.22;       // rise over which a lake fills up
inline constexpr int pocketCeiling(int ver) { return seaLevel(ver) - kPocketCeilingDrop; }

// v5 adds dungeons, and adds nothing else. Every terrain branch below is untouched,
// so a chunk generated at v4 comes out identical to what v4 always produced — which
// is the property that lets an existing world be moved forward without its landscape
// shifting under what the player built on it. The golden vectors hash every version
// from 1 up, so that claim is checked rather than asserted.
inline constexpr int kGenVersion = 5;

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
  // ---- v4 fields ----
  const Noise aquifer;  // where underground lakes sit, and how full they are

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

// True when a ravine has carved this column open from the surface down.
//
// Exposed because ravines are cut *after* the heightmap, out of ground heightAt
// has already reported as solid — so a column over a canyon still reports its
// uncarved surface, and anything choosing a place to stand on height alone will
// happily pick the lip of a thirty-block drop. Cheap to ask in bulk: the
// low-frequency band that decides where a ravine runs is tested first, and the
// column height only worked out for the few that fall inside it.
bool ravineAt(const NoiseSet& n, int wx, int wz, int ver = kGenVersion);

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

// A container a dungeon put down, in world coordinates.
struct DungeonChest {
  int x = 0, y = 0, z = 0;
  // True for the chest in the room holding the altar, which draws from a richer
  // table. The one thing that makes walking to the far room worth doing.
  bool altarRoom = false;
};

// Every dungeon chest standing inside one chunk.
//
// Re-derived rather than remembered. `generate` writes voxels and nothing else — it
// runs on a worker and only the ChunkData comes back — so there is nowhere to put a
// list of chest positions even if we wanted one. But placement is a pure function of
// the seed and the coordinate, so asking again is exact and costs a handful of
// hashes. World calls this as a chunk lands and fills what it finds.
void dungeonChestsIn(int cx, int cz, const NoiseSet& n, std::uint32_t seed, int ver,
                     std::vector<DungeonChest>& out);

// The altar of the nearest dungeon to a point, searching a square of lattice cells
// around it. For `--find-dungeon`: a structure that can only be found by luck is one
// that cannot be checked, screenshotted, or shown to anybody.
struct DungeonSite {
  int x = 0, y = 0, z = 0;
  int rooms = 0;
};
bool findDungeon(const NoiseSet& n, std::uint32_t seed, int ver, int nearX, int nearZ,
                 int searchCells, DungeonSite& out);

}  // namespace hr::world
