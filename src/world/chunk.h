// Chunk dimensions and voxel storage, ported from js/world/chunk.js.
//
// A chunk is a full-height column, 16 x 192 x 16, divided for storage into twelve
// 16 x 16 x 16 BANDS. Each of the four per-cell arrays — block ids, metadata,
// skylight, blocklight — holds each band either as ONE value or as a dense
// 4096-entry buffer, so a band that is all air, all stone, all darkness or all
// daylight costs a few bytes instead of four or eight kilobytes. Most are.
//
// The flat arrays this replaces cost 240 KB per chunk unconditionally, which at
// render distance 12 is 625 chunks and 150 MB of mostly-nothing, and at 16 is
// 255 MB. See Banded below for the arithmetic and the measured result.
//
// "Band" and not "section" on purpose: mesher.h already has sections, which are
// 32 tall and exist for draw culling. These are 16 tall and exist for storage,
// the two do not line up, and calling both the same thing would be an invitation
// to index one with the other.
//
// The split is free rather than a re-indexing, because localIdx puts y slowest:
// 16 y-layers of 256 cells is a contiguous run of 4096 flat indices, so a band is
// `i >> 12` and an offset within it is `i & 4095`. Every existing loop over the
// flat index keeps working and keeps its access order.
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
// the extra 64 blocks all land below sea level. Costs 5 bytes a cell, which when
// every cell was stored took a chunk's voxels, metadata and two light channels
// from 160 KB to 240 KB. Banded storage is what stopped that being charged for
// the two thirds of a column that is a single repeated value.
inline constexpr int WH = 192;  // world height (y)
inline constexpr int kCellsPerChunk = CX * WH * CZ;  // 49152

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

inline constexpr int kBandHeight = 16;                       // y layers per band
inline constexpr int kCellsPerBand = CX * kBandHeight * CZ;  // 4096
inline constexpr int kBandsPerChunk = WH / kBandHeight;      // 12
inline constexpr int kBandShift = 12;                        // band of cell i = i >> 12
inline constexpr int kBandMask = kCellsPerBand - 1;          // offset in it = i & 4095

static_assert(WH % kBandHeight == 0, "the world height must divide into bands");
static_assert(kCellsPerBand == 1 << kBandShift, "the shift must match the band size");
static_assert(kBandsPerChunk * kCellsPerBand == kCellsPerChunk, "bands must tile a chunk");
// The property the whole scheme rests on: because localIdx runs y slowest, the
// cells of one band are a contiguous run of flat indices. If this ever stops
// holding, `i >> kBandShift` silently addresses the wrong band.
static_assert(localIdx(0, kBandHeight, 0) == kCellsPerBand,
              "a band must be a contiguous run of the flat index");

// One per-cell value for a whole chunk column, stored a band at a time.
//
// A band is either UNIFORM — one value, no buffer — or DENSE, a full 4096
// entries. That is the whole representation: no palette, no bit packing. It is
// chosen for what the data actually looks like rather than for how clever it
// could be. Metadata and blocklight are zero almost everywhere; skylight is 15
// above the terrain and 0 well below it; a chunk's upper bands are usually all
// air. Those are the bulk, and uniform storage takes them to nothing. What is
// left is genuinely mixed, and a palette over its four or five block ids would
// save a few more kilobytes in exchange for an indirection in the mesher's and
// the lighting flood's innermost loops — measure before believing that trade,
// and see the note beside bytes().
//
// Copyable by value, because copy-on-write hands a whole ChunkData to a worker
// and clones it on the next write.
template <typename T>
class Banded {
 public:
  Banded() = default;
  explicit Banded(T value) {
    for (Band& s : bands_) s.uniform = value;
  }

  T get(int i) const {
    const Band& s = bands_[i >> kBandShift];
    return s.cells.empty() ? s.uniform : s.cells[i & kBandMask];
  }

  void set(int i, T value) {
    Band& s = bands_[i >> kBandShift];
    if (s.cells.empty()) {
      // Writing a band's own value back into it is extremely common — every air
      // cell worldgen skips, every cell the water sim rewrites unchanged — and
      // must not be what turns a free band into four kilobytes.
      if (s.uniform == value) return;
      s.cells.assign(kCellsPerBand, s.uniform);
    }
    s.cells[i & kBandMask] = value;
  }

  void fill(T value) {
    for (Band& s : bands_) {
      s.cells.clear();
      s.cells.shrink_to_fit();
      s.uniform = value;
    }
  }

