// Voxel ray cast (Amanatides & Woo DDA), ported from js/game/raycast.js.
//
// Finds the block the player is looking at and the adjacent cell to place into.

#pragma once

#include "core/mat4.h"
#include "world/world.h"

namespace hr::game {

struct RayHit {
  bool hit = false;
  int x = 0, y = 0, z = 0;     // the block that was struck
  int nx = 0, ny = 0, nz = 0;  // the empty neighbour cell on the near side
  float dist = 0.0f;
  // Contact point on the entered face, so placement can read where on the face
  // the cursor landed — upper vs lower half for slabs and stairs.
  Vec3 point;

  explicit operator bool() const { return hit; }
};

// `hitLiquid` makes the ray stop on water too, which is what bucket scooping and
// pouring need; the ordinary aiming ray passes straight through it.
RayHit raycast(const world::World& world, const Vec3& origin, const Vec3& dir,
               float maxDist = 6.0f, bool hitLiquid = false);

}  // namespace hr::game
