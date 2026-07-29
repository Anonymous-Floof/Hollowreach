#include "game/raycast.h"

#include <cmath>
#include <limits>

namespace hr::game {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

int signOf(float v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

// Distance along the axis to the first cell boundary, as a fraction of a cell.
float boundaryFraction(float origin, int step) {
  return step > 0 ? (std::floor(origin) + 1.0f - origin) : (origin - std::floor(origin));
}

}  // namespace

RayHit raycast(const world::World& world, const Vec3& origin, const Vec3& dir, float maxDist,
               bool hitLiquid) {
  const float o[3] = {origin.x, origin.y, origin.z};
  const float d[3] = {dir.x, dir.y, dir.z};

  int cell[3] = {static_cast<int>(std::floor(o[0])), static_cast<int>(std::floor(o[1])),
                 static_cast<int>(std::floor(o[2]))};
  int step[3] = {signOf(d[0]), signOf(d[1]), signOf(d[2])};

  float tDelta[3], tMax[3];
  for (int a = 0; a < 3; ++a) {
    if (step[a] != 0) {
      tDelta[a] = std::fabs(1.0f / d[a]);
      tMax[a] = boundaryFraction(o[a], step[a]) * tDelta[a];
    } else {
      tDelta[a] = kInf;
      tMax[a] = kInf;
    }
  }

  int normal[3] = {0, 0, 0};
  float t = 0.0f;
  const world::BlockRegistry& reg = world::blocks();

  while (t <= maxDist) {
    const world::BlockId id = world.getBlock(cell[0], cell[1], cell[2]);
    const world::BlockDef& b = reg.def(id);
    if (id != world::kAir && (hitLiquid || b.render != world::RenderKind::Liquid)) {
      RayHit out;
      out.hit = true;
      out.x = cell[0];
      out.y = cell[1];
      out.z = cell[2];
      out.nx = cell[0] + normal[0];
      out.ny = cell[1] + normal[1];
      out.nz = cell[2] + normal[2];
      out.dist = t;
      out.point = {o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t};
      return out;
    }

    // Advance along whichever axis reaches its next boundary first. The tie
    // ordering here (x, then y, then z) is the JS's and decides which face a ray
    // through an exact edge is treated as entering.
    if (tMax[0] < tMax[1] && tMax[0] < tMax[2]) {
      cell[0] += step[0];
      t = tMax[0];
      tMax[0] += tDelta[0];
      normal[0] = -step[0];
      normal[1] = 0;
      normal[2] = 0;
    } else if (tMax[1] < tMax[2]) {
      cell[1] += step[1];
      t = tMax[1];
      tMax[1] += tDelta[1];
      normal[0] = 0;
      normal[1] = -step[1];
      normal[2] = 0;
    } else {
      cell[2] += step[2];
      t = tMax[2];
      tMax[2] += tDelta[2];
      normal[0] = 0;
      normal[1] = 0;
      normal[2] = -step[2];
    }
  }
  return {};
}

}  // namespace hr::game
