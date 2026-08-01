// Physical support for blocks that cannot stand on their own.
//
// Mine the dirt under a torch and the torch should fall; flood a meadow and the
// grass should wash away; dig under sand and it should come down. None of that
// happened before this existed — a greeble whose ground was removed simply hung
// in the air.
//
// THE WHOLE DESIGN CONSTRAINT IS SCALE. A generated chunk holds hundreds of
// plants and a loaded world tens of thousands, so anything that walks them
// looking for unsupported ones is dead on arrival. This is event-driven for
// exactly the reason WaterSim is, and shares its shape deliberately:
//
//   * A cell is only examined when something *near it* changed. World::setBlock
//     and World::setWaterCell schedule the edited cell and its six neighbours,
//     and nothing else ever schedules anything.
//   * Chunk generation does not schedule. A freshly generated world — however
//     much grass it contains — starts with an empty queue and costs nothing,
//     which is the same reason a generated ocean never ticks.
//   * Each tick processes a bounded batch and carries the rest, so collapsing a
//     sand cliff cannot stall a frame.
//
// A settled world therefore has an empty queue and this class is free.
//
// The one asymmetry worth knowing: the cell ABOVE an edit matters more than the
// others, because that is where a block loses its support from. It is scheduled
// like the rest; it is called out here because "why does the cell above need
// waking" is the question this file exists to answer.

#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace hr::world {

class World;

class BlockUpdateSim {
 public:
  explicit BlockUpdateSim(World& world) : world_(world) {}

  // Queue a cell for re-evaluation. Deduplicated, so scheduling the same cell
  // fifty times in one batch costs one entry.
  void schedule(int x, int y, int z);
  // A cell and its six neighbours: what any block change means to its
  // surroundings. This is the only entry point the World uses.
  void onEdit(int x, int y, int z);

  void tick(float dt);

  std::size_t pending() const { return queue_.size(); }
  void clear();

 private:
  // Returns true when the cell was changed, which is what schedules its own
  // neighbours in turn and lets a collapse propagate.
  bool process(int x, int y, int z);

  // Sixteen updates a second — quicker than water's six, because a block losing
  // its footing should look immediate rather than considered. The batch cap is
  // the same order as water's for the same reason.
  static constexpr float kTick = 0.0625f;
  static constexpr std::size_t kMaxUpdates = 1024;

  struct Cell {
    int x, y, z;
  };

  World& world_;
  std::vector<Cell> queue_;
  // Same 28/28/8 packing as WaterSim's dedupe set and the block entity keys.
  std::unordered_set<std::uint64_t> queued_;
  float acc_ = 0.0f;
};

}  // namespace hr::world
