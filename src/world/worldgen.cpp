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
// The low-frequency band that decides WHERE a ravine runs, split out from the
// depth so a caller can ask the cheap half on its own — spawn selection probes
// eighty columns per candidate and wants nothing to do with a heightmap for the
// seventy-nine of them that are nowhere near a canyon.
bool inRavineBand(const NoiseSet& n, int wx, int wz) {
  return std::abs(n.ravine.fbm2(wx * 0.0045 + 7, wz * 0.0045 - 3, 2)) < kRavineBand;
}
int ravineFloor(const NoiseSet& n, int wx, int wz, int h, int ver) {
  const int sea = seaLevel(ver);
  if (h <= sea + 2) return 0;  // never crack open the sea floor
  if (!inRavineBand(n, wx, wz)) return 0;
  // Depth varies along the crack so the floor undulates. Measured down from the
  // sea rather than up from bedrock, so a v3 ravine is the same canyon in a
  // deeper world instead of one that reaches four times further toward the ore.
  // The v2 arithmetic is unchanged by construction: 46 - 32 = 14, 46 - 38 = 8.
  const double d = n.ravine.fbm2(wx * 0.02 - 40, wz * 0.02 + 25, 2);
  const int floorY = (sea - 32) + static_cast<int>(std::floor(d * 5));
  const int floorMin = ravineFloorMin(ver);
  return floorY > floorMin ? floorY : floorMin;
}

// Water line for flooded deep caverns (v2 and v3): carved space at or below this
// level fills with still water instead of air. 46 - 34 = 12 keeps v2 exactly as it
// was. v4 abandons the line for pockets — see waterPocketTop and the note in
// worldgen.h — but the arithmetic stays here because a v2 or v3 world has to keep
// generating the terrain it was made with.
int deepWaterY(int ver) { return seaLevel(ver) - 34; }

// v4: the level an underground lake fills to in this column, or 0 for a dry one.
//
// `where` is a broad, slow field, and most of the world sits under the threshold
// and holds no water at all. Where it does rise, how far it has risen is what sets
// the level — so a lake is deepest through the middle of the region carrying it
// and tapers away at the rim. Scaling the ceiling rather than adding to a floor is
// what lets the taper actually reach nothing: out at the edge the level is a few
// blocks off bedrock, so only the very bottom of a cave there is wet, and past the
// edge there is no water at all.
int waterPocketTop(const NoiseSet& n, int wx, int wz, int ver) {
  if (ver < 4) return 0;
  const double where = n.aquifer.fbm2(wx * 0.0060 + 13.0, wz * 0.0060 - 29.0, 2);
  if (where <= kPocketThreshold) return 0;
  const double t = std::min(1.0, (where - kPocketThreshold) / kPocketSpan);
  return static_cast<int>(std::floor(t * pocketCeiling(ver)));
}

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

// v3. Same veins, same noise, same salts — only the bands move, so a seed's ore
// keeps its shape and changes its depth.
//
// The first four are the "you have to go and find it" tier and every one of them
// ends at or below deepOreCeiling (y=55), which sits seven blocks under the
// deepest a ravine floor can reach (y=62). That is the guarantee: no amount of
// standing at the bottom of a canyon shows you aetherite. Caves still can, and
// should — spelunking is meant to pay.
//
// The rest are the "you will trip over it" tier and are allowed near the surface,
// because a new player needs copper and coal without a mining expedition.
const OreSpec kOresV3[] = {
    {"ore_aetherite", 3, 30, 0.19, 0.73, 11, 0, 23},
    {"ore_gloamite", 3, 38, 0.19, 0.72, 55, 13, 88},
    {"ore_sparkstone", 4, 46, 0.19, 0.71, 40, 5, 70},
    {"ore_sunbrass", 4, 55, 0.18, 0.69, 80, 9, 14},
    {"ore_azurite", 6, 70, 0.18, 0.67, 3, 50, 31},
    {"ore_verdanite", 8, 80, 0.18, 0.67, 21, 60, 47},
    {"ore_ferralite", 8, 88, 0.18, 0.66, 60, 22, 90},
    {"ore_copper", 12, 98, 0.17, 0.62, 17, 33, 5},
    {"ore_embercoal", 16, 132, 0.17, 0.60, 99, 71, 41},
};
static_assert(std::size(kOres) == std::size(kOresV3), "both tables index oreIds()");

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

