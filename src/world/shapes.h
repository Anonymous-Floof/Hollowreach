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
//   Painting bits 0-1 the wall it hangs on (0:+x 1:-x 2:+z 3:-z), same as Ladder
//   Cross    0 standing on the floor, 1-4 mounted on a wall and leaning that way
//            into the room (1:+x 2:-x 3:+z 4:-z). Only torches use the wall
//            mounts; a plant is always 0, because its billboard does not lean.

#pragma once

#include <cstdint>
#include <vector>

#include "world/blocks.h"

namespace hr::world {

// min/max corners in [0,1].
//
// `part` says which art a box wears. 0 is the block's own face textures, the only
// thing most shapes need; N > 0 is BlockDef::parts[N - 1], for a model built from
// several materials — a bed is a dyed mattress on an undyed wooden frame, and one
// set of six face textures cannot say that. Physics ignores it.
struct Box {
  float x0 = 0, y0 = 0, z0 = 0;
  float x1 = 1, y1 = 1, z1 = 1;
  std::uint8_t part = 0;
};

// Where on its tile one corner of a box face samples, as fractions of the tile: `fu`
// across and `fv` DOWN from the top edge, both in [0, 1]. `face` is the mesher's
// numbering (0:+x 1:-x 2:+y 3:-y 4:+z 5:-z) and x, y, z the corner in block-local
// [0,1] space.
//
// The fractions are the corner's POSITION in the cell, not its place on the box.
// That is the whole point: a slab shows the bottom half of its tile rather than the
// whole tile squashed into half the height, a stair's step lines up with the slab
// beneath it, and the edge of a door shows the three texels of frame that are
// actually there instead of the full door art crushed into a sliver. For a full
// cube it reduces to the corners of the tile, in the orientation every shaped face
// has always used — so nothing that was a whole block changes.
//
// One function for the three places that draw a shape — the chunk mesher, the
// dropped and held model, and the inventory icon — because the day two of them
// disagree is the day a stair in your hand stops matching the one on the floor.
void faceUv(int face, float x, float y, float z, float& fu, float& fv);

// Turns tile fractions a quarter-turn at a time, about the tile's centre. The bed
// uses it so the pillow end of its top art follows the way the bed was laid; done on
// the fractions rather than by shuffling a face's four corner UVs, because only the
// fractions still mean the right thing on a box smaller than the cell.
void rotateUv(int quarterTurns, float& fu, float& fv);

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

// The direction a wall-mounted Cross block leans into the room, or false when it
// is simply standing on the floor. One table, three readers: the mesher leans the
// billboard along it, placement chooses the meta from the face that was clicked,
// and the support check looks the opposite way, at the wall.
bool crossMountDir(int meta, int& dx, int& dz);

// The cell that physically holds a support-needing block up, as an offset from its
// own. The floor beneath for everything except a torch on a wall, which is held by
// the wall behind it — and reading the floor for one of those is what broke every
// wall torch in the world the moment support checks arrived.
void supportOffset(RenderKind kind, int meta, int& dx, int& dy, int& dz);

}  // namespace hr::world
