// Geometry for non-cube blocks, ported from js/world/shapes.js.
//
// Axis-aligned boxes in block-local [0,1] space. The same boxes drive meshing,
// collision and item display, which is what keeps a placed stair, a dropped stair
// and the stair in your hand all the same shape.
//
// Metadata layout per render kind:
//   Stair    bits 0-1 facing (0:+x 1:-x 2:+z 3:-z), bit 2 upside-down (top half)
//   Slab     bit 0 top half
//   VSlab    bits 0-1 which half (0:-x 1:+x 2:-z 3:+z)
//   Ladder   bits 0-1 the wall it hugs (0:+x 1:-x 2:+z 3:-z)
//   Trapdoor bit 0 open, bit 1 top half, bits 2-3 facing (the open swing wall)
//   Door     bit 0 open, bit 1 upper half, bits 2-3 facing (front)
//   Bed      bits 0-1 facing, bit 2 head

#pragma once

#include <vector>

#include "world/blocks.h"

namespace hr::world {

// min/max corners in [0,1].
struct Box {
  float x0 = 0, y0 = 0, z0 = 0;
  float x1 = 1, y1 = 1, z1 = 1;
};

// True for the render kinds expressed as box lists.
bool isShaped(RenderKind kind);

// Boxes for the mesher. Empty when the kind is not box-shaped.
std::vector<Box> renderBoxes(RenderKind kind, int meta);

// Boxes for physics. Ladders are pass-through: you climb inside them.
std::vector<Box> collisionBoxes(RenderKind kind, int meta);

// The pose a shaped block is *shown* in when it is not placed in the world —
// inventory icons, dropped items, the held viewmodel — so all three read the same.
// Chosen so the shape is obvious from a three-quarter view: stairs step up to the
// right, slabs sit in the bottom half, doors stand closed and tall.
std::vector<Box> displayBoxes(RenderKind kind);

}  // namespace hr::world
