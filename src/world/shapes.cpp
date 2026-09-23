#include "world/shapes.h"

namespace hr::world {
namespace {

// One texel of a 16-texel tile. Every thickness below is a whole number of these,
// because faces are now textured by position (faceUv): a panel 0.18 thick showed
// 2.88 texels of its edge art, a fraction of a pixel that shimmered as it moved.
// 3/16 shows exactly three.
constexpr float P = 1.0f / 16.0f;

constexpr Box kSlabBottom {0, 0, 0, 1, 0.5f, 1};
constexpr Box kSlabTop {0, 0.5f, 0, 1, 1, 1};   // upper-half slab (meta bit 0)
// What you stand on and bump into: the mattress top. The headboard is taller, but
// clipping on it as you walk over a bed would be a wall at knee height for no gain.
constexpr Box kBedBox {0, 0, 0, 1, 9 * P, 1};
constexpr Box kFullCube {0, 0, 0, 1, 1, 1};

// Door and trapdoor panels, and the ladder's depth off its wall.
constexpr float kPanel = 3 * P;
constexpr float kLadder = 2 * P;

// Which art a bed's boxes wear: the mattress is the block's own (dyeable) faces,
// the rest are BlockDef::parts. See blocks.cpp, where the bed declares them in
// this order.
constexpr std::uint8_t kBedMattress = 0;
constexpr std::uint8_t kBedWood = 1;
constexpr std::uint8_t kBedPillow = 2;

// A box in texels, laid out along a bed: `a` runs from the foot end (0) toward the
// head (16), `c` across it, `y` up. Facing (meta bits 0-1, 0:+x 1:-x 2:+z 3:-z) is
// the direction from foot to head, so one table of boxes serves all four ways a
// bed can be laid.
Box bedBox(int facing, float a0, float a1, float c0, float c1, float y0, float y1,
           std::uint8_t part) {
  const bool alongX = facing < 2;
  const bool positive = facing == 0 || facing == 2;
  float lo = a0 * P, hi = a1 * P;
  if (!positive) {
    lo = 1.0f - a1 * P;
    hi = 1.0f - a0 * P;
  }
  Box b;
  b.y0 = y0 * P;
  b.y1 = y1 * P;
  if (alongX) {
    b.x0 = lo; b.x1 = hi;
    b.z0 = c0 * P; b.z1 = c1 * P;
  } else {
    b.z0 = lo; b.z1 = hi;
    b.x0 = c0 * P; b.x1 = c1 * P;
  }
  b.part = part;
  return b;
}

// A bed is two cells, and each draws its own end: a tall headboard and the pillow at
// the head, a low footboard at the foot, and between them a wooden frame with the
// mattress on it. The boards go to the floor and ARE the legs, so the frame spans
// them with a shadowed gap beneath — the silhouette of a bed rather than of a slab.
//
// Only the mattress wears the block's own faces, so a dye lands on the bedding and
// leaves the wood and the pillow alone. Before this the whole block was one box with
// one texture, and the only way to keep a dyed bed from having dyed legs was to
// paint the legs grey so the dye made them a darker shade of the same colour.
std::vector<Box> bedBoxes(int meta) {
  const int f = meta & 3;
  if (meta & 4) {  // head
    return {bedBox(f, 14, 16, 0, 16, 0, 14, kBedWood),     // headboard
            bedBox(f, 0, 14, 0, 16, 3, 6, kBedWood),       // frame
            bedBox(f, 0, 14, 0, 16, 6, 9, kBedMattress),   // mattress
            bedBox(f, 9, 14, 2, 14, 9, 11, kBedPillow)};   // pillow
  }
  return {bedBox(f, 0, 2, 0, 16, 0, 10, kBedWood),        // footboard
          bedBox(f, 2, 16, 0, 16, 3, 6, kBedWood),        // frame
          bedBox(f, 2, 16, 0, 16, 6, 9, kBedMattress)};   // mattress
}

// The bed in one cell, for an icon, a dropped bed and the one in your hand: both
// boards, so it reads as a whole bed rather than as half of one. Head toward +x,
// which puts the headboard at the back of the icon's three-quarter view.
std::vector<Box> bedDisplayBoxes() {
  return {bedBox(0, 14, 16, 0, 16, 0, 14, kBedWood),
          bedBox(0, 0, 2, 0, 16, 0, 10, kBedWood),
          bedBox(0, 2, 14, 0, 16, 3, 6, kBedWood),
          bedBox(0, 2, 14, 0, 16, 6, 9, kBedMattress),
          bedBox(0, 9, 14, 2, 14, 9, 11, kBedPillow)};
}

// A box from texel coordinates, for the hand-placed shapes below.
constexpr Box texels(float x0, float y0, float z0, float x1, float y1, float z1) {
  return {x0 * P, y0 * P, z0 * P, x1 * P, y1 * P, z1 * P};
}

// Four stones of different sizes, placed by hand so the cluster looks scattered
// rather than gridded.
std::vector<Box> pebbleBoxes() {
  return {texels(3, 0, 4, 7, 2, 7), texels(9, 0, 9, 12, 2, 13), texels(10, 0, 3, 12, 1, 5),
          texels(4, 0, 10, 6, 1, 12)};
}

// The same cluster at a size an inventory slot can show. At true scale the stones
// are a quarter of a block across and an icon of them is a few grey pixels.
std::vector<Box> pebbleDisplayBoxes() {
  return {texels(1, 0, 2, 8, 4, 8), texels(8, 0, 8, 14, 4, 15), texels(9, 0, 2, 14, 2, 6),
          texels(2, 0, 10, 6, 2, 14)};
}

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
  constexpr float T = kLadder;
  switch (meta & 3) {
    case 0: return {{1 - T, 0, 0, 1, 1, 1}};
    case 1: return {{0, 0, 0, T, 1, 1}};
    case 2: return {{0, 0, 1 - T, 1, 1, 1}};
    default: return {{0, 0, 0, 1, 1, T}};
  }
}

// A painting: a thin slab against the wall it hangs on, and thinner than a ladder
// because it is a picture rather than something to stand on.
std::vector<Box> paintingBoxes(int meta) {
  constexpr float T = P;  // one texel
  switch (meta & 3) {
    case 0: return {{1 - T, 0, 0, 1, 1, 1}};
    case 1: return {{0, 0, 0, T, 1, 1}};
    case 2: return {{0, 0, 1 - T, 1, 1, 1}};
    default: return {{0, 0, 0, 1, 1, T}};
  }
}

std::vector<Box> trapdoorBoxes(int meta) {
  constexpr float T = kPanel;
  if (!(meta & 1)) {
    return {(meta & 2) ? Box {0, 1 - T, 0, 1, 1, 1} : Box {0, 0, 0, 1, T, 1}};
  }
  switch ((meta >> 2) & 3) {
    case 0: return {{1 - T, 0, 0, 1, 1, 1}};
    case 1: return {{0, 0, 0, T, 1, 1}};
    case 2: return {{0, 0, 1 - T, 1, 1, 1}};
    default: return {{0, 0, 0, 1, 1, T}};
  }
}

std::vector<Box> doorBoxes(int meta) {
  constexpr float T = kPanel;
  const int f = (meta >> 2) & 3;
  if (!(meta & 1)) {
    switch (f) {
      case 0: return {{0, 0, 0, T, 1, 1}};
      case 1: return {{1 - T, 0, 0, 1, 1, 1}};
      case 2: return {{0, 0, 0, 1, 1, T}};
      default: return {{0, 0, 1 - T, 1, 1, 1}};
    }
  }
  // Open: swung 90 degrees to the adjacent wall, hinged at the low corner.
  switch (f) {
    case 0: return {{0, 0, 0, 1, 1, T}};
    case 1: return {{0, 0, 1 - T, 1, 1, 1}};
    case 2: return {{1 - T, 0, 0, 1, 1, 1}};
    default: return {{0, 0, 0, T, 1, 1}};
  }
}

// One canonical display pose per kind. Ladder uses 3 so it faces the viewer.
int displayMeta(RenderKind kind) {
  return (kind == RenderKind::Ladder || kind == RenderKind::Painting) ? 3 : 0;
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
    case RenderKind::Painting:
    case RenderKind::Pebbles:
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
    case RenderKind::Bed: return bedBoxes(meta);
    case RenderKind::Painting: return paintingBoxes(meta);
    case RenderKind::Pebbles: return pebbleBoxes();
    default: return {};
  }
}

