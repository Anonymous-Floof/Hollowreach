// Turns a chunk's voxels into renderable geometry, ported from js/world/mesher.js.
//
// Only faces exposed to air or a transparent neighbour are emitted. Output is two
// interleaved vertex buffers: opaque (which includes cutout geometry, resolved by
// `discard` in the shader) and water (translucent, drawn in a later forward pass).
//
// Two departures from the JS, both deliberate:
//
//  1. The vertex is packed to 24 bytes instead of nine floats (36). Non-indexed at
//     six vertices per quad, that is 144 bytes per quad rather than 216. Light is
//     already quantised to 1/15 and averaged over four cells, and `shade` only has
//     24 distinct values, so byte precision is invisible.
//  2. Every emitter funnels through one `emitQuad`, which takes four corners in
//     ring order. The web build had four separate vertex-pushing paths with two
//     different corner conventions. A single generic emitter is what lets a future
//     Minecraft model loader — with arbitrary, possibly rotated quads — reuse this
//     code instead of adding a fifth path.
//
// A tint channel is present and always white; see resource/colormap.h for why.

#pragma once

#include <cstdint>
#include <vector>

#include "resource/atlas.h"
#include "resource/colormap.h"
#include "world/chunk.h"

namespace hr::world {

// 24 bytes. Attribute layout, matching assets/shaders/terrain.vert:
//   0  aPos    3 x float          world space, as in the JS (no per-chunk matrix)
//   1  aUV     2 x ushort norm    atlas coordinates
//   2  aPack   4 x ubyte          shade, skylight, blocklight, flags
//   3  aTint   4 x ubyte norm     biome tint, white today
struct TerrainVertex {
  float x, y, z;
  std::uint16_t u, v;
  std::uint8_t shade, sky, block, flags;
  std::uint8_t tintR, tintG, tintB, tintA;
};
static_assert(sizeof(TerrainVertex) == 24, "terrain vertex must stay tightly packed");

// flags bits 0-1: atmospheric motion, read by the terrain vertex shader.
inline constexpr std::uint8_t kWaveNone = 0;   // static
inline constexpr std::uint8_t kWaveLeaf = 1;   // leaf sway
inline constexpr std::uint8_t kWaveWater = 2;  // water-surface ripple

// Per-block, per-face atlas lookups resolved once after the atlas is built, so the
// mesher never hashes a ResourceId in its inner loop.
struct BlockTileTable {
  void build(const resource::Atlas& atlas);

  const resource::TileRef& face(BlockId id, int faceDir) const {
    return faces_[static_cast<std::size_t>(id) * 6 + faceDir];
  }
  const resource::TileRef& foot(BlockId id) const { return foot_[id]; }
  const resource::TileRef& stem(BlockId id) const { return stem_[id]; }
  bool hasStem(BlockId id) const { return hasStem_[id] != 0; }

 private:
  std::vector<resource::TileRef> faces_;  // 6 per block
  std::vector<resource::TileRef> foot_, stem_;
  std::vector<std::uint8_t> hasStem_;
};

// The 3x3 chunk neighbourhood the mesher reads. Reach is exactly one cell outside
// the centre chunk — face culling and the smooth-light corner average never look
// further — which is what makes an immutable snapshot of these nine pointers a
// complete and safe input for an off-thread mesh job.
struct MeshNeighbourhood {
  // grid index = (dz + 1) * 3 + (dx + 1); null for a chunk that is not loaded.
  const ChunkData* grid[9] {};
  int cx = 0, cz = 0;

  const ChunkData* centre() const { return grid[4]; }
};

struct MeshResult {
  std::vector<TerrainVertex> opaque;
  std::vector<TerrainVertex> water;
};

// `climate` may be null; it is only consulted for tinted blocks, of which there
// are none today.
MeshResult meshChunk(const MeshNeighbourhood& nb, const BlockTileTable& tiles,
                     const resource::ClimateTile* climate = nullptr);

}  // namespace hr::world
