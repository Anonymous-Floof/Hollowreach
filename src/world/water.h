// Flowing-water cellular automaton, ported from js/world/water.js.
//
// Water lives in the voxel grid as the `water` block; its *level* is the per-cell
// metadata byte:
//
//   meta == 0            a SOURCE: full, permanent, and the thing everything else
//                        flows out of.
//   meta & kFalling (8)  water descending from above. Rendered full, and spreads
//                        sideways at the bottom as if it were a source.
//   meta & 7  (1..7)     a flowing level. 1 is nearly full, 7 the thinnest film
//                        before it dries up.
//
// The simulation is event-driven: a cell is only re-evaluated when something near
// it changes, so a still ocean costs nothing at all — which is the only reason a
// world full of water can be simulated at this granularity. Each tick processes a
// bounded batch, and the overflow carries into the next one, so a big spill cannot
// stall a frame.
//
// Two forces drive it to a steady state:
//   * recompute — a flowing cell works out the level it *should* hold from its
//                 neighbours, and dries up when nothing feeds it any more. This is
//                 what makes water recede when its source is removed.
//   * spread    — a settled cell pushes water down first and then outward, one
//                 level thinner each step.
//
// Every change reschedules the touched cell's neighbours, so the field converges
// and then goes quiet.
//
// The accumulator is the frame-rate-independent kind the plan called out
// (js/world/water.js:60-66): it subtracts the period rather than resetting to
// zero, and caps the backlog at one tick so a long frame does not try to catch up
// all at once.

#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

namespace hr::world {

class World;

// Bit 3 of a water cell's metadata: this column is descending from above, and so
// behaves as full depth however thin its own level says it is.
inline constexpr int kWaterFalling = 8;
// The thinnest film that still holds water; one step further and the cell dries.
inline constexpr int kWaterMaxLevel = 7;

class WaterSim {
 public:
  explicit WaterSim(World& world) : world_(world) {}

  // Queue a cell for re-evaluation. Deduplicated, so scheduling the same cell
  // fifty times in one batch costs one entry.
  void schedule(int x, int y, int z);
  // A cell and its six neighbours, which is what any block change means to water.
  void onEdit(int x, int y, int z);

  void tick(float dt);

  // Cells waiting to be evaluated. Zero for a world at rest, which is the normal
  // state; the debug overlay shows it because a number that never settles is the
  // signature of a spill that cannot converge.
  std::size_t pending() const { return queue_.size(); }
  void clear();

 private:
  void process(int x, int y, int z);
  // The level this flowing cell should hold, given its neighbours. Returns 0 for a
  // newly-merged source, kDry when nothing feeds it any more.
  int recompute(int x, int y, int z) const;
  // `falling` means the cell reads as full depth — a source, or a column fed from
  // above. `source` separates the two, because only a column has somewhere left to
  // fall to and so has to reach the bottom before it may spread outward.
  void spread(int x, int y, int z, int level, bool falling, bool source);

  static constexpr int kDry = -1;
  // Seconds between batches, and cells per batch. Roughly six updates a second.
  static constexpr float kTick = 0.16f;
  static constexpr std::size_t kMaxUpdates = 1200;

  struct Cell {
    int x, y, z;
  };

  World& world_;
  std::vector<Cell> queue_;
  // The dedupe set. The web build built an "x,y,z" string per lookup; this packs
  // the same three numbers into one integer, with the same 28/28/8 split the block
  // entity keys use. A y of -1 packs to 255, which no real cell can be, so the
  // "one below the bottom" schedule cannot alias a live one.
  std::unordered_set<std::uint64_t> queued_;
  float acc_ = 0.0f;
};

}  // namespace hr::world
