// Swept-AABB collision against the voxel world, ported from js/game/physics.js.
//
// One implementation shared by the player and every entity, so there is exactly one
// place collision can be wrong. A body is a feet-origin box: from
// (x - hw, y, z - hw) to (x + hw, y + h, z + hw).
//
// Axes are resolved one at a time — x, then z, then y — which is what makes sliding
// along a wall work without the corner cases a single swept test would need.

#pragma once

#include "core/mat4.h"
#include "world/world.h"

namespace hr::game {

// Any collidable thing. The player and every entity expose this same shape.
struct Body {
  Vec3 pos;         // feet centre
  float hw = 0.3f;  // half width on x and z
  float h = 1.8f;   // height
};

inline constexpr float kCollisionEpsilon = 1e-3f;

// Does the box overlap any solid block box?
bool aabbBlocked(const world::World& world, const Vec3& lo, const Vec3& hi);
bool bodyOverlaps(const world::World& world, const Body& body);

// Moves `body` along one axis (0 = x, 1 = y, 2 = z), resolving against the nearest
// blocking face. Returns true when the move was blocked.
bool sweepAxis(const world::World& world, Body& body, int axis, float delta);

// sweepAxis plus an instant auto-step: when a horizontal move is blocked, lift the
// body up to `stepHeight` and try again, so walking into a stair or a hillside
// climbs it in the same frame with momentum intact. Reverts the probe if there is
// no headroom. Only meaningful for a grounded body on a horizontal axis.
bool stepSweep(const world::World& world, Body& body, int axis, float delta,
               float stepHeight);

}  // namespace hr::game
