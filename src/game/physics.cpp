#include "game/physics.h"

#include <cmath>

namespace hr::game {
namespace {

constexpr float EPS = kCollisionEpsilon;

float axisOf(const Vec3& v, int axis) { return axis == 0 ? v.x : (axis == 1 ? v.y : v.z); }
void setAxis(Vec3& v, int axis, float value) {
  if (axis == 0) v.x = value;
  else if (axis == 1) v.y = value;
  else v.z = value;
}

float boxMin(const world::Box& b, int axis) {
  return axis == 0 ? b.x0 : (axis == 1 ? b.y0 : b.z0);
}
float boxMax(const world::Box& b, int axis) {
  return axis == 0 ? b.x1 : (axis == 1 ? b.y1 : b.z1);
}

}  // namespace

bool aabbBlocked(const world::World& world, const Vec3& lo, const Vec3& hi) {
  // The upper bound is nudged inward so a box whose face lies exactly on a cell
  // boundary does not test the cell beyond it.
  const int x0 = static_cast<int>(std::floor(lo.x));
  const int x1 = static_cast<int>(std::floor(hi.x - EPS));
  const int y0 = static_cast<int>(std::floor(lo.y));
  const int y1 = static_cast<int>(std::floor(hi.y - EPS));
  const int z0 = static_cast<int>(std::floor(lo.z));
  const int z1 = static_cast<int>(std::floor(hi.z - EPS));

  for (int x = x0; x <= x1; ++x) {
    for (int y = y0; y <= y1; ++y) {
      for (int z = z0; z <= z1; ++z) {
        // The returned reference points at a scratch buffer reused per call, so it
        // must be consumed before the next query.
        const std::vector<world::Box>& boxes = world.collisionBoxesAt(x, y, z);
        for (const world::Box& b : boxes) {
          if (hi.x > b.x0 && lo.x < b.x1 && hi.y > b.y0 && lo.y < b.y1 && hi.z > b.z0 &&
              lo.z < b.z1) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool bodyOverlaps(const world::World& world, const Body& body) {
  const Vec3 lo {body.pos.x - body.hw, body.pos.y, body.pos.z - body.hw};
  const Vec3 hi {body.pos.x + body.hw, body.pos.y + body.h, body.pos.z + body.hw};
  return aabbBlocked(world, lo, hi);
}

bool sweepAxis(const world::World& world, Body& body, int axis, float delta) {
  if (delta == 0.0f) return false;

  setAxis(body.pos, axis, axisOf(body.pos, axis) + delta);

  Vec3 lo {body.pos.x - body.hw, body.pos.y, body.pos.z - body.hw};
  Vec3 hi {body.pos.x + body.hw, body.pos.y + body.h, body.pos.z + body.hw};

  const int x0 = static_cast<int>(std::floor(lo.x));
  const int x1 = static_cast<int>(std::floor(hi.x - EPS));
  const int y0 = static_cast<int>(std::floor(lo.y));
  const int y1 = static_cast<int>(std::floor(hi.y - EPS));
  const int z0 = static_cast<int>(std::floor(lo.z));
  const int z1 = static_cast<int>(std::floor(hi.z - EPS));

  bool hit = false;
  for (int x = x0; x <= x1; ++x) {
    for (int y = y0; y <= y1; ++y) {
      for (int z = z0; z <= z1; ++z) {
        // Copied, not referenced: snapping below mutates the body and the next
        // collisionBoxesAt call would invalidate the scratch buffer.
        const std::vector<world::Box> boxes = world.collisionBoxesAt(x, y, z);
        for (const world::Box& b : boxes) {
          // Overlap on the two axes we are not resolving, then on this one.
          if (axis != 0 && !(hi.x > b.x0 && lo.x < b.x1)) continue;
          if (axis != 1 && !(hi.y > b.y0 && lo.y < b.y1)) continue;
          if (axis != 2 && !(hi.z > b.z0 && lo.z < b.z1)) continue;
          if (!(axisOf(hi, axis) > boxMin(b, axis) && axisOf(lo, axis) < boxMax(b, axis))) {
            continue;
          }

          // Snap to the blocking face. On y the body's origin is its feet, so a
          // downward stop lands the feet on the surface and an upward stop puts the
          // head against the ceiling.
          const float extent = axis == 1 ? body.h : body.hw;
          if (delta > 0) {
            setAxis(body.pos, axis, boxMin(b, axis) - extent - EPS);
          } else {
            setAxis(body.pos, axis,
                    boxMax(b, axis) + (axis == 1 ? 0.0f : body.hw) + EPS);
          }
          hit = true;

          // Keep the extents in step, so a second box in the same sweep is tested
          // against the already-resolved position.
          if (axis == 1) {
            lo.y = body.pos.y;
            hi.y = body.pos.y + body.h;
          } else {
            setAxis(lo, axis, axisOf(body.pos, axis) - body.hw);
            setAxis(hi, axis, axisOf(body.pos, axis) + body.hw);
          }
        }
      }
    }
  }
  return hit;
}

bool stepSweep(const world::World& world, Body& body, int axis, float delta,
               float stepHeight) {
  const bool blocked = sweepAxis(world, body, axis, delta);
  if (!blocked || delta == 0.0f) return blocked;

  const float savedAxis = axisOf(body.pos, axis);
  const float savedY = body.pos.y;

  body.pos.y += stepHeight + EPS;
  if (bodyOverlaps(world, body)) {
    body.pos.y = savedY;  // no headroom to step into
    return true;
  }
  if (!sweepAxis(world, body, axis, delta)) return false;  // stepped up onto the ledge

  // Still blocked at the raised height, so undo the probe entirely.
  setAxis(body.pos, axis, savedAxis);
  body.pos.y = savedY;
  return true;
}

}  // namespace hr::game
