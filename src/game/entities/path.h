// A* over the voxel grid, ported from js/game/entities/ai/path.js.
//
// A node is a STANDING CELL: integer (x, y, z) where the mob's feet would be. A
// cell is standable when the body fits (`height` cells of non-solid) and the block
// under the feet is solid — or water, for swimmers. Moves considered:
//
//   * 4 cardinal walks on the level
//   * 4 diagonals, only when both adjacent cardinals are open (no corner cuts)
//   * step UP one block (the manager's auto-step climbs these)
//   * drop DOWN up to `maxFall`, priced by how far
//   * swim through water when `swim` is set, at 1.8x the cost
//
// The search is BUDGETED: it expands at most `budget.left` nodes, shared across
// every mob in a frame, and returns the best PARTIAL path toward the goal when it
// runs out or the goal is unreachable — so a chasing mob starts moving somewhere
// useful instead of standing still while it thinks.

#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/mat4.h"

namespace hr::world {
class World;
}

namespace hr::game {

struct Entity;

struct PathOptions {
  int height = 2;         // body height in cells (zombie/player sized)
  int maxFall = 3;        // will not path over drops taller than this
  bool swim = false;      // water cells are traversable (slowly)
  bool avoidWater = true; // never enter water; ignored when `swim` is set
  int maxExpand = 640;    // per-call expansion cap, also capped by the budget
  int maxDist = 48;       // give up beyond this straight-line distance from start
};

// A waypoint is a cell centre plus, for the follower, whether the step into it
// went up or down — a lip the auto-step cannot slide over needs a real hop.
enum class StepHint : std::uint8_t { None, Up, Down };

struct PathPoint {
  float x = 0, y = 0, z = 0;
  StepHint hint = StepHint::None;
};

struct Path {
  bool found = false;  // false marks a partial path toward the goal
  std::vector<PathPoint> points;
  float cost = 0;
};

// The shared per-frame expansion allowance. Refilled by AIServices::tick.
struct PathBudget {
  int left = 0;
};

// Returns false only when the START cell itself is invalid; otherwise always at
// least a partial path.
bool findPath(const world::World& world, const Vec3& start, const Vec3& goal,
              const PathOptions& options, PathBudget* budget, Path& out);

// Steers an entity along a path: sets vel x/z toward the next waypoint, hops at
// "up" hints taller than the auto-step, and reports when it is finished or needs a
// replan. Lives inside the entity and is deliberately NOT serialised — a reloaded
// mob simply plans afresh.
class PathFollower {
 public:
  enum class Status : std::uint8_t { Moving, Done, Stuck };

  PathFollower() = default;
  explicit PathFollower(Path path) : points_(std::move(path.points)), found_(path.found) {}

  bool valid() const { return !points_.empty(); }
  bool done() const { return index_ >= static_cast<int>(points_.size()); }
  // Final planned cell, for testing whether the goal has since moved.
  const PathPoint* end() const { return points_.empty() ? nullptr : &points_.back(); }

  Status step(Entity& entity, float dt, float speed);

 private:
  std::vector<PathPoint> points_;
  bool found_ = false;
  int index_ = 1;  // points[0] is the cell the mob is already standing in
  float stuckT_ = 0.0f;
  float lastDistance_ = 1e30f;
};

}  // namespace hr::game
