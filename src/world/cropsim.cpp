#include "world/cropsim.h"

#include <vector>

#include "game/blockentities.h"  // blockEntityKey / unpackBlockEntityKey
#include "world/blocks.h"
#include "world/world.h"

namespace hr::world {

void CropSim::tick(float dt) {
  acc_ += dt;
  if (acc_ < kTick) return;
  acc_ = 0.0f;

  const std::vector<game::BlockEntityKey>& cells = world_.cropCells();
  if (cells.empty()) {
    cursor_ = 0;
    return;
  }
  if (cursor_ >= cells.size()) cursor_ = 0;

  const WellKnownBlocks& w = wk();
  const BlockRegistry& reg = blocks();
  const std::size_t count = cells.size() < kMaxPerTick ? cells.size() : kMaxPerTick;

  // Cells whose crop advanced. Collected and applied after the walk, because
  // setBlock/setMeta can reshape the very vector being iterated — a crop that
  // washes away mid-sweep removes itself from the index.
  struct Advance {
    int x, y, z;
    int stage;
  };
  std::vector<Advance> advanced;

  for (std::size_t n = 0; n < count; ++n) {
    if (cursor_ >= cells.size()) cursor_ = 0;
    const game::BlockEntityKey key = cells[cursor_++];
    int x = 0, y = 0, z = 0;
    game::unpackBlockEntityKey(key, x, y, z);

    const BlockId id = world_.getBlock(x, y, z);
    const BlockDef& def = reg.def(id);
    // The index is rebuilt from edits and maintained on setBlock, but a chunk that
    // is not loaded reads as air — so this is a skip, never a reason to drop the
    // entry. Dropping it would mean a farm stopped growing the first time its owner
    // walked away and came back.
    if (def.cropStages <= 0) continue;

    // The soil, read once and used for both what the tile LOOKS like and how fast
    // the crop grows. Two separate readings would be two things to fall out of step.
    const BlockId soil = world_.getBlock(x, y - 1, z);
    const bool tilled = isFarmland(soil);
    const bool rich = isRichFarmland(soil);
    const bool moist = tilled && world_.moistFarmland(x, y - 1, z);

    // Make the ground show its state. Moisture was computed on demand and never
    // drawn, so a watered field looked exactly like a dry one and the doubled growth
    // rate read as folklore. Deferred like the advances below, because setBlock
    // during the walk would edit the index being walked.
    if (tilled) {
      const BlockId want = farmlandFor(rich, moist);
      if (want != soil) soilSwaps_.push_back({x, y - 1, z, want});
    }

    const int stage = cropStageOf(world_.getMeta(x, y, z));
    // Ripe crops stop growing but their soil is still refreshed above — a finished
    // field should not freeze its ground in whatever state it happened to ripen in.
    if (stage >= def.cropStages - 1) continue;

    // xorshift32. Growth is not re-derived from anything, so it does not need the
    // coordinate-hash determinism the generator does — and must not have it, or
    // every crop in a field would ripen on exactly the same sweep.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    const float roll = static_cast<float>(rng_ & 0xFFFFFF) / 16777216.0f;

    // Damp soil roughly doubles the rate, which is the whole reason to dig a farm
    // beside water rather than anywhere convenient.
    float chance = kAdvanceChance;
    if (moist) chance *= kMoistBoost;
    // Fertiliser multiplies the RATE rather than skipping a stage. A skip would make
    // the item a way to not play the system; a multiplier makes it a way to play it
    // faster, and it stacks with damp soil so a well-made farm is worth making.
    if (rich) chance *= kFertiliserBoost;
    // Sky light, not block light: a torch keeps monsters off a field but does not
    // make anything grow, and a crop under a floor should sit there indefinitely
    // rather than ripening in the dark.
    if (world_.getSky(x, y, z) < 9) chance *= 0.25f;

    if (roll < chance) advanced.push_back({x, y, z, stage + 1});
  }

  for (const Advance& a : advanced) {
    // setMeta, not setBlock: only the tile changes, so neither light nor opacity is
    // touched and only the mesh is dirtied.
    world_.setMeta(a.x, a.y, a.z, cropMetaFor(a.stage));
  }
  // setBlock here, not setMeta: these are four different blocks, because a cube's
  // faces are fixed at registry-build time and cannot be chosen per cell.
  for (const SoilSwap& s : soilSwaps_) world_.setBlock(s.x, s.y, s.z, s.id, 0);
  soilSwaps_.clear();
}

}  // namespace hr::world