BlockId oreAt(const NoiseSet& n, int wx, int wy, int wz, int ver) {
  const OreIds& ids = oreIds();
  const OreSpec* table = ver >= 3 ? kOresV3 : kOres;
  for (std::size_t i = 0; i < std::size(kOres); ++i) {
    const OreSpec& o = table[i];
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
  const BlockId base = oreAt(n, wx, wy, wz, ver);
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
  // Where the caverns start opening up. Measured from BEDROCK, not from the sea:
  // this is a "how close to the bottom of the world" feature, and the first version
  // of v3 scaled it with sea level, which turned eighty blocks of the new depth
  // into maximum-density sponge instead of the thin seam v2 had above bedrock.
  //
  // That was the whole performance regression. Cave surface is the most expensive
  // geometry in the game -- it is all interior faces, none of it is visible from
  // anywhere, and there was five times as much of it as v2 had. It also worked
  // against the point of the deeper world: rare ore is supposed to be something you
  // mine toward, not something lying in an open cavern.
  constexpr int kDeepCavernStart = 30;
  const int deepStart = kDeepCavernStart;
  if (ver >= 2 && wy < deepStart) {
    const double lowered = 0.55 - (deepStart - wy) * 0.005;
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
    const BlockId cur = d.voxels.get(i);
    if (cur != kAir && !blocks().def(cur).isLeaf) return;
  }
  d.voxels.set(i, id);
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
  const int sea = seaLevel(ver);
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
        if (info.h <= sea + 1) continue;  // no trees on beaches or in water
        const double wsel = hash2i(seed ^ 0xbeefu, wx, wz);
        const TreeKind kind =
            wsel < 0.15 ? TreeKind::Pine : wsel < 0.3 ? TreeKind::Dusk : TreeKind::Oak;
        stampCanopyTree(chunk, seed, wx, wz, info.h, kind);
        continue;
      }

      const ColumnInfo info = columnInfo(n, wx, wz, ver);
      if (info.h <= sea + 1) {
        // Warm beaches grow the odd palm above the tide line.
        if (info.h == sea + 1 && info.temperature > 0.22 && r < 0.012) {
          stampPalm(chunk, seed, wx, wz, info.h);
        }
        continue;
      }
      if (ravineFloor(n, wx, wz, info.h, ver)) continue;  // the ground here is carved away
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
// Which wild crops suit a biome. Snow and desert lists are short by design: a
// player who wants a full diet has to go somewhere, and a biome that grew
// everything would make travelling pointless.
const std::vector<BlockId>& cropsForBiome(const WellKnownBlocks& w, Biome b) {
  switch (b) {
    case Biome::Forest: return w.cropsForest;
    case Biome::Birch: return w.cropsBirch;
    case Biome::Desert: return w.cropsDesert;
    case Biome::Snow: return w.cropsSnow;
    case Biome::Meadow: break;
  }
  return w.cropsMeadow;
}

BlockId pickFoliage(const NoiseSet& n, std::uint32_t seed, int wx, int wz, BlockId ground,
                    Biome biome, int ver) {
  const WellKnownBlocks& w = wk();
  const double r = hash2i(seed ^ 0x0f0au, wx, wz);  // per-cell presence roll
  const double lush = 0.5 + n.flora.fbm2(wx * 0.012, wz * 0.012, 3) * 0.5;  // 0 dry .. 1 wet

  // Secondary rolls MUST offset their coordinates, not just the seed. hash2i
  // barely avalanches a single flipped seed bit, so hash2i(seed^A, wx, wz) and
  // hash2i(seed^B, wx, wz) come out correlated — without the coordinate shifts
  // the sub-type picks below would track `r` and their tails would never fire.

  // Wild crop stands (v6).
  //
  // THIS RUNS BEFORE THE GROUND BRANCHES BELOW, and that placement is the whole
  // point rather than a tidiness choice. Sand and snowturf each return outright, so
  // a crop roll written after them could only ever fire on turf — which quietly made
  // the desert list (melon, chili, maize) and the snow list unreachable in the entire
  // game. Three crops existed, were painted, were registered, and could not be found
  // by anybody.
  //
  // Sand only counts when the BIOME is Desert, never merely because the ground is
  // sand. Every lake and coastline in the world is sand too, and without that test
  // the first build of this grew grapes and tomatoes along every beach on the map.
  const bool arable = ground == w.turf || (ground == w.sand && biome == Biome::Desert) ||
                      (ground == w.snowturf && biome == Biome::Snow);
  if (ver >= 6 && arable) {
    // Rice answers to wet ground rather than to biome, so it is asked first and
    // separately, and only on soil.
    if (ground == w.turf && lush > 0.66 && r < 0.055 &&
        hash2i(seed ^ 0x0f12u, wx + 77, wz + 313) < 0.5 && w.cropRice != kAir) {
      return w.cropRice;
    }
    // A coarse field decides WHERE a stand can be and a ~10-block region picks its
    // one dominant crop, so a patch reads as a stand of something. Desert and snow
    // stands are rarer: they are the reward for going somewhere unpleasant, and a
    // snowfield thick with cabbages would look absurd.
    const bool harsh = (ground != w.turf);
    const double field = n.flora.fbm2(wx * 0.045 - 130, wz * 0.045 + 88, 2);
    if (field > (harsh ? 0.14 : 0.06) && r < (harsh ? 0.055 : 0.075)) {
      const std::vector<BlockId>& list = cropsForBiome(w, biome);
      if (!list.empty()) {
        // floorDiv, not `/`, for the same reason the flower grid below uses it: C++
        // truncation would shift the whole grid across the negative half of a world.
        const int gx = static_cast<int>(floorDiv(wx, 10));
        const int gz = static_cast<int>(floorDiv(wz, 10));
        const double rk = hash2i(seed ^ 0x0f11u, gx + 401, gz + 617);
        int pick = truncToInt(rk * static_cast<double>(list.size()));
        // hash2i is [0, 1), so this cannot fire — but the flower code below indexes a
        // fixed five where this indexes a list edited by hand, and an out-of-range
        // read here would be a crash inside chunk generation.
        if (pick < 0) pick = 0;
        if (pick >= static_cast<int>(list.size())) pick = static_cast<int>(list.size()) - 1;
        return list[static_cast<std::size_t>(pick)];
      }
    }
  }

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
    // Eight species from v7, five before it. Gated on the SPECIES COUNT rather than
    // by adding a separate roll, which is what keeps a v6 world byte-identical: the
    // same hashes are drawn in the same order with the same seeds, and only the
    // number they are scaled by changes. A world made before v7 therefore still
    // grows exactly the meadow it always did.
    const std::size_t species = ver >= 7 ? w.flowersV7.size() : w.flowers.size();
    const auto flowerAt = [&w, ver](std::size_t i) {
      return ver >= 7 ? w.flowersV7[i] : w.flowers[i];
    };
    const double rk = hash2i(seed ^ 0x0f0du, gx, gz);
    const BlockId dominant = flowerAt(static_cast<std::size_t>(truncToInt(rk * species)));
    if (hash2i(seed ^ 0x0f0eu, wx + 17, wz + 83) < 0.72) return dominant;
    const double pick = hash2i(seed ^ 0x0f0cu, wx + 211, wz + 149);
    return flowerAt(static_cast<std::size_t>(truncToInt(pick * species)));
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
      const ColumnInfo col = columnInfo(n, wx, wz, ver);
      const int h = col.h;
      if (h + 1 >= WH) continue;
      // Occupied by a trunk, leaf or water.
      if (d.voxels.get(localIdx(x, h + 1, z)) != kAir) continue;
      const BlockId ground = d.voxels.get(localIdx(x, h, z));
      const BlockId plant = pickFoliage(n, seed, wx, wz, ground, col.biome, ver);
      if (plant == kAir) continue;
      d.voxels.set(localIdx(x, h + 1, z), plant);
      // A wild crop generates RIPE. Nothing in the world tends it — the growth
      // sweep only ever visits crops the player planted — so a stand that came up
      // as seedlings would stay seedlings for the life of the world.
      const BlockDef& pd = blocks().def(plant);
      if (pd.cropStages > 0) {
        d.meta.set(localIdx(x, h + 1, z),
                   static_cast<std::uint8_t>(cropMetaFor(pd.cropStages - 1)));
      }
    }
  }
}

