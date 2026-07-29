// Same floating-point discipline as noise.cpp: this file's arithmetic decides
// terrain, and a fused multiply-add or a reassociated sum changes it.
#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#pragma fp_contract(off)
#elif defined(__clang__) || defined(__GNUC__)
#pragma STDC FP_CONTRACT OFF
#endif

#include "world/worldgen.h"

#include <cmath>

#include "core/jsmath.h"
#include "core/prng.h"

namespace hr::world {
namespace {

using jsmath::floorDiv;
using jsmath::truncToInt;

// Deliberately not std::clamp/std::min: the JS used Math.min/Math.max, and this
// mirrors the expression it came from.
double smoothstep(double e0, double e1, double x) {
  double t = (x - e0) / (e1 - e0);
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;
  return t * t * (3.0 - 2.0 * t);
}

// Low frequency on purpose: a base wavelength of ~800 blocks makes a biome read
// as a region you travel to rather than confetti. Two octaves keep borders wobbly.
double climateT(const NoiseSet& n, int wx, int wz) {
  return n.temp.fbm2(wx * 0.0012, wz * 0.0012, 2);
}
double climateM(const NoiseSet& n, int wx, int wz) {
  return n.moist.fbm2(wx * 0.0014 + 37, wz * 0.0014 - 11, 2);
}

// Thresholds tuned to the measured field distribution: a 2-octave fbm here runs
// roughly +/-0.26 at the 10th/90th percentile, NOT +/-1. Every biome shows up —
// snow ~8%, desert ~7%, birch ~10%, forest ~15%, meadow the rest.
Biome biomeOf(double T, double M) {
  if (T < -0.30) return Biome::Snow;
  if (T > 0.26 && M < 0.10) return Biome::Desert;
  if (T < -0.13 && M > 0.02) return Biome::Birch;
  if (M > 0.16) return Biome::Forest;
  return Biome::Meadow;
}

// Ravines (v2): a thin band of a low-frequency field carves a canyon from the
// surface into the deeps. Returns 0 for none, or the canyon floor y.
constexpr double kRavineBand = 0.016;
int ravineFloor(const NoiseSet& n, int wx, int wz, int h) {
  if (h <= kSeaLevel + 2) return 0;  // never crack open the sea floor
  const double rv = n.ravine.fbm2(wx * 0.0045 + 7, wz * 0.0045 - 3, 2);
  if (std::abs(rv) >= kRavineBand) return 0;
  // Depth varies along the crack so the floor undulates.
  const double d = n.ravine.fbm2(wx * 0.02 - 40, wz * 0.02 + 25, 2);
  const int floorY = 14 + static_cast<int>(std::floor(d * 5));
  return floorY > 8 ? floorY : 8;
}

// Water line for flooded deep caverns (v2): carved space at or below this level
// fills with still water instead of air.
constexpr int kDeepWaterY = 12;

// Ore veins. Deepest and rarest first so they win ties. Higher `scale` means
// higher-frequency noise and so smaller, more broken-up veins; higher `threshold`
// means fewer cells qualify. Tuned so ore is a find, not a guarantee: measured
// densities run from embercoal ~1.2% down to aetherite ~0.15% of in-band cells.
struct OreSpec {
  const char* key;
  int yMin, yMax;
  double scale, threshold;
  int sx, sy, sz;
};
const OreSpec kOres[] = {
    {"ore_aetherite", 3, 16, 0.19, 0.73, 11, 0, 23},
    {"ore_gloamite", 3, 18, 0.19, 0.72, 55, 13, 88},
    {"ore_sparkstone", 3, 20, 0.19, 0.71, 40, 5, 70},
    {"ore_sunbrass", 4, 28, 0.18, 0.69, 80, 9, 14},
    {"ore_azurite", 4, 36, 0.18, 0.67, 3, 50, 31},
    {"ore_verdanite", 6, 44, 0.18, 0.67, 21, 60, 47},
    {"ore_ferralite", 5, 46, 0.18, 0.66, 60, 22, 90},
    {"ore_copper", 6, 60, 0.17, 0.62, 17, 33, 5},
    {"ore_embercoal", 8, 84, 0.17, 0.60, 99, 71, 41},
};

// Ore ids resolved once, in table order, so the inner loop never touches a string.
struct OreIds {
  BlockId id[std::size(kOres)] {};
  OreIds() {
    for (std::size_t i = 0; i < std::size(kOres); ++i) id[i] = blocks().idOf(kOres[i].key);
  }
};
const OreIds& oreIds() {
  static const OreIds ids;
  return ids;
}

BlockId oreAt(const NoiseSet& n, int wx, int wy, int wz) {
  const OreIds& ids = oreIds();
  for (std::size_t i = 0; i < std::size(kOres); ++i) {
    const OreSpec& o = kOres[i];
    if (wy < o.yMin || wy > o.yMax) continue;
    const double value = n.ore.noise3((wx + o.sx) * o.scale, (wy + o.sy) * o.scale,
                                      (wz + o.sz) * o.scale);
    if (value > o.threshold) return ids.id[i];
  }
  return wk().greystone;
}

// Deep stone: an ore if one rolls here, otherwise a smooth-noise blob picks a
// stone variant so the underground is not uniformly greystone. v2 also seams in
// the odd pocket of gravel or packed dirt.
BlockId stoneAt(const NoiseSet& n, int wx, int wy, int wz, int ver) {
  const BlockId base = oreAt(n, wx, wy, wz);
  if (base != wk().greystone) return base;  // ore wins the cell
  if (ver >= 2) {
    const double p = n.stonevar.fbm3(wx * 0.035 + 50, wy * 0.035, wz * 0.035 - 50, 2);
    if (p > 0.52) return wk().shingle;  // gravel pocket
    if (p < -0.54) return wk().loam;    // packed-dirt pocket
  }
  const double value = n.stonevar.fbm3(wx * 0.02, wy * 0.02, wz * 0.02, 2);
  if (value > 0.3) return wk().umberstone;
  if (value < -0.3) return wk().slatestone;
  return wk().greystone;
}

bool isCave(const NoiseSet& n, int wx, int wy, int wz, int ver) {
  // Blobby caverns. v2 lowers the threshold with depth, so the deeps open into
  // proper caverns while the near-surface stays tight.
  const double blob = n.cave.fbm3(wx * 0.045, wy * 0.06, wz * 0.045, 3);
  double th = 0.55;
  if (ver >= 2 && wy < 30) {
    const double lowered = 0.55 - (30 - wy) * 0.005;
    th = lowered > 0.42 ? lowered : 0.42;
  }
  if (blob > th) return true;
  // Thin spaghetti tunnels (ridged noise).
  const double t = std::abs(n.cave2.noise3(wx * 0.05, wy * 0.07, wz * 0.05));
  return t < 0.045;
}

// --- trees -------------------------------------------------------------------

enum class TreeKind { Oak, Pine, Dusk, Birch };

struct TreeSpec {
  double density;
  TreeKind kind;
};

TreeSpec treeSpecFor(Biome biome, double wsel) {
  switch (biome) {
    case Biome::Forest:
      return {0.050, wsel < 0.35 ? TreeKind::Pine : wsel < 0.44 ? TreeKind::Dusk : TreeKind::Oak};
    case Biome::Birch:
      return {0.045, wsel < 0.78 ? TreeKind::Birch : TreeKind::Oak};
    case Biome::Snow:
      return {0.020, TreeKind::Pine};
    case Biome::Desert:
      return {0.0, TreeKind::Oak};
    default:  // meadow
      return {0.014, wsel < 0.10   ? TreeKind::Pine
                     : wsel < 0.20 ? TreeKind::Dusk
                     : wsel < 0.28 ? TreeKind::Birch
                                   : TreeKind::Oak};
  }
}

void treeBlocks(TreeKind kind, BlockId& logId, BlockId& leafId) {
  const WellKnownBlocks& w = wk();
  switch (kind) {
    case TreeKind::Pine: logId = w.pine_log; leafId = w.pine_leaves; break;
    case TreeKind::Dusk: logId = w.dusk_log; leafId = w.dusk_leaves; break;
    case TreeKind::Birch: logId = w.birch_log; leafId = w.birch_leaves; break;
    case TreeKind::Oak: default: logId = w.log; leafId = w.leaves; break;
  }
}

const char* treeLeafKey(TreeKind kind) {
  switch (kind) {
    case TreeKind::Pine: return "pine_leaves";
    case TreeKind::Dusk: return "dusk_leaves";
    case TreeKind::Birch: return "birch_leaves";
    case TreeKind::Oak: default: return "leaves";
  }
}

void setIfInChunk(Chunk& chunk, int wx, int wy, int wz, BlockId id, bool force) {
  const int lx = wx - chunk.cx * CX;
  const int lz = wz - chunk.cz * CZ;
  if (lx < 0 || lx >= CX || lz < 0 || lz >= CZ || wy < 0 || wy >= WH) return;
  const int i = localIdx(lx, wy, lz);
  ChunkData& d = *chunk.data;
  if (!force) {
    const BlockId cur = d.voxels[i];
    if (cur != kAir && !blocks().def(cur).isLeaf) return;
  }
  d.voxels[i] = id;
}

// The classic round-canopy tree shared by oak, pine, dusk and birch.
void stampCanopyTree(Chunk& chunk, std::uint32_t seed, int wx, int wz, int h, TreeKind kind) {
  BlockId logId = 0, leafId = 0;
  treeBlocks(kind, logId, leafId);
  const int height = 4 + truncToInt(hash2i(seed ^ 0xa11u, wx, wz) * 3);  // 4..6
  const int topY = h + height;
  for (int dy = -2; dy <= 1; ++dy) {
    const int ly = topY + dy;
    const int rad = dy >= 0 ? 1 : 2;
    for (int lx = -rad; lx <= rad; ++lx) {
      for (int lz = -rad; lz <= rad; ++lz) {
        if (dy < 0 && std::abs(lx) == rad && std::abs(lz) == rad &&
            hash3i(seed, wx + lx, ly, wz + lz) < 0.5) {
          continue;  // rounded corners
        }
        setIfInChunk(chunk, wx + lx, ly, wz + lz, leafId, false);
      }
    }
  }
  for (int t = 1; t <= height; ++t) setIfInChunk(chunk, wx, h + t, wz, logId, true);
}

// A palm: a taller bare trunk crowned by radiating fronds with drooping tips.
void stampPalm(Chunk& chunk, std::uint32_t seed, int wx, int wz, int h) {
  const int height = 5 + truncToInt(hash2i(seed ^ 0xa17u, wx, wz) * 3);  // 5..7
  const int topY = h + height;
  const BlockId leaf = wk().palm_leaves;
  const BlockId logId = wk().palm_log;
  setIfInChunk(chunk, wx, topY + 1, wz, leaf, false);  // crown tuft
  const int fronds[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  for (const auto& f : fronds) {
    setIfInChunk(chunk, wx + f[0], topY, wz + f[1], leaf, false);
    setIfInChunk(chunk, wx + f[0] * 2, topY, wz + f[1] * 2, leaf, false);
    setIfInChunk(chunk, wx + f[0] * 3, topY - 1, wz + f[1] * 3, leaf, false);  // drooping tip
  }
  const int diagonals[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
  for (const auto& d : diagonals) {
    setIfInChunk(chunk, wx + d[0], topY, wz + d[1], leaf, false);
  }
  for (int t = 1; t <= height; ++t) setIfInChunk(chunk, wx, h + t, wz, logId, true);
}

// Trees are stamped by scanning a margin around the chunk, so a canopy that
// originates just outside still appears seamlessly — no cross-chunk writes and no
// ordering dependency. 3 covers the palm fronds.
void stampTrees(Chunk& chunk, const NoiseSet& n, std::uint32_t seed, int ver) {
  const int margin = ver >= 2 ? 3 : 2;
  const int minX = chunk.cx * CX - margin, maxX = chunk.cx * CX + CX + margin;
  const int minZ = chunk.cz * CZ - margin, maxZ = chunk.cz * CZ + CZ + margin;

  // wx outer, wz inner, and trunks are stamped with force=true, so a later tree
  // overwrites an earlier one. Preserve the nesting or dense stands differ.
  for (int wx = minX; wx < maxX; ++wx) {
    for (int wz = minZ; wz < maxZ; ++wz) {
      const double r = hash2i(seed ^ 0x5eedu, wx, wz);
      if (r >= 0.06) continue;  // early out: above any biome's density

      if (ver < 2) {
        if (r >= 0.018) continue;
        const ColumnInfo info = columnInfo(n, wx, wz, ver);
        if (info.h <= kSeaLevel + 1) continue;  // no trees on beaches or in water
        const double wsel = hash2i(seed ^ 0xbeefu, wx, wz);
        const TreeKind kind =
            wsel < 0.15 ? TreeKind::Pine : wsel < 0.3 ? TreeKind::Dusk : TreeKind::Oak;
        stampCanopyTree(chunk, seed, wx, wz, info.h, kind);
        continue;
      }

      const ColumnInfo info = columnInfo(n, wx, wz, ver);
      if (info.h <= kSeaLevel + 1) {
        // Warm beaches grow the odd palm above the tide line.
        if (info.h == kSeaLevel + 1 && info.temperature > 0.22 && r < 0.012) {
          stampPalm(chunk, seed, wx, wz, info.h);
        }
        continue;
      }
      if (ravineFloor(n, wx, wz, info.h)) continue;  // the ground here is carved away
      const double wsel = hash2i(seed ^ 0xbeefu, wx, wz);
      const TreeSpec spec = treeSpecFor(info.biome, wsel);
      if (r >= spec.density) continue;
      stampCanopyTree(chunk, seed, wx, wz, info.h, spec.kind);
    }
  }
}

// --- foliage -----------------------------------------------------------------

// A light dusting of cross-billboard plants so the world reads as lived-in. Purely
// per-column and inside the chunk, so it needs no margin scan. Runs after trees so
// it never grows inside a trunk and fills the gaps between them instead.
BlockId pickFoliage(const NoiseSet& n, std::uint32_t seed, int wx, int wz, BlockId ground) {
  const WellKnownBlocks& w = wk();
  const double r = hash2i(seed ^ 0x0f0au, wx, wz);  // per-cell presence roll

  if (ground == w.sand) {
    if (r < 0.010) return w.dead_shrub;  // sparse desert and beach twigs
    if (r < 0.015) return w.pebbles;
    return kAir;
  }
  if (ground == w.snowturf) {  // snowfields: sparse and bare
    if (r < 0.006) return w.pebbles;
    if (r < 0.009) return w.dead_shrub;
    return kAir;
  }
  if (ground != w.turf) return kAir;  // only grass grows plants

  const double lush = 0.5 + n.flora.fbm2(wx * 0.012, wz * 0.012, 3) * 0.5;  // 0 dry .. 1 wet

  // Secondary rolls MUST offset their coordinates, not just the seed. hash2i
  // barely avalanches a single flipped seed bit, so hash2i(seed^A, wx, wz) and
  // hash2i(seed^B, wx, wz) come out correlated — without the coordinate shifts
  // the sub-type picks below would track `r` and their tails would never fire.

  // Rare shade mushrooms in the wettest hollows.
  if (lush > 0.62 && r < 0.006) {
    return hash2i(seed ^ 0x0f0fu, wx + 53, wz + 199) < 0.5 ? w.mushroom_red : w.mushroom_brown;
  }

  // Flower meadows: a coarse patch field gates them, and each ~12-block region
  // favours one dominant bloom so beds read as coherent colour, not confetti.
  const double patch = n.flora.fbm2(wx * 0.05 + 40, wz * 0.05 - 25, 2);
  if (patch > 0.30 && r < 0.05) {
    // floorDiv, not `/`: JS floors toward negative infinity, so C++ truncation
    // would shift this grid across the whole negative half of every world.
    const int gx = static_cast<int>(floorDiv(wx, 12));
    const int gz = static_cast<int>(floorDiv(wz, 12));
    const double rk = hash2i(seed ^ 0x0f0du, gx, gz);
    const BlockId dominant = w.flowers[static_cast<std::size_t>(truncToInt(rk * 5))];
    if (hash2i(seed ^ 0x0f0eu, wx + 17, wz + 83) < 0.72) return dominant;
    const double pick = hash2i(seed ^ 0x0f0cu, wx + 211, wz + 149);
    return w.flowers[static_cast<std::size_t>(truncToInt(pick * 5))];
  }

  // Grasses: density tracks moisture, ferns favour wet ground, the odd shrub.
  const double grassChance = 0.08 + lush * 0.22;  // ~8% dry .. ~30% lush
  if (r < grassChance) {
    const double t = hash2i(seed ^ 0x0f0bu, wx + 101, wz + 57);
    if (lush > 0.6 && t < 0.18) return w.fern;
    if (t < 0.05) return w.bush;
    if (t > 0.985) return w.pebbles;
    return w.tall_grass;
  }
  return kAir;
}

void stampFoliage(Chunk& chunk, const NoiseSet& n, std::uint32_t seed, int ver) {
  ChunkData& d = *chunk.data;
  const int baseX = chunk.cx * CX, baseZ = chunk.cz * CZ;
  for (int z = 0; z < CZ; ++z) {
    for (int x = 0; x < CX; ++x) {
      const int wx = baseX + x, wz = baseZ + z;
      const int h = columnInfo(n, wx, wz, ver).h;
      if (h + 1 >= WH) continue;
      // Occupied by a trunk, leaf or water.
      if (d.voxels[localIdx(x, h + 1, z)] != kAir) continue;
      const BlockId ground = d.voxels[localIdx(x, h, z)];
      const BlockId plant = pickFoliage(n, seed, wx, wz, ground);
      if (plant != kAir) d.voxels[localIdx(x, h + 1, z)] = plant;
    }
  }
}

// Papyrus (v2): reed clumps 1-3 tall on shore cells — ground at the waterline with
// open water in a neighbouring column. Runs after foliage and overwrites any grass
// tuft that landed on the same cell.
void stampPapyrus(Chunk& chunk, const NoiseSet& n, std::uint32_t seed, int ver) {
  ChunkData& d = *chunk.data;
  const int baseX = chunk.cx * CX, baseZ = chunk.cz * CZ;
  const WellKnownBlocks& w = wk();
  for (int z = 0; z < CZ; ++z) {
    for (int x = 0; x < CX; ++x) {
      const int wx = baseX + x, wz = baseZ + z;
      const double r = hash2i(seed ^ 0x9a90u, wx, wz);
      if (r >= 0.20) continue;
      const ColumnInfo info = columnInfo(n, wx, wz, ver);
      if (info.biome == Biome::Snow) continue;
      if (info.h < kSeaLevel || info.h > kSeaLevel + 1) continue;  // right at the waterline
      const BlockId ground = d.voxels[localIdx(x, info.h, z)];
      if (ground != w.sand && ground != w.turf && ground != w.loam) continue;

      bool shore = false;
      const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (const auto& dir : dirs) {
        if (columnInfo(n, wx + dir[0], wz + dir[1], ver).h < kSeaLevel) {
          shore = true;
          break;
        }
      }
      if (!shore) continue;

      const int tall = 1 + truncToInt((r / 0.20) * 3);  // 1..3
      for (int k = 1; k <= tall; ++k) {
        if (info.h + k >= WH) break;
        const int i = localIdx(x, info.h + k, z);
        const BlockId cur = d.voxels[i];
        // The base reed may replace a grass tuft the foliage pass planted;
        // anything else — trunk, leaf, water — ends the clump.
        if (cur != kAir && !(k == 1 && blocks().def(cur).replaceable)) break;
        d.voxels[i] = w.papyrus;
      }
    }
  }
}

}  // namespace

// --- NoiseSet ---------------------------------------------------------------

NoiseSet::NoiseSet(std::uint32_t seed)
    : terrain(seed),
      hills(seed ^ 0x9e37u),
      cave(seed ^ 0x1b3fu),
      cave2(seed ^ 0x77d1u),
      ore(seed ^ 0x2c91u),
      stonevar(seed ^ 0x5a17u),
      flora(seed ^ 0x0f10u),
      temp(seed ^ 0x3c5au),
      moist(seed ^ 0x66b1u),
      mount(seed ^ 0x14e9u),
      ridge(seed ^ 0x7f23u),
      ravine(seed ^ 0x2ba7u),
      seed_(seed) {}

const char* biomeName(Biome biome) {
  switch (biome) {
    case Biome::Meadow: return "Meadow";
    case Biome::Forest: return "Forest";
    case Biome::Birch: return "Birch Grove";
    case Biome::Desert: return "Desert";
    case Biome::Snow: return "Snowfield";
  }
  return "Meadow";
}

ColumnInfo columnInfo(const NoiseSet& n, int wx, int wz, int ver) {
  const double cont = n.terrain.fbm2(wx * 0.0055, wz * 0.0055, 4);      // broad continents
  const double hills = n.hills.fbm2(wx * 0.021, wz * 0.021, 3) * 0.55;  // local bumps

  ColumnInfo out;
  double h;
  if (ver >= 2) {
    out.temperature = climateT(n, wx, wz);
    out.biome = biomeOf(out.temperature, climateM(n, wx, wz));
    const double flat = out.biome == Biome::Desert ? 0.55 : 1.0;  // dunes, not crags
    // Mountain ranges: a ridged field sharpened inside a rare low-frequency mask,
    // so most of the world stays gentle but ranges rise to real peaks.
    const double mmask =
        smoothstep(0.10, 0.45, n.mount.fbm2(wx * 0.0016 + 91, wz * 0.0016 - 53, 2));
    double ridged = 0.0;
    if (mmask > 0.001) {
      const double band = n.ridge.fbm2(wx * 0.008, wz * 0.008, 3);
      const double peak = 1.0 - std::abs(band);
      // The only transcendental in the whole terrain chain. std::pow is not
      // bit-specified across implementations, so if the golden-vector diff ever
      // fails only on mountain columns, this is the line to suspect.
      ridged = std::pow(peak > 0.0 ? peak : 0.0, 2.6) * 38 * mmask;
    }
    h = kSeaLevel + cont * 24 + hills * 14 * flat + ridged;
  } else {
    h = kSeaLevel + cont * 24 + hills * 14;
  }

  int hi = static_cast<int>(std::floor(h));
  if (hi < 4) hi = 4;
  if (hi > WH - 14) hi = WH - 14;
  out.h = hi;
  return out;
}

int heightAt(const NoiseSet& n, int wx, int wz, int ver) {
  return columnInfo(n, wx, wz, ver).h;
}

Biome biomeAt(const NoiseSet& n, int wx, int wz, int ver) {
  if (ver < 2) return Biome::Meadow;
  return biomeOf(climateT(n, wx, wz), climateM(n, wx, wz));
}

void generate(Chunk& chunk, const NoiseSet& n, int ver) {
  const std::uint32_t seed = n.seed();
  ChunkData& d = *chunk.data;
  const WellKnownBlocks& w = wk();
  const int baseX = chunk.cx * CX, baseZ = chunk.cz * CZ;

  for (int z = 0; z < CZ; ++z) {
    for (int x = 0; x < CX; ++x) {
      const int wx = baseX + x, wz = baseZ + z;
      const ColumnInfo info = columnInfo(n, wx, wz, ver);
      const int h = info.h;
      const bool beach = h <= kSeaLevel + 1;
      const int rvFloor = ver >= 2 ? ravineFloor(n, wx, wz, h) : 0;

      for (int y = 0; y < WH; ++y) {
        BlockId id = kAir;
        if (y < 2) {
          id = w.bedrock;
        } else if (y <= h) {
          const int depth = h - y;
          if (beach) {
            id = depth <= 2 ? w.sand : (depth <= 4 ? w.sandstone : w.greystone);
          } else if (ver >= 2 && info.biome == Biome::Desert) {
            id = depth <= 2 ? w.sand : (depth <= 6 ? w.sandstone : stoneAt(n, wx, y, wz, ver));
          } else if (depth == 0) {
            id = (ver >= 2 && info.biome == Biome::Snow) ? w.snowturf : w.turf;
          } else if (depth <= 3) {
            id = w.loam;
          } else {
            id = stoneAt(n, wx, y, wz, ver);
          }

          // Ravines slice from the surface into the deeps (v2).
          if (rvFloor && y >= rvFloor && id != w.bedrock) {
            id = y <= kDeepWaterY ? w.water : kAir;
          }
          // Otherwise carve caves out of underground solid — not the very top,
          // not bedrock.
          else if (id != w.bedrock && depth >= 1 && y > 2 && isCave(n, wx, y, wz, ver)) {
            if (y <= kSeaLevel && depth <= 2) {
              // Keep the sea floor sealed.
            } else {
              id = (ver >= 2 && y <= kDeepWaterY) ? w.water : kAir;
            }
          }
        } else if (y <= kSeaLevel) {
          id = w.water;
        }
        d.voxels[localIdx(x, y, z)] = id;
      }
    }
  }

  // Order is load-bearing: papyrus overwrites foliage, foliage skips cells trees
  // already filled.
  stampTrees(chunk, n, seed, ver);
  stampFoliage(chunk, n, seed, ver);
  if (ver >= 2) stampPapyrus(chunk, n, seed, ver);
  chunk.generated = true;
}

SurfacePreview surfacePreview(const NoiseSet& n, int wx, int wz, int ver) {
  const std::uint32_t seed = n.seed();
  const ColumnInfo info = columnInfo(n, wx, wz, ver);
  const int h = info.h;
  if (h < kSeaLevel) return {"water", h};
  if (ver >= 2 && ravineFloor(n, wx, wz, h)) return {"greystone", 14};

  // Tree canopies: any tree rooted within 2 cells shades this column.
  for (int dx = -2; dx <= 2; ++dx) {
    for (int dz = -2; dz <= 2; ++dz) {
      const int tx = wx + dx, tz = wz + dz;
      const double r = hash2i(seed ^ 0x5eedu, tx, tz);
      if (r >= 0.06) continue;
      const ColumnInfo ti = (dx || dz) ? columnInfo(n, tx, tz, ver) : info;
      if (ti.h <= kSeaLevel + 1) continue;
      if (ver >= 2 && ravineFloor(n, tx, tz, ti.h)) continue;  // no tree in a ravine

      bool haveKind = false;
      TreeKind kind = TreeKind::Oak;
      if (ver < 2) {
        if (r < 0.018) {
          const double wsel = hash2i(seed ^ 0xbeefu, tx, tz);
          kind = wsel < 0.15 ? TreeKind::Pine : wsel < 0.3 ? TreeKind::Dusk : TreeKind::Oak;
          haveKind = true;
        }
      } else {
        const TreeSpec spec = treeSpecFor(ti.biome, hash2i(seed ^ 0xbeefu, tx, tz));
        if (r < spec.density) {
          kind = spec.kind;
          haveKind = true;
        }
      }
      if (haveKind) return {treeLeafKey(kind), ti.h + 5};
    }
  }

  if (h <= kSeaLevel + 1) return {"sand", h};
  if (ver >= 2 && info.biome == Biome::Desert) return {"sand", h};
  if (ver >= 2 && info.biome == Biome::Snow) return {"snowturf", h};
  return {"turf", h};
}

}  // namespace hr::world

#if defined(_MSC_VER)
#pragma float_control(pop)
#endif
