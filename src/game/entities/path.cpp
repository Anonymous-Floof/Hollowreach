#include "game/entities/path.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "game/entities/entity.h"
#include "world/blocks.h"
#include "world/world.h"

namespace hr::game {
namespace {

constexpr float kSqrt2 = 1.41421356237309515f;
constexpr float kPi = 3.14159265358979f;

// Four cardinals then four diagonals. A diagonal carries the indices of the two
// cardinals that must both be open at the same level, so a mob never cuts a corner
// through the gap between two blocks.
struct Dir {
  int dx, dz;
  float cost;
  int needA, needB;  // -1 for the cardinals
};

constexpr Dir kDirs[8] = {
    {1, 0, 1.0f, -1, -1},   {-1, 0, 1.0f, -1, -1},  {0, 1, 1.0f, -1, -1},
    {0, -1, 1.0f, -1, -1},  {1, 1, kSqrt2, 0, 2},   {1, -1, kSqrt2, 0, 3},
    {-1, 1, kSqrt2, 1, 2},  {-1, -1, kSqrt2, 1, 3},
};

bool solidAt(const world::World& world, int x, int y, int z) {
  return world::blocks().solid(world.getBlock(x, y, z));
}

bool waterAt(const world::World& world, int x, int y, int z) {
  return world.getBlock(x, y, z) == world::wk().water;
}

// Can a body of `height` cells stand with its feet at (x, y, z)?
// 0 = no; 1 = yes, on solid ground; 2 = yes, afloat (swimmers only).
int standable(const world::World& world, int x, int y, int z, const PathOptions& o) {
  for (int i = 0; i < o.height; ++i) {
    if (solidAt(world, x, y + i, z)) return 0;
    if (!o.swim && o.avoidWater && waterAt(world, x, y + i, z)) return 0;
  }
  if (solidAt(world, x, y - 1, z)) return 1;
  if (o.swim && (waterAt(world, x, y - 1, z) || waterAt(world, x, y, z))) return 2;
  return 0;
}

// Octile distance on xz plus a vertical term — admissible enough for game paths.
float heuristic(int x, int y, int z, int gx, int gy, int gz) {
  const float dx = static_cast<float>(std::abs(x - gx));
  const float dz = static_cast<float>(std::abs(z - gz));
  return std::max(dx, dz) + (kSqrt2 - 1.0f) * std::min(dx, dz) +
         std::abs(static_cast<float>(y - gy)) * 0.6f;
}

struct Node {
  int x = 0, y = 0, z = 0;
  float g = 0, f = 0;
  int parent = -1;
  bool closed = false;
  StepHint hint = StepHint::None;
};

// Binary min-heap over node indices, keyed on f. Indices rather than pointers
// because the node pool is a vector that reallocates as the search grows.
class Heap {
 public:
  void push(const std::vector<Node>& pool, int index) {
    heap_.push_back(index);
    int i = static_cast<int>(heap_.size()) - 1;
    while (i > 0) {
      const int p = (i - 1) >> 1;
      if (pool[heap_[p]].f <= pool[heap_[i]].f) break;
      std::swap(heap_[p], heap_[i]);
      i = p;
    }
  }

  int pop(const std::vector<Node>& pool) {
    const int top = heap_.front();
    heap_.front() = heap_.back();
    heap_.pop_back();
    int i = 0;
    const int n = static_cast<int>(heap_.size());
    for (;;) {
      const int l = i * 2 + 1, r = l + 1;
      int m = i;
      if (l < n && pool[heap_[l]].f < pool[heap_[m]].f) m = l;
      if (r < n && pool[heap_[r]].f < pool[heap_[m]].f) m = r;
      if (m == i) break;
      std::swap(heap_[m], heap_[i]);
      i = m;
    }
    return top;
  }

  bool empty() const { return heap_.empty(); }