// Papyrus (v2): reed clumps 1-3 tall on shore cells — ground at the waterline with
// open water in a neighbouring column. Runs after foliage and overwrites any grass
// tuft that landed on the same cell.
void stampPapyrus(Chunk& chunk, const NoiseSet& n, std::uint32_t seed, int ver) {
  const int sea = seaLevel(ver);
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
      if (info.h < sea || info.h > sea + 1) continue;  // right at the waterline
      const BlockId ground = d.voxels.get(localIdx(x, info.h, z));
      if (ground != w.sand && ground != w.turf && ground != w.loam) continue;

      bool shore = false;
      const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      for (const auto& dir : dirs) {
        if (columnInfo(n, wx + dir[0], wz + dir[1], ver).h < sea) {
          shore = true;
          break;
        }
      }
      if (!shore) continue;

      const int tall = 1 + truncToInt((r / 0.20) * 3);  // 1..3
      for (int k = 1; k <= tall; ++k) {
        if (info.h + k >= WH) break;
        const int i = localIdx(x, info.h + k, z);
        const BlockId cur = d.voxels.get(i);
        // The base reed may replace a grass tuft the foliage pass planted;
        // anything else — trunk, leaf, water — ends the clump.
        if (cur != kAir && !(k == 1 && blocks().def(cur).replaceable)) break;
        d.voxels.set(i, w.papyrus);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Dungeons (v5)
// ---------------------------------------------------------------------------
//
// The first structure in the game, and the first thing here that is an *object*
// rather than a field. Caves and ravines are thresholds on noise, evaluated per
// cell and continuous across a border because both sides ask the same question at
// the same coordinate. A dungeon cannot work that way: it has a centre, a count of
// rooms, and a chest in a particular corner.
//
// So it follows the pattern trees established instead — the margin scan. Every
// chunk independently re-derives any dungeon whose footprint could reach it, stamps
// the whole thing, and throws away the writes that fall outside its own bounds. No
// chunk ever writes to another and no chunk depends on another having been
// generated first, which is what keeps generation safe to run on any worker in any
// order.
//
// The difference from trees is what gets scanned. A tree is one column, so trees
// scan columns. A dungeon is tens of blocks across, and scanning every column in a
// margin that wide would be thousands of hash calls per chunk. Dungeons sit on a
// LATTICE instead: one candidate per kDungeonCell square, so a chunk tests a
// handful of cells and rejects almost all of them on a single hash.
//
// The jitter and the footprint are both sized so a dungeon is always contained
// entirely within its own lattice cell (16 + 48 + 31 < 96). That is what makes the
// scan below exact rather than approximate, and it is worth preserving: widen the
// footprint past the cell and chunks on the far side stop seeing the half that
// reaches them.

constexpr int kDungeonCell = 96;      // lattice pitch
constexpr int kDungeonJitter = 48;    // how far the origin wanders inside its cell
constexpr int kDungeonInset = 16;     // and how far it stays off the cell's edge
constexpr double kDungeonChance = 0.55;
constexpr int kRoomSlot = 20;         // pitch of the 2x2 grid rooms are placed on
constexpr int kRoomMin = 7, kRoomMax = 11;
constexpr int kRoomHeight = 4;        // interior, floor to ceiling
constexpr int kDungeonSpan = kRoomSlot + kRoomMax;  // 31: the widest a dungeon gets
constexpr int kRockAbove = 8;         // rock that must remain over the ceiling
constexpr int kDungeonYMin = 16, kDungeonYMax = 42;
constexpr int kMaxRooms = 4;

// Salts and coordinate steps. Distinct steps rather than distinct salts alone,
// because hash2i barely avalanches one flipped seed bit — see core/prng.h. Rolling
// everything at (cellX, cellZ) with only the salt changed would correlate the room
// count with the depth with the layout.
constexpr std::uint32_t kSaltDunExists = 0xd0e0u;
constexpr std::uint32_t kSaltDunX = 0xd0e1u;
constexpr std::uint32_t kSaltDunZ = 0xd0e2u;
constexpr std::uint32_t kSaltDunY = 0xd0e3u;
constexpr std::uint32_t kSaltDunRooms = 0xd0e4u;
constexpr std::uint32_t kSaltDunSize = 0xd0e5u;

struct DungeonRoom {
  int x = 0, z = 0;  // min corner of the interior floor
  int w = 0, d = 0;  // interior extent
};

struct DungeonPlan {
  bool exists = false;
  int floorY = 0;
  int roomCount = 0;
  DungeonRoom rooms[kMaxRooms];
  int altarRoom = 0;  // which room holds the altar; the rest hold chests
  // The way in. A sealed dungeon can only be found by mining blindly into one,
  // which in practice means never — so the hub reaches out to whatever cave is
  // nearest and opens into it. False when there is no cave within reach, which
  // leaves that one sealed rather than tunnelling until it finds something.
  bool tunnel = false;
  int tunnelDx = 0, tunnelDz = 0;  // unit direction, one axis only
  int tunnelLen = 0;               // cells from the hub's centre
};

const struct DungeonBlocks {
  BlockId wall = 0, floor = 0, chest = 0, altar = 0;
  DungeonBlocks() {
    wall = blocks().idOf("bricks");
    floor = blocks().idOf("cobbled");
    chest = blocks().idOf("chest");
    altar = blocks().idOf("evil_altar");
  }
} & dungeonBlocks() {
  static const DungeonBlocks b;
  return b;
}

// The four slots a room may occupy, as offsets in kRoomSlot units. Room 0 always
// takes the first, so a dungeon is never empty and the corridors below always have
// a hub to run from.
constexpr int kSlotX[kMaxRooms] = {0, 1, 0, 1};
constexpr int kSlotZ[kMaxRooms] = {0, 0, 1, 1};

// Everything about the dungeon in one lattice cell, or `exists = false`. Pure: the
// same cell always yields the same plan, on any machine and any thread, which is
// what lets the chest positions be re-derived later instead of stored.
DungeonPlan dungeonPlan(const NoiseSet& n, std::uint32_t seed, int cellX, int cellZ, int ver) {
  DungeonPlan plan;
  if (ver < 5) return plan;
  if (hash2i(seed ^ kSaltDunExists, cellX, cellZ) >= kDungeonChance) return plan;

  const int ox = cellX * kDungeonCell + kDungeonInset +
                 truncToInt(hash2i(seed ^ kSaltDunX, cellX + 31, cellZ + 7) * kDungeonJitter);
  const int oz = cellZ * kDungeonCell + kDungeonInset +
                 truncToInt(hash2i(seed ^ kSaltDunZ, cellX + 101, cellZ + 59) * kDungeonJitter);

  plan.roomCount =
      2 + truncToInt(hash2i(seed ^ kSaltDunRooms, cellX + 13, cellZ + 191) * (kMaxRooms - 1));
  if (plan.roomCount > kMaxRooms) plan.roomCount = kMaxRooms;

  for (int i = 0; i < plan.roomCount; ++i) {
    DungeonRoom& r = plan.rooms[i];
    const double rs = hash2i(seed ^ kSaltDunSize, ox + i * 37, oz + i * 149);
    const int size = kRoomMin + truncToInt(rs * (kRoomMax - kRoomMin + 1));
    r.w = size;
    r.d = size;
    r.x = ox + kSlotX[i] * kRoomSlot;
    r.z = oz + kSlotZ[i] * kRoomSlot;
  }

  // The altar goes in the largest room, ties going to the earliest. A rule rather
  // than a roll, so the room worth walking to is the one that looks worth it.
  for (int i = 1; i < plan.roomCount; ++i) {
    if (plan.rooms[i].w * plan.rooms[i].d >
        plan.rooms[plan.altarRoom].w * plan.rooms[plan.altarRoom].d) {
      plan.altarRoom = i;
    }
  }

  const int band = kDungeonYMax - kDungeonYMin + 1;
  plan.floorY =
      kDungeonYMin + truncToInt(hash2i(seed ^ kSaltDunY, ox + 5, oz + 23) * band);

  // Never break the surface. The heightmap is sampled across the footprint rather
  // than at the centre, because a dungeon under a hillside would otherwise poke out
  // of the low side — and a chamber open to the sky is lit, which switches its
  // altar off and makes the whole thing pointless.
  //
  // heightAt does not know about ravines, so one can still slice a dungeon open.
  // That is left alone deliberately: it is a rare and good way to find one.
  const int ceiling = plan.floorY + kRoomHeight + 1;
  int lowest = WH;
  for (int sx = 0; sx <= kDungeonSpan; sx += 8) {
    for (int sz = 0; sz <= kDungeonSpan; sz += 8) {
      const int h = heightAt(n, ox + sx, oz + sz, ver);
      if (h < lowest) lowest = h;
    }
  }
  if (ceiling + kRockAbove > lowest) return plan;  // still exists = false

  plan.exists = true;

  // The way in. Probe the four cardinals from the hub for the nearest cave and
  // run a tunnel to it.
  //
  // Straight and axis-aligned rather than a path toward some chosen cave: this is
  // re-derived by every chunk that overlaps the dungeon, so it has to be the same
  // answer computed from nothing but the seed and a handful of samples. A search
  // would not be.
  //
  // Direction order breaks ties, and the surface check ends a direction rather
  // than skipping past it — a tunnel that dives under a valley and comes up inside
  // it would light the altar from the far end, which is the one thing a dungeon
  // cannot survive.
  const DungeonRoom& hub = plan.rooms[0];
  const int hx = hub.x + hub.w / 2, hz = hub.z + hub.d / 2;
  constexpr int kTunnelMax = 40;
  constexpr int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

  // A cave inside the dungeon's own footprint is not a way out. isCave is pure
  // noise and knows nothing about what is about to be built on top of it, so it
  // happily reports the natural cavern the altar chamber is going to replace —
  // and the first version of this accepted one three blocks from the hub's centre,
  // which is still inside a room eight across. The tunnel was real, deterministic,
  // and entirely enclosed by the room it started in.
  const auto insideDungeon = [&plan](int x, int z) {
    for (int i = 0; i < plan.roomCount; ++i) {
      const DungeonRoom& r = plan.rooms[i];
      if (x >= r.x - 1 && x <= r.x + r.w && z >= r.z - 1 && z <= r.z + r.d) return true;
    }
    return false;
  };

  int bestLen = -1, bestDir = -1;
  for (int d = 0; d < 4; ++d) {
    for (int dist = 1; dist <= kTunnelMax; ++dist) {
      const int px = hx + dirs[d][0] * dist, pz = hz + dirs[d][1] * dist;
      if (heightAt(n, px, pz, ver) < ceiling + kRockAbove) break;
      if (insideDungeon(px, pz)) continue;  // still indoors; keep walking
      // Somewhere you can actually stand, not the first cell the noise says yes to.
      //
      // isCave is true for the thin spaghetti tunnels as well as the caverns, and
      // those are frequent enough that the nearest hit is usually a one-cell speck
      // a few blocks past the wall. A tunnel to one of those is a corridor ending
      // in a dead end, which is worse than no tunnel: it looks like the way out.
      // So count the open space around the candidate and insist on a real opening.
      int open = 0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          for (int dz = -1; dz <= 1; ++dz) {
            if (isCave(n, px + dx, plan.floorY + 1 + dy, pz + dz, ver)) ++open;
          }
        }
      }
      if (open < 9) continue;  // a third of the neighbourhood, and the centre with it
      if (bestLen < 0 || dist < bestLen) {
        bestLen = dist;
        bestDir = d;
      }
      break;
    }
  }
  if (bestLen > 0) {
    plan.tunnel = true;
    plan.tunnelDx = dirs[bestDir][0];
    plan.tunnelDz = dirs[bestDir][1];
    plan.tunnelLen = bestLen;
  }
  return plan;
}

