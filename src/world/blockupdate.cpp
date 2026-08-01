#include "world/blockupdate.h"

#include <algorithm>

#include "world/blocks.h"
#include "world/shapes.h"
#include "world/world.h"

namespace hr::world {
namespace {

std::uint64_t cellKey(int x, int y, int z) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x) & 0xFFFFFFFu) << 36) |
         (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & 0xFFFFFFFu) << 8) |
         static_cast<std::uint64_t>(static_cast<std::uint32_t>(y) & 0xFFu);
}

}  // namespace

void BlockUpdateSim::schedule(int x, int y, int z) {
  if (y < 0 || y >= WH) return;
  if (!queued_.insert(cellKey(x, y, z)).second) return;
  queue_.push_back(Cell{x, y, z});
}

void BlockUpdateSim::onEdit(int x, int y, int z) {
  schedule(x, y, z);
  // Above first, and deliberately: it is the cell that just lost its floor, and
  // ordering it first lets a column collapse in one pass rather than one layer
  // per tick.
  schedule(x, y + 1, z);
  schedule(x, y - 1, z);
  schedule(x + 1, y, z);
  schedule(x - 1, y, z);
  schedule(x, y, z + 1);
  schedule(x, y, z - 1);
}

void BlockUpdateSim::clear() {
  queue_.clear();
  queued_.clear();
  acc_ = 0.0f;
}

void BlockUpdateSim::tick(float dt) {
  acc_ += dt;
  if (acc_ < kTick) return;
  // Subtract rather than reset, and cap the backlog at one tick: the same
  // frame-rate-independent accumulator the water sim uses.
  acc_ = std::min(acc_ - kTick, kTick);

  std::vector<Cell> batch;
  batch.swap(queue_);
  queued_.clear();

  const std::size_t limit = std::min(batch.size(), kMaxUpdates);
  for (std::size_t i = 0; i < limit; ++i) process(batch[i].x, batch[i].y, batch[i].z);
  for (std::size_t i = limit; i < batch.size(); ++i) {
    schedule(batch[i].x, batch[i].y, batch[i].z);
  }
}

bool BlockUpdateSim::process(int x, int y, int z) {
  // A cell in a chunk that is not loaded reads as air, which would look exactly
  // like "your ground is gone" and quietly strip the plants off a neighbour the
  // moment you mined near an unloaded edge. Ask nothing of an absent chunk.
  if (!world_.chunkReady(World::floorDiv16(x), World::floorDiv16(z))) return false;

  const BlockId id = world_.getBlock(x, y, z);
  if (id == kAir) return false;

  const BlockDef& def = blocks().def(id);
  if (!def.needsGround && !def.falls) return false;

  const BlockId below = y > 0 ? world_.getBlock(x, y - 1, z) : wk().bedrock;

  // Lost its support. Which cell that is comes from the block's own metadata: a
  // torch on a wall is held by the wall, and asking the floor about it is what
  // dropped every wall torch in the world the first time this ran.
  if (def.needsGround) {
    int dx = 0, dy = -1, dz = 0;
    supportOffset(def.render, world_.getMeta(x, y, z), dx, dy, dz);
    const int sx = x + dx, sy = y + dy, sz = z + dz;
    // A wall mount reaches sideways, which can be into a chunk that has not
    // arrived — and an absent chunk reads as air, the same trap as above.
    if (!world_.chunkReady(World::floorDiv16(sx), World::floorDiv16(sz))) return false;
    // Solid rather than opaque is the right test: a slab, a stair or a pane of
    // glass all hold a torch up, while water and air do not.
    const BlockId support = sy < 0 ? wk().bedrock : world_.getBlock(sx, sy, sz);
    if (!blocks().solid(support)) {
      world_.breakBlockInto(x, y, z);
      return true;
    }
  }

  // Falls. Air below only. Landing *into* water is what builds a beach, and sand
  // that sank through the sea would erase every shoreline in the world.
  if (def.falls && below == kAir && y > 0) {
    world_.beginFall(x, y, z);
    return true;
  }

  return false;
}

}  // namespace hr::world
