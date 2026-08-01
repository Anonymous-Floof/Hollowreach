#include "world/shapes.h"

namespace hr::world {
namespace {

constexpr Box kSlabBottom {0, 0, 0, 1, 0.5f, 1};
constexpr Box kSlabTop {0, 0.5f, 0, 1, 1, 1};   // upper-half slab (meta bit 0)
constexpr Box kBedBox {0, 0, 0, 1, 0.56f, 1};   // a low slab you can lie or stand on
constexpr Box kFullCube {0, 0, 0, 1, 1, 1};

// Stairs: the slab half plus the raised step opposite it. Bit 2 flips it over.
std::vector<Box> stairBoxes(int meta) {
  const int f = meta & 3;
  const int top = (meta >> 2) & 1;
  const Box slab = top ? kSlabTop : kSlabBottom;
  const float sy0 = top ? 0.0f : 0.5f;  // the step sits opposite the slab
  const float sy1 = top ? 0.5f : 1.0f;
  Box step;
  switch (f) {
    case 0: step = {0.5f, sy0, 0, 1, sy1, 1}; break;
    case 1: step = {0, sy0, 0, 0.5f, sy1, 1}; break;
    case 2: step = {0, sy0, 0.5f, 1, sy1, 1}; break;
    default: step = {0, sy0, 0, 1, sy1, 0.5f}; break;
  }
  return {slab, step};
}

// Vertical slab: half a block along one horizontal axis, full height.
std::vector<Box> vslabBoxes(int meta) {
  switch (meta & 3) {
    case 0: return {{0, 0, 0, 0.5f, 1, 1}};   // -x half
    case 1: return {{0.5f, 0, 0, 1, 1, 1}};   // +x half
    case 2: return {{0, 0, 0, 1, 1, 0.5f}};   // -z half
    default: return {{0, 0, 0.5f, 1, 1, 1}};  // +z half
  }
}

std::vector<Box> ladderBoxes(int meta) {
  constexpr float T = 0.1f;
  switch (meta & 3) {
    case 0: return {{1 - T, 0, 0, 1, 1, 1}};
    case 1: return {{0, 0, 0, T, 1, 1}};
    case 2: return {{0, 0, 1 - T, 1, 1, 1}};
    default: return {{0, 0, 0, 1, 1, T}};
  }
}

std::vector<Box> trapdoorBoxes(int meta) {
  if (!(meta & 1)) {
    return {(meta & 2) ? Box {0, 0.82f, 0, 1, 1, 1} : Box {0, 0, 0, 1, 0.18f, 1}};
  }
  switch ((meta >> 2) & 3) {
    case 0: return {{0.82f, 0, 0, 1, 1, 1}};
    case 1: return {{0, 0, 0, 0.18f, 1, 1}};
    case 2: return {{0, 0, 0.82f, 1, 1, 1}};
    default: return {{0, 0, 0, 1, 1, 0.18f}};
  }
}

std::vector<Box> doorBoxes(int meta) {
  const int f = (meta >> 2) & 3;
  if (!(meta & 1)) {
    switch (f) {
      case 0: return {{0, 0, 0, 0.18f, 1, 1}};
      case 1: return {{0.82f, 0, 0, 1, 1, 1}};
      case 2: return {{0, 0, 0, 1, 1, 0.18f}};
      default: return {{0, 0, 0.82f, 1, 1, 1}};
    }
  }
  // Open: swung 90 degrees to the adjacent wall, hinged at the low corner.
  switch (f) {
    case 0: return {{0, 0, 0, 1, 1, 0.18f}};
    case 1: return {{0, 0, 0.82f, 1, 1, 1}};
    case 2: return {{0.82f, 0, 0, 1, 1, 1}};
    default: return {{0, 0, 0, 0.18f, 1, 1}};
  }
}

// One canonical display pose per kind. Ladder uses 3 so it faces the viewer.
int displayMeta(RenderKind kind) {
  return kind == RenderKind::Ladder ? 3 : 0;
}

}  // namespace

bool isShaped(RenderKind kind) {
  switch (kind) {
    case RenderKind::Stair:
    case RenderKind::Slab:
    case RenderKind::VSlab:
    case RenderKind::Ladder:
    case RenderKind::Trapdoor:
    case RenderKind::Door:
    case RenderKind::Bed:
      return true;
    default:
      return false;
  }
}

std::vector<Box> renderBoxes(RenderKind kind, int meta) {
  switch (kind) {
    case RenderKind::Stair: return stairBoxes(meta);
    case RenderKind::Slab: return {(meta & 1) ? kSlabTop : kSlabBottom};
    case RenderKind::VSlab: return vslabBoxes(meta);
    case RenderKind::Ladder: return ladderBoxes(meta);
    case RenderKind::Trapdoor: return trapdoorBoxes(meta);
    case RenderKind::Door: return doorBoxes(meta);
    case RenderKind::Bed: return {kBedBox};
    default: return {};
  }
}

std::vector<Box> collisionBoxes(RenderKind kind, int meta) {
  if (kind == RenderKind::Ladder) return {};
  return renderBoxes(kind, meta);
}

std::vector<Box> displayBoxes(RenderKind kind) {
  std::vector<Box> boxes = renderBoxes(kind, displayMeta(kind));
  if (boxes.empty()) return {kFullCube};
  return boxes;
}

bool crossMountDir(int meta, int& dx, int& dz) {
  // Index 0 is the floor mount and is never returned; it is present so the table
  // can be indexed by the meta directly.
  static constexpr int kMount[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  dx = 0;
  dz = 0;
  const int m = meta & 7;
  if (m < 1 || m > 4) return false;
  dx = kMount[m][0];
  dz = kMount[m][1];
  return true;
}

void supportOffset(RenderKind kind, int meta, int& dx, int& dy, int& dz) {
  dx = 0;
  dy = -1;
  dz = 0;
  if (kind != RenderKind::Cross) return;
  int mx = 0, mz = 0;
  if (!crossMountDir(meta, mx, mz)) return;
  // The wall is behind the lean, not under the flame.
  dx = -mx;
  dy = 0;
  dz = -mz;
}

}  // namespace hr::world