// A space in the dungeon, given by its INTERIOR extent. The shell sits one block
// outside it on every face.
struct DungeonBox {
  int x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
  // Whether this space is built or merely bored.
  //
  // Rooms and the corridors between them are masonry: shell first, then hollow.
  // The tunnel out to the cave is not — it is a hole through rock, and shelling it
  // would lay a brick wall across its far end, one block short of the cave it
  // exists to reach. Which is exactly what it did: every dungeon had a beautifully
  // built corridor to nowhere.
  bool shell = true;
};

// Masonry: the shell around a box, and the floor under it.
//
// Every shell in the dungeon is laid before any interior is hollowed, and that
// ordering is the whole reason this is split in two. Doing a box at a time — shell
// and interior together — means the second box's walls are stamped through the first
// box's air, which walls a corridor across the room it was supposed to open into and
// leaves the altar standing in a one-block slot. Which is exactly what it did.
void stampShell(Chunk& chunk, const DungeonBox& b) {
  const DungeonBlocks& blk = dungeonBlocks();
  for (int x = b.x0 - 1; x <= b.x1 + 1; ++x) {
    for (int z = b.z0 - 1; z <= b.z1 + 1; ++z) {
      for (int y = b.y0 - 1; y <= b.y1 + 1; ++y) {
        if (y < 3 || y >= WH) continue;  // never disturb bedrock
        const bool inside =
            x >= b.x0 && x <= b.x1 && z >= b.z0 && z <= b.z1 && y >= b.y0 && y <= b.y1;
        if (inside) continue;
        setIfInChunk(chunk, x, y, z, y == b.y0 - 1 ? blk.floor : blk.wall, true);
      }
    }
  }
}