std::vector<Box> collisionBoxes(RenderKind kind, int meta) {
  // A ladder is climbed from inside it, and a painting is a picture on a wall you
  // should be able to stand against — neither is something to bump into.
  if (kind == RenderKind::Ladder || kind == RenderKind::Painting) return {};
  // Pebbles are not solid, so nothing asks; stated anyway rather than left to a
  // flag set somewhere else, since four ankle-high boxes to trip on would be absurd.
  if (kind == RenderKind::Pebbles) return {};
  // One box for the whole bed, whatever its model is made of. Physics wants the
  // thing you stand on, not the headboard and the pillow as separate obstacles.
  if (kind == RenderKind::Bed) return {kBedBox};
  return renderBoxes(kind, meta);
}

std::vector<Box> displayBoxes(RenderKind kind) {
  if (kind == RenderKind::Bed) return bedDisplayBoxes();
  if (kind == RenderKind::Pebbles) return pebbleDisplayBoxes();
  std::vector<Box> boxes = renderBoxes(kind, displayMeta(kind));
  if (boxes.empty()) return {kFullCube};
  return boxes;
}

void faceUv(int face, float x, float y, float z, float& fu, float& fv) {
  // Each face's corners run in the order the mesher emits them, and for a full cube
  // these land on the tile's corners in exactly the orientation that order always
  // had: +x and -z read left to right along their axis, -x and +z are the same art
  // seen from the other side, and the tops put the tile's top edge toward +z.
  switch (face) {
    case 0: fu = z; fv = 1.0f - y; break;
    case 1: fu = 1.0f - z; fv = 1.0f - y; break;
    case 2: fu = x; fv = 1.0f - z; break;
    case 3: fu = x; fv = z; break;
    case 4: fu = 1.0f - x; fv = 1.0f - y; break;
    default: fu = x; fv = 1.0f - y; break;
  }
}

void rotateUv(int quarterTurns, float& fu, float& fv) {
  for (int i = 0; i < (quarterTurns & 3); ++i) {
    const float u = fu;
    fu = fv;
    fv = 1.0f - u;
  }
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
  if (kind == RenderKind::Painting) {
    // Held by the wall it hangs on, which its meta names outright.
    static constexpr int kWall[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    dx = kWall[meta & 3][0];
    dy = 0;
    dz = kWall[meta & 3][1];
    return;
  }
  if (kind != RenderKind::Cross) return;
  int mx = 0, mz = 0;
  if (!crossMountDir(meta, mx, mz)) return;
  // The wall is behind the lean, not under the flame.
  dx = -mx;
  dy = 0;
  dz = -mz;
}

}  // namespace hr::world