 private:
  std::vector<int> heap_;
};

}  // namespace

bool findPath(const world::World& world, const Vec3& start, const Vec3& goal,
              const PathOptions& o, PathBudget* budget, Path& out) {
  out.found = false;
  out.points.clear();
  out.cost = 0;

  const int sx = static_cast<int>(std::floor(start.x));
  const int sy = static_cast<int>(std::floor(start.y + 0.01f));
  const int sz = static_cast<int>(std::floor(start.z));
  const int gx = static_cast<int>(std::floor(goal.x));
  const int gy = static_cast<int>(std::floor(goal.y + 0.01f));
  const int gz = static_cast<int>(std::floor(goal.z));

  // Snap a slightly-off start — mid-jump, or inside a step — down to real footing.
  int fy = sy;
  if (!standable(world, sx, fy, sz, o)) {
    if (standable(world, sx, sy - 1, sz, o)) fy = sy - 1;
    else if (standable(world, sx, sy + 1, sz, o)) fy = sy + 1;
    else return false;
  }

  int allow = o.maxExpand;
  if (budget) allow = std::min(allow, budget->left);
  if (allow <= 0) return false;

  // Cell key relative to the start, so the search never has to hash a world
  // coordinate. 9 bits of y is more than the world's height.
  const auto key = [&](int x, int y, int z) -> std::uint32_t {
    return (static_cast<std::uint32_t>(x - sx + 256) << 18) |
           (static_cast<std::uint32_t>(z - sz + 256) << 9) |
           static_cast<std::uint32_t>(y - sy + 256);
  };

  std::vector<Node> pool;
  pool.reserve(static_cast<std::size_t>(allow) * 2);
  std::unordered_map<std::uint32_t, int> index;
  index.reserve(static_cast<std::size_t>(allow) * 2);
  Heap open;

  pool.push_back(Node{sx, fy, sz, 0.0f, heuristic(sx, fy, sz, gx, gy, gz), -1, false,
                      StepHint::None});
  index.emplace(key(sx, fy, sz), 0);
  open.push(pool, 0);

  int best = 0;  // closest-to-goal node seen, for partial paths
  float bestH = pool[0].f;
  int goalNode = -1;
  int expanded = 0;

  const auto visit = [&](int x, int y, int z, float g, int parent, StepHint hint) {
    const std::uint32_t k = key(x, y, z);
    auto it = index.find(k);
    int n = it == index.end() ? -1 : it->second;
    if (n >= 0 && pool[n].closed) return;
    if (n >= 0 && pool[n].g <= g) return;
    const float h = heuristic(x, y, z, gx, gy, gz);
    if (n < 0) {
      pool.push_back(Node{x, y, z, g, g + h, parent, false, hint});
      n = static_cast<int>(pool.size()) - 1;
      index.emplace(k, n);
    } else {
      pool[n].g = g;
      pool[n].f = g + h;
      pool[n].parent = parent;
      if (hint != StepHint::None) pool[n].hint = hint;
    }
    open.push(pool, n);
    if (h < bestH) {
      bestH = h;
      best = n;
    }
  };

  while (!open.empty() && expanded < allow) {
    const int ci = open.pop(pool);
    if (pool[ci].closed) continue;
    pool[ci].closed = true;
    ++expanded;

    const Node cur = pool[ci];  // by value: `pool` reallocates inside visit()
    if (cur.x == gx && cur.z == gz && std::abs(cur.y - gy) <= 1) {
      goalNode = ci;
      break;
    }
    // Range guard: an unreachable goal must not flood the whole loaded world.
    if (std::abs(cur.x - sx) > o.maxDist || std::abs(cur.z - sz) > o.maxDist) continue;

    bool openLevel[4] = {false, false, false, false};
    const bool inWater = o.swim && standable(world, cur.x, cur.y, cur.z, o) == 2;

    for (int i = 0; i < 8; ++i) {
      const Dir& d = kDirs[i];
      if (d.needA >= 0 && !(openLevel[d.needA] && openLevel[d.needB])) continue;
      const int nx = cur.x + d.dx, nz = cur.z + d.dz;
      const float stepCost = d.cost * (inWater ? 1.8f : 1.0f);

      const int level = standable(world, nx, cur.y, nz, o);
      if (i < 4) openLevel[i] = level != 0;
      if (level) {
        visit(nx, cur.y, nz, cur.g + stepCost * (level == 2 ? 1.8f : 1.0f), ci, StepHint::None);
        continue;
      }
      if (d.needA >= 0) continue;  // diagonals only move on the level

      // Step up one block: headroom above the current cell plus a standable target.
      if (!solidAt(world, cur.x, cur.y + o.height, cur.z) &&
          standable(world, nx, cur.y + 1, nz, o)) {
        visit(nx, cur.y + 1, nz, cur.g + stepCost + 0.4f, ci, StepHint::Up);
        continue;
      }
      // Drop: a clear column on the far side down to footing within maxFall.
      if (!solidAt(world, nx, cur.y + o.height - 1, nz)) {
        for (int dy = 0; dy <= o.maxFall; ++dy) {
          const int yy = cur.y - dy;
          const int st = standable(world, nx, yy, nz, o);
          if (st) {
            visit(nx, yy, nz, cur.g + stepCost + 0.35f * dy * (st == 2 ? 0.5f : 1.0f), ci,
                  dy > 0 ? StepHint::Down : StepHint::None);
            break;
          }
          if (solidAt(world, nx, yy, nz)) break;  // a wall, not a drop
        }
      }
    }
  }

  if (budget) budget->left -= expanded;

  const int end = goalNode >= 0 ? goalNode : (best != 0 ? best : -1);
  if (end < 0) return true;  // a valid start with nowhere to go: an empty path

  for (int n = end; n >= 0; n = pool[n].parent) {
    out.points.push_back(PathPoint{pool[n].x + 0.5f, static_cast<float>(pool[n].y),
                                   pool[n].z + 0.5f, pool[n].hint});
  }
  std::reverse(out.points.begin(), out.points.end());
  out.found = goalNode >= 0;
  out.cost = pool[end].g;
  return true;
}

PathFollower::Status PathFollower::step(Entity& e, float dt, float speed) {
  if (done()) return Status::Done;
  const PathPoint& wp = points_[index_];
  const float dx = wp.x - e.pos.x;
  const float dz = wp.z - e.pos.z;
  const float dxz = std::sqrt(dx * dx + dz * dz);

  // Reached this waypoint — close on xz and roughly the right level.
  if (dxz < 0.4f && std::fabs(wp.y - e.pos.y) < 1.3f) {
    ++index_;
    stuckT_ = 0.0f;
    lastDistance_ = 1e30f;
    return done() ? Status::Done : Status::Moving;
  }

  const float heading = std::atan2(dz, dx);
  e.vel.x = std::cos(heading) * speed;
  e.vel.z = std::sin(heading) * speed;
  e.yaw = kPi / 2.0f - heading;  // model convention: the head points local +z

  // Hop when the next cell is above us and we are grounded. The manager's
  // auto-step usually handles it; this covers lips the sweep cannot slide over.
  if (e.onGround && wp.y > e.pos.y + 0.6f && dxz < 1.1f) e.vel.y = 8.2f;

  if (dxz > lastDistance_ - 0.02f) {
    stuckT_ += dt;
    if (stuckT_ > 1.2f) return Status::Stuck;
  } else {
    stuckT_ = 0.0f;
    lastDistance_ = dxz;
  }
  return Status::Moving;
}

}  // namespace hr::game
