// Chunk dimensions and voxel storage, ported from js/world/chunk.js.
//
// A chunk is a full-height column: 16 x 128 x 16, no vertical sections. Block ids
// and both light channels are flat arrays with no palette and no nibble packing —
// 160 KB per chunk, which is what makes copy-on-write cheap enough to hand an
// immutable snapshot to a worker thread (see the threading milestone).
//
// CX == CZ == 16 is assumed by the lighting flood, which decodes coordinates from
// a flat index with shifts and masks. Changing it means revisiting that code.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "world/blocks.h"

namespace hr::world {

inline constexpr int CX = 16;   // chunk width  (x)
inline constexpr int CZ = 16;   // chunk depth  (z)
// World height. Raised from 128 at gen v3 so there is somewhere to put an
// underground: the sea moved from y=46 to y=100 and the surface came with it, so
// the extra 64 blocks all land below sea level. Costs 5 bytes a cell — a chunk's
// voxels, metadata and two light channels go from 160 KB to 240 KB.
inline constexpr int WH = 192;  // world height (y)
inline constexpr int kCellsPerChunk = CX * WH * CZ;  // 32768

// (y * CZ + z) * CX + x — the same layout the save format's edit records use, so
// changing it would invalidate every world.
inline constexpr int localIdx(int x, int y, int z) { return (y * CZ + z) * CX + x; }

// Decoding the other way, used by the lighting flood.
inline constexpr int idxX(int i) { return i & 15; }
inline constexpr int idxZ(int i) { return (i >> 4) & 15; }
inline constexpr int idxY(int i) { return i >> 8; }

// A chunk coordinate pair packed into one integer key. The JS built a "cx,cz"
// string per lookup and needed a memo to hide the cost; a packed 64-bit key needs
// no such thing.
using ChunkKey = std::uint64_t;
inline constexpr ChunkKey chunkKey(int cx, int cz) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) |
         static_cast<std::uint32_t>(cz);
}
inline constexpr int keyCx(ChunkKey k) { return static_cast<std::int32_t>(k >> 32); }
inline constexpr int keyCz(ChunkKey k) { return static_cast<std::int32_t>(k & 0xFFFFFFFF); }

// The mutable voxel payload, split from the chunk's bookkeeping so it can be
// shared immutably with worker threads via shared_ptr<const ChunkData>.
struct ChunkData {
  std::array<BlockId, kCellsPerChunk> voxels {};
  // Per-cell metadata: orientation/state for shaped blocks, plus a "persistent"
  // flag for player-placed leaves so they survive decay.
  std::array<std::uint8_t, kCellsPerChunk> meta {};
  std::array<std::uint8_t, kCellsPerChunk> skylight {};
  std::array<std::uint8_t, kCellsPerChunk> blocklight {};

  BlockId get(int x, int y, int z) const {
    if (y < 0 || y >= WH) return kAir;
    return voxels[localIdx(x, y, z)];
  }
  void set(int x, int y, int z, BlockId v) {
    if (y < 0 || y >= WH) return;
    voxels[localIdx(x, y, z)] = v;
  }
  // Out of range reads full daylight above and darkness below, matching the JS.
  int sky(int x, int y, int z) const {
    if (y < 0 || y >= WH) return 15;
    return skylight[localIdx(x, y, z)];
  }
  int blockLight(int x, int y, int z) const {
    if (y < 0 || y >= WH) return 0;
    return blocklight[localIdx(x, y, z)];
  }
};

// An emitter cell found during the light pass, in world coordinates at the cell
// centre. The deferred renderer gathers nearby ones into its coloured point-light
// list each frame instead of rescanning voxels.
struct Emitter {
  float x = 0, y = 0, z = 0;
  BlockId id = 0;
};

struct Chunk {
  int cx = 0, cz = 0;
  // Shared, and treated as immutable once anything else holds a reference: a
  // worker job captures this pointer and reads it without a lock, so the main
  // thread clones before writing whenever the count says somebody is looking
  // (World::mutableData). A job therefore never sees a torn chunk, and no lock is
  // taken on either side. Cloning costs 160 KB, and only on the first write after
  // a job was handed the chunk — a run of edits in one frame pays for one.
  std::shared_ptr<ChunkData> data = std::make_shared<ChunkData>();

  bool generated = false;
  bool meshDirty = true;
  // Wants its ONE full light pass. Set when the chunk is created, cleared when
  // that pass is submitted, and never set again: after the pass lands, every
  // further change to this chunk's light is incremental and travels by BFS (see
  // lightengine.cpp). A whole-chunk relight is not merely wasteful now, it is
  // wrong — a rebuild-from-zero is seeded from whatever the neighbours hold at
  // submit time, so it can land BELOW light the BFS has already carried in and
  // undo it. There is deliberately no way to ask for a second one.
  bool needsLight = true;
  // Has that pass actually landed? Distinct from `!needsLight`, which is cleared
  // at *submit* — so between submit and install a never-lit chunk claims to be
  // clean while both its light arrays are still all zero. That reads as pitch
  // darkness, which the renderer forgives (the chunk has no mesh yet either) and
  // the monster spawner absolutely must not: it would take a sunlit meadow that
  // had just streamed in for a cave. It is also the flag the incremental passes
  // test before writing anywhere, since a chunk that has not been lit will read
  // its neighbours itself when its turn comes.
  bool lit = false;

  // A job of each kind is either out or not. The dirty flag above is what makes
  // the result usable: it is cleared at submit and set again by anything that
  // invalidates the chunk, so a result that comes back to a dirty chunk is stale
  // by definition and is dropped. That is the entire staleness protocol — no
  // revision counters, no comparing what the job was built from, and it is
  // correct because every path that changes a chunk already had to dirty it.
  bool genInFlight = false;
  bool lightInFlight = false;
  bool meshInFlight = false;

  std::vector<Emitter> emitters;

  int worldX(int x) const { return cx * CX + x; }
  int worldZ(int z) const { return cz * CZ + z; }
};

}  // namespace hr::world
