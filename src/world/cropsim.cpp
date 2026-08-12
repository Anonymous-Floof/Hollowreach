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

    const int stage = cropStageOf(world_.getMeta(x, y, z));
    if (stage >= def.cropStages - 1) continue;  // already ripe

    // xorshift32. Growth is not re-derived from anything, so it does not need the
    // coordinate-hash determinism the generator does — and must not have it, or
    // every crop in a field would ripen on exactly the same sweep.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    const float roll = static_cast<float>(rng_ & 0xFFFFFF) / 16777216.0f;

    // Damp soil roughly doubles the rate, which is the whole reason to dig a farm
    // beside water rather than anywhere convenient. Read from the block beneath the
    // crop, so re-tilling somewhere better actually helps.
    float chance = kAdvanceChance;
    if (world_.getBlock(x, y - 1, z) == w.farmland && world_.moistFarmland(x, y - 1, z)) {
      chance *= 2.0f;
    }
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
}

}  // namespace hr::world