// And the air inside it, cut afterwards so it opens through anything laid above.
void stampHollow(Chunk& chunk, const DungeonBox& b) {
  for (int x = b.x0; x <= b.x1; ++x) {
    for (int z = b.z0; z <= b.z1; ++z) {
      for (int y = b.y0; y <= b.y1; ++y) {
        if (y < 3 || y >= WH) continue;
        setIfInChunk(chunk, x, y, z, kAir, true);
      }
    }
  }
}

// Where a room's chest stands: one in from the corner furthest from the hub, so it
// is visible on the way in rather than behind you.
void roomChest(const DungeonRoom& r, int floorY, int& cx, int& cy, int& cz) {
  cx = r.x + r.w - 1;
  cy = floorY;
  cz = r.z + r.d - 1;
}

// Every space in a dungeon, rooms then the corridors joining them. Collected first
// so the two masonry passes below can each run over the whole set.
int dungeonBoxes(const DungeonPlan& plan, DungeonBox* out) {
  const int y0 = plan.floorY;
  const int y1 = plan.floorY + kRoomHeight - 1;
  int n = 0;

  for (int i = 0; i < plan.roomCount; ++i) {
    const DungeonRoom& r = plan.rooms[i];
    out[n++] = DungeonBox{r.x, y0, r.z, r.x + r.w - 1, y1, r.z + r.d - 1};
  }

  // An L from the hub to each other room: along x at the hub's centre line, then
  // along z at the target's. Centre to centre, so it always lands inside both rooms
  // and can never miss — the overlap costs nothing once the interior pass runs last.
  // Corridors are lower than the rooms, which is what makes a doorway read as a
  // doorway from inside.
  const DungeonRoom& hub = plan.rooms[0];
  const int hx = hub.x + hub.w / 2, hz = hub.z + hub.d / 2;
  for (int i = 1; i < plan.roomCount; ++i) {
    const DungeonRoom& r = plan.rooms[i];
    const int tx = r.x + r.w / 2, tz = r.z + r.d / 2;
    const int lox = hx < tx ? hx : tx, hix = hx < tx ? tx : hx;
    const int loz = hz < tz ? hz : tz, hiz = hz < tz ? tz : hz;
    out[n++] = DungeonBox{lox, y0, hz, hix, y0 + 2, hz};
    out[n++] = DungeonBox{tx, y0, loz, tx, y0 + 2, hiz};
  }

  // And the way out, which starts inside the hub and ends in open cave. Overlapping
  // the room costs nothing once the interior pass runs last, and starting at the
  // centre rather than the wall means the mouth is always cut through the wall
  // rather than stopping a block short of it.
  if (plan.tunnel) {
    // One past the cave cell that was found, so the bore ends inside open space
    // rather than against the last block of rock in front of it.
    const int ex = hx + plan.tunnelDx * (plan.tunnelLen + 1);
    const int ez = hz + plan.tunnelDz * (plan.tunnelLen + 1);
    const int lox = hx < ex ? hx : ex, hix = hx < ex ? ex : hx;
    const int loz = hz < ez ? hz : ez, hiz = hz < ez ? ez : hz;
    out[n++] = DungeonBox{lox, y0, loz, hix, y0 + 2, hiz, /*shell=*/false};
  }
  return n;
}

