#include "world/water.h"

#include <algorithm>

#include "world/blocks.h"
#include "world/world.h"

namespace hr::world {
namespace {

constexpr int kHDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

std::uint64_t cellKey(int x, int y, int z) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x) & 0xFFFFFFFu) << 36) |
         (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & 0xFFFFFFFu) << 8) |
         static_cast<std::uint64_t>(static_cast<std::uint32_t>(y) & 0xFFu);
}

}  // namespace

void WaterSim::schedule(int x, int y, int z) {
  if (!queued_.insert(cellKey(x, y, z)).second) return;
  queue_.push_back(Cell{x, y, z});
}

void WaterSim::onEdit(int x, int y, int z) {
  schedule(x, y, z);
  schedule(x + 1, y, z);
  schedule(x - 1, y, z);
  schedule(x, y + 1, z);
  schedule(x, y - 1, z);
  schedule(x, y, z + 1);
  schedule(x, y, z - 1);
}

void WaterSim::clear() {
  queue_.clear();
  queued_.clear();
  acc_ = 0.0f;
}

void WaterSim::tick(float dt) {
  acc_ += dt;
  if (acc_ < kTick) return;
  // Subtract the period rather than resetting to zero — resetting makes the real
  // rate depend on where the frame boundary happened to fall — and cap the backlog
  // at one tick, so a long stall does not then try to run ten batches at once.
  acc_ = std::min(acc_ - kTick, kTick);

  std::vector<Cell> batch;
  batch.swap(queue_);
  queued_.clear();

  const std::size_t limit = std::min(batch.size(), kMaxUpdates);
  for (std::size_t i = 0; i < limit; ++i) process(batch[i].x, batch[i].y, batch[i].z);
  // The overflow carries into the next batch rather than being processed now.
  for (std::size_t i = limit; i < batch.size(); ++i) {
    schedule(batch[i].x, batch[i].y, batch[i].z);
  }
}

void WaterSim::process(int x, int y, int z) {
  const BlockId water = wk().water;
  if (world_.getBlock(x, y, z) != water) return;  // only water cells own behaviour

  const int meta = world_.getMeta(x, y, z);
  const bool source = meta == 0;

  if (!source) {
    const int next = recompute(x, y, z);
    if (next == kDry) {
      world_.setWaterCell(x, y, z, kAir, 0);
      return;
    }
    if (next != meta) {
      world_.setWaterCell(x, y, z, water, next);
      return;
    }
  }
  // Settled — a source, or flowing at the level it should be. Push water onward.
  const bool falling = source || (meta & kWaterFalling) != 0;
  const int level = source ? 0 : (meta & 7);
  spread(x, y, z, level, falling, source);
}

int WaterSim::recompute(int x, int y, int z) const {
  const BlockId water = wk().water;
  if (world_.getBlock(x, y + 1, z) == water) return kWaterFalling;  // fed from above

  int minAdj = 99;
  int sourceCount = 0;
  for (const auto& d : kHDirs) {
    if (world_.getBlock(x + d[0], y, z + d[1]) != water) continue;
    const int nm = world_.getMeta(x + d[0], y, z + d[1]);
    // A source or a falling column acts as full depth however its level reads.
    const int nLevel = (nm == 0 || (nm & kWaterFalling)) ? 0 : (nm & 7);
    if (nm == 0) ++sourceCount;
    if (nLevel + 1 < minAdj) minAdj = nLevel + 1;
  }
  // Two adjacent sources over solid ground merge into a new one — this is the
  // "infinite water" a player digs a two-by-one hole for.
  if (sourceCount >= 2 && blocks().solid(world_.getBlock(x, y - 1, z))) return 0;
  if (minAdj > kWaterMaxLevel) return kDry;
  return minAdj;
}

void WaterSim::spread(int x, int y, int z, int level, bool falling, bool source) {
  const BlockId water = wk().water;

  // 1) Descend. Water prefers to fall, but only into a cell that can take more of
  // it: open air, or a shallow flowing cell. Full water below — a source, or a
  // column already falling — can accept none, and falling through to the sideways
  // case is what lets a sea-level source creep onto the adjacent shore.
  // Washaway lives here rather than in BlockUpdateSim, and the distinction
  // matters: a torch breaks when water FLOWS INTO its cell, not when water
  // happens to be next to it. Testing adjacency instead would strip the grass off
  // every generated shoreline the first time anything nearby was edited, because
  // a settled ocean is adjacent to a great deal of grass and is never going
  // anywhere. Only the two places water actually advances call this.
  const auto clearFragile = [&](int cx, int cy, int cz) {
    const BlockId id = world_.getBlock(cx, cy, cz);
    if (id == kAir || id == water) return id == kAir;
    if (!blocks().def(id).washesAway) return false;
    world_.breakBlockInto(cx, cy, cz);
    return true;
  };

  const BlockId below = world_.getBlock(x, y - 1, z);
  if (below == kAir || clearFragile(x, y - 1, z)) {
    world_.setWaterCell(x, y - 1, z, water, kWaterFalling);
    return;
  }
  if (below == water) {
    const int bm = world_.getMeta(x, y - 1, z);
    if (bm != 0 && bm != kWaterFalling) {
      // A shallow flowing cell: deepen it into a full column.
      world_.setWaterCell(x, y - 1, z, water, kWaterFalling);
      return;
    }
  }

  // 2) Spread horizontally, one level thinner — but only once the water has
  // somewhere to rest. Water goes down before it goes out, and everything standing
  // over a fall belongs to the fall.
  //
  // Two ways a cell can still be on its way down. The water beneath it is itself a
  // descending column, which is true of a source poured over a ledge the moment its
  // first cell has dropped — that one fanned the source out across the top of its
  // own waterfall, and every cell of the fan started a fresh column of its own.
  if (below == water && (world_.getMeta(x, y - 1, z) & kWaterFalling) != 0) return;
  // Or this cell is part of such a column and has not reached the bottom: it is
  // standing on water rather than on ground.
  const bool onGround = below != kAir && below != water;
  if (falling && !source && !onGround) return;
  //
  // Together these are what stops a waterfall pushing outward at full strength from
  // every height at once — which it could not recover from, because each cell it
  // reached then had water directly above it, and recompute() promotes that to a
  // full column, so it spread at full strength too. Nothing thinned, so nothing
  // dried. A bucket emptied off a twenty-block wall covered ninety-six thousand
  // cells and took seventeen hundred ticks to settle, against a hundred and
  // thirteen cells for the same bucket poured out on flat ground.

  if (!falling && level >= kWaterMaxLevel) return;  // too thin to continue
  const int next = (falling ? 1 : level + 1) & 7;
  for (const auto& d : kHDirs) {
    const int nx = x + d[0], nz = z + d[1];
    const BlockId id = world_.getBlock(nx, y, nz);
    if (id == kAir || clearFragile(nx, y, nz)) {
      world_.setWaterCell(nx, y, nz, water, next);
    } else if (id == water) {
      const int nm = world_.getMeta(nx, y, nz);
      // Deepen a thinner flowing neighbour; never touch a source or a falling one.
      if (nm != 0 && !(nm & kWaterFalling) && (nm & 7) > next) {
        world_.setWaterCell(nx, y, nz, water, next);
      }
    }
  }
}

}  // namespace hr::world
