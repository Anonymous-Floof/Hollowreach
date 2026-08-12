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

  // Growth stages. `stageCount` is 0 for everything that is not a crop, and the
  // index is clamped rather than asserted: metadata comes off disk and off the wire,
  // so a stage of 200 is a thing a hostile or merely old save can contain and must
  // draw as something rather than read past the end of the table.
  int stageCount(BlockId id) const { return stageCount_[id]; }
  const resource::TileRef& stage(BlockId id, int s) const {
    const int n = stageCount_[id];
    const int i = s < 0 ? 0 : (s >= n ? n - 1 : s);
    return stages_[stageBase_[id] + static_cast<std::size_t>(i)];
  }

 private:
  std::vector<resource::TileRef> faces_;  // 6 per block
  std::vector<resource::TileRef> foot_, stem_;
  std::vector<std::uint8_t> hasStem_;
  // Stage tiles are packed end to end with a per-block base, rather than a fixed
  // stride: only eighteen of a hundred and forty-odd blocks have any, and a fixed
  // four-wide row for every block would be mostly empty.
  std::vector<resource::TileRef> stages_;
  std::vector<std::size_t> stageBase_;
  std::vector<int> stageCount_;
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

// Vertical slices of a chunk, for culling. A chunk is a 16 x WH x 16 column, and
// treating it as one indivisible lump was affordable when WH was 128 and terrain
// filled a third of it. At 192, with caverns opening from bedrock to y=84, a
// visible column carries around ninety layers of cave wall that nothing on the
// surface can see -- and it was being submitted twice a frame, once to the
// g-buffer and once to the shadow map.
//
// The split costs nothing to produce: meshChunk walks y outermost, so vertices
// already come out in ascending y and a section is a contiguous RANGE of the same
// vertex array. One VBO per chunk still, one glDrawArrays per visible section.
inline constexpr int kSectionHeight = 32;
inline constexpr int kSections = WH / kSectionHeight;
static_assert(WH % kSectionHeight == 0, "sections must tile the column exactly");

struct MeshSections {
  // first[i] is the vertex offset of section i; count[i] its length. Empty
  // sections have count 0 and are never drawn or tested.
  std::uint32_t first[kSections] {};
  std::uint32_t count[kSections] {};
  // Whether any face in the section carries skylight. This is the exact answer to
  // "can the sun reach this geometry", not an approximation of it: the lighting
  // pass has already flooded skylight down every open column, so a face with sky 0
  // is one the sun provably cannot see, and it cannot cast a shadow anybody can.
  // A section of pure cave wall is therefore skipped by the shadow pass outright.
  bool sunlit[kSections] {};
};

struct MeshResult {
  std::vector<TerrainVertex> opaque;
  std::vector<TerrainVertex> water;
  MeshSections opaqueSections;
  MeshSections waterSections;
};

// `climate` may be null; it is only consulted for tinted blocks, of which there
// are none today.
MeshResult meshChunk(const MeshNeighbourhood& nb, const BlockTileTable& tiles,
                     const resource::ClimateTile* climate = nullptr);

}  // namespace hr::world