void stampDungeon(Chunk& chunk, const DungeonPlan& plan) {
  if (!plan.exists) return;
  const DungeonBlocks& b = dungeonBlocks();
  const int y0 = plan.floorY;

  // Rooms, two corridor legs per room after the first, and the way out.
  DungeonBox boxes[kMaxRooms + (kMaxRooms - 1) * 2 + 1];
  const int count = dungeonBoxes(plan, boxes);

  // Masonry, then air, then furniture. Three passes over the whole dungeon rather
  // than one pass per box: see stampShell.
  for (int i = 0; i < count; ++i) {
    if (boxes[i].shell) stampShell(chunk, boxes[i]);
  }
  for (int i = 0; i < count; ++i) stampHollow(chunk, boxes[i]);

  for (int i = 0; i < plan.roomCount; ++i) {
    const DungeonRoom& r = plan.rooms[i];
    if (i == plan.altarRoom) {
      // Dead centre, on the floor. No light anywhere in here: the altar only
      // breeds in pitch dark, so a torch would be the one decoration that turns
      // the dungeon off.
      setIfInChunk(chunk, r.x + r.w / 2, y0, r.z + r.d / 2, b.altar, true);
    }
    int cx = 0, cy = 0, cz = 0;
    roomChest(r, y0, cx, cy, cz);
    setIfInChunk(chunk, cx, cy, cz, b.chest, true);
  }
}