  // Collapses any dense band whose cells all agree back to uniform. Worth doing
  // after a pass that writes the whole column and then discovers most of it came
  // out the same — worldgen does — and not worth doing after a single edit,
  // which is why it is not called from set().
  void compact() {
    for (Band& s : bands_) {
      if (s.cells.empty()) continue;
      const T first = s.cells[0];
      bool same = true;
      for (const T v : s.cells) {
        if (!(v == first)) {
          same = false;
          break;
        }
      }
      if (!same) continue;
      s.uniform = first;
      s.cells.clear();
      s.cells.shrink_to_fit();
    }
  }

  // Rebuilds the whole column from a flat buffer of kCellsPerChunk, keeping only
  // the bands that are not all one value. The counterpart of copyTo, and the way
  // back for anything that works flat for speed and stores banded for size.
  void loadFrom(const T* src) {
    for (int k = 0; k < kBandsPerChunk; ++k) {
      Band& s = bands_[k];
      const T* p = src + static_cast<std::size_t>(k) * kCellsPerBand;
      bool same = true;
      for (int i = 1; i < kCellsPerBand; ++i) {
        if (!(p[i] == p[0])) {
          same = false;
          break;
        }
      }
      if (same) {
        s.uniform = p[0];
        s.cells.clear();
        s.cells.shrink_to_fit();
      } else {
        s.cells.assign(p, p + kCellsPerBand);
      }
    }
  }

  // Every cell, in flat index order, into a caller's buffer of kCellsPerChunk.
  // For the things that need one contiguous run: hashing, and the golden dump.
  void copyTo(T* out) const {
    for (int k = 0; k < kBandsPerChunk; ++k) {
      const Band& s = bands_[k];
      T* dst = out + static_cast<std::size_t>(k) * kCellsPerBand;
      if (s.cells.empty()) {
        for (int i = 0; i < kCellsPerBand; ++i) dst[i] = s.uniform;
      } else {
        for (int i = 0; i < kCellsPerBand; ++i) dst[i] = s.cells[i];
      }
    }
  }

  // Band-level access, for a caller that wants to walk a whole band without
  // paying the uniform test per cell. Null means the band is uniform.
  const T* dense(int band) const {
    const Band& s = bands_[band];
    return s.cells.empty() ? nullptr : s.cells.data();
  }
  T uniformValue(int band) const { return bands_[band].uniform; }

  // What this column actually costs, buffers only. The point of the exercise, and
  // the number to look at before deciding a palette is worth an indirection.
  std::size_t bytes() const {
    std::size_t n = 0;
    for (const Band& s : bands_) n += s.cells.capacity() * sizeof(T);
    return n;
  }

  bool operator==(const Banded& other) const {
    for (int i = 0; i < kCellsPerChunk; ++i) {
      if (!(get(i) == other.get(i))) return false;
    }
    return true;
  }
  bool operator!=(const Banded& other) const { return !(*this == other); }

 private:
  struct Band {
    T uniform {};
    std::vector<T> cells;  // empty, or exactly kCellsPerBand
  };
  std::array<Band, kBandsPerChunk> bands_;
};

// The mutable voxel payload, split from the chunk's bookkeeping so it can be
// shared immutably with worker threads via shared_ptr<const ChunkData>.
struct ChunkData {
  Banded<BlockId> voxels;
  // Per-cell metadata: orientation/state for shaped blocks, plus a "persistent"
  // flag for player-placed leaves so they survive decay.
  Banded<std::uint8_t> meta;
  Banded<std::uint8_t> skylight;
  Banded<std::uint8_t> blocklight;

  // Collapses every array back to uniform wherever it can. Cheap enough to run at
  // the end of a worldgen or lighting job, on the worker that produced it.
  void compact() {
    voxels.compact();
    meta.compact();
    skylight.compact();
    blocklight.compact();
  }
  std::size_t bytes() const {
    return voxels.bytes() + meta.bytes() + skylight.bytes() + blocklight.bytes();
  }

  BlockId get(int x, int y, int z) const {
    if (y < 0 || y >= WH) return kAir;
    return voxels.get(localIdx(x, y, z));
  }
  void set(int x, int y, int z, BlockId v) {
    if (y < 0 || y >= WH) return;
    voxels.set(localIdx(x, y, z), v);
  }
  // Out of range reads full daylight above and darkness below, matching the JS.
  int sky(int x, int y, int z) const {
    if (y < 0 || y >= WH) return 15;
    return skylight.get(localIdx(x, y, z));
  }
  int blockLight(int x, int y, int z) const {
    if (y < 0 || y >= WH) return 0;
    return blocklight.get(localIdx(x, y, z));
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
  // taken on either side. A clone costs whatever the chunk actually occupies —
  // see ChunkData::bytes, typically a fraction of the old flat 240 KB — and only
  // on the first write after a job was handed the chunk, so a run of edits in one
  // frame pays for one.
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