// Which lattice cells a chunk has to consider. A dungeon is contained inside its
// own cell by construction, so this only has to cover the cells the chunk overlaps
// — the extra ring is cheap insurance against that invariant being edited away.
void dungeonCellRange(int cx, int cz, int& x0, int& x1, int& z0, int& z1) {
  const auto cellOf = [](int world) {
    return world >= 0 ? world / kDungeonCell : -(((-world) + kDungeonCell - 1) / kDungeonCell);
  };
  x0 = cellOf(cx * CX) - 1;
  x1 = cellOf(cx * CX + CX - 1) + 1;
  z0 = cellOf(cz * CZ) - 1;
  z1 = cellOf(cz * CZ + CZ - 1) + 1;
}

void stampDungeons(Chunk& chunk, const NoiseSet& n, std::uint32_t seed, int ver) {
  if (ver < 5) return;
  int x0 = 0, x1 = 0, z0 = 0, z1 = 0;
  dungeonCellRange(chunk.cx, chunk.cz, x0, x1, z0, z1);
  for (int cellX = x0; cellX <= x1; ++cellX) {
    for (int cellZ = z0; cellZ <= z1; ++cellZ) {
      stampDungeon(chunk, dungeonPlan(n, seed, cellX, cellZ, ver));
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
      aquifer(seed ^ 0x6dc5u),
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
  const int sea = seaLevel(ver);
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
    h = sea + cont * 24 + hills * 14 * flat + ridged;
  } else {
    h = sea + cont * 24 + hills * 14;
  }

  int hi = static_cast<int>(std::floor(h));
  if (hi < 4) hi = 4;
  const int top = topClamp(ver);
  if (hi > top) hi = top;
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

bool ravineAt(const NoiseSet& n, int wx, int wz, int ver) {
  if (ver < 2) return false;  // v1 has no ravines at all
  // Band first, height second — the order that makes probing a neighbourhood of
  // this affordable. Both must hold, so asking them in either order is the same
  // answer; only the cost differs.
  if (!inRavineBand(n, wx, wz)) return false;
  return ravineFloor(n, wx, wz, heightAt(n, wx, wz, ver), ver) != 0;
}

void generate(Chunk& chunk, const NoiseSet& n, int ver) {
  const int sea = seaLevel(ver);
  const int deepWater = deepWaterY(ver);
  const std::uint32_t seed = n.seed();
  ChunkData& d = *chunk.data;
  const WellKnownBlocks& w = wk();
  const int baseX = chunk.cx * CX, baseZ = chunk.cz * CZ;

  for (int z = 0; z < CZ; ++z) {
    for (int x = 0; x < CX; ++x) {
      const int wx = baseX + x, wz = baseZ + z;
      const ColumnInfo info = columnInfo(n, wx, wz, ver);
      const int h = info.h;
      const bool beach = h <= sea + 1;
      const int rvFloor = ver >= 2 ? ravineFloor(n, wx, wz, h, ver) : 0;
      const int pocket = waterPocketTop(n, wx, wz, ver);
      // What a cell that has been carved out becomes. Up to v3 that was decided by
      // one world-wide water line; from v4 it is this column's own lake level, and
      // most columns have none.
      const auto carved = [&](int cy) {
        if (ver >= 4) return cy <= pocket ? w.water : kAir;
        return (ver >= 2 && cy <= deepWater) ? w.water : kAir;
      };

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
            id = carved(y);
          }
          // Otherwise carve caves out of underground solid — not the very top,
          // not bedrock.
          else if (id != w.bedrock && depth >= 1 && y > 2 && isCave(n, wx, y, wz, ver)) {
            if (y <= sea && depth <= 2) {
              // Keep the sea floor sealed.
            } else {
              id = carved(y);
            }
          }
        } else if (y <= sea) {
          id = w.water;
        }
        d.voxels.set(localIdx(x, y, z), id);
      }
    }
  }

  // Order is load-bearing: papyrus overwrites foliage, foliage skips cells trees
  // already filled.
  stampTrees(chunk, n, seed, ver);
  stampFoliage(chunk, n, seed, ver);
  if (ver >= 2) stampPapyrus(chunk, n, seed, ver);
  // Last, and unconditionally on top: a dungeon is masonry, so it overwrites
  // whatever the terrain passes decided was there rather than negotiating with it.
  // Gated at v5, and purely additive — every branch above is untouched, so a world
  // on v4 comes out of this function byte for byte what it always did.
  if (ver >= 5) stampDungeons(chunk, n, seed, ver);
  chunk.generated = true;
}

void dungeonChestsIn(int cx, int cz, const NoiseSet& n, std::uint32_t seed, int ver,
                     std::vector<DungeonChest>& out) {
  out.clear();
  if (ver < 5) return;
  int x0 = 0, x1 = 0, z0 = 0, z1 = 0;
  dungeonCellRange(cx, cz, x0, x1, z0, z1);
  const int minX = cx * CX, maxX = minX + CX - 1;
  const int minZ = cz * CZ, maxZ = minZ + CZ - 1;
  for (int cellX = x0; cellX <= x1; ++cellX) {
    for (int cellZ = z0; cellZ <= z1; ++cellZ) {
      const DungeonPlan plan = dungeonPlan(n, seed, cellX, cellZ, ver);
      if (!plan.exists) continue;
      for (int i = 0; i < plan.roomCount; ++i) {
        int wx = 0, wy = 0, wz = 0;
        roomChest(plan.rooms[i], plan.floorY, wx, wy, wz);
        if (wx < minX || wx > maxX || wz < minZ || wz > maxZ) continue;
        out.push_back(DungeonChest{wx, wy, wz, i == plan.altarRoom});
      }
    }
  }
}

bool findDungeon(const NoiseSet& n, std::uint32_t seed, int ver, int nearX, int nearZ,
                 int searchCells, DungeonSite& out) {
  if (ver < 5) return false;
  const auto cellOf = [](int world) {
    return world >= 0 ? world / kDungeonCell : -(((-world) + kDungeonCell - 1) / kDungeonCell);
  };
  const int homeX = cellOf(nearX), homeZ = cellOf(nearZ);
  bool found = false;
  long long best = 0;
  for (int dx = -searchCells; dx <= searchCells; ++dx) {
    for (int dz = -searchCells; dz <= searchCells; ++dz) {
      const DungeonPlan plan = dungeonPlan(n, seed, homeX + dx, homeZ + dz, ver);
      if (!plan.exists) continue;
      const DungeonRoom& hub = plan.rooms[plan.altarRoom];
      const int ax = hub.x + hub.w / 2, az = hub.z + hub.d / 2;
      const long long ddx = ax - nearX, ddz = az - nearZ;
      const long long d2 = ddx * ddx + ddz * ddz;
      if (found && d2 >= best) continue;
      best = d2;
      found = true;
      out.x = ax;
      out.y = plan.floorY;
      out.z = az;
      out.rooms = plan.roomCount;
      out.tunnel = plan.tunnel;
      out.tunnelLen = plan.tunnelLen;
      out.tunnelDx = plan.tunnelDx;
      out.tunnelDz = plan.tunnelDz;
    }
  }
  return found;
}

SurfacePreview surfacePreview(const NoiseSet& n, int wx, int wz, int ver) {
  const int sea = seaLevel(ver);
  const std::uint32_t seed = n.seed();
  const ColumnInfo info = columnInfo(n, wx, wz, ver);
  const int h = info.h;
  if (h < sea) return {"water", h};
  if (ver >= 2 && ravineFloor(n, wx, wz, h, ver)) return {"greystone", 14};

  // Tree canopies: any tree rooted within 2 cells shades this column.
  for (int dx = -2; dx <= 2; ++dx) {
    for (int dz = -2; dz <= 2; ++dz) {
      const int tx = wx + dx, tz = wz + dz;
      const double r = hash2i(seed ^ 0x5eedu, tx, tz);
      if (r >= 0.06) continue;
      const ColumnInfo ti = (dx || dz) ? columnInfo(n, tx, tz, ver) : info;
      if (ti.h <= sea + 1) continue;
      if (ver >= 2 && ravineFloor(n, tx, tz, ti.h, ver)) continue;  // no tree in a ravine

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

  if (h <= sea + 1) return {"sand", h};
  if (ver >= 2 && info.biome == Biome::Desert) return {"sand", h};
  if (ver >= 2 && info.biome == Biome::Snow) return {"snowturf", h};
  return {"turf", h};
}

}  // namespace hr::world

#if defined(_MSC_VER)
#pragma float_control(pop)
#endif
