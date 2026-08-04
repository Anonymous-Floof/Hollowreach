// Per-chunk lighting, ported from js/world/lighting.js.
//
// Skylight and block light, both breadth-first flood fills so light spreads
// horizontally into tunnels and cave mouths rather than only straight down. Values
// are baked into the mesh, which is why day and night only scale skylight in the
// shader and never force a remesh — and equally why relighting a chunk has to
// remesh its neighbours.
//
// What is here is the FIRST pass only: a chunk gets exactly one of these, on a
// worker, when it has just been generated. It rebuilds from zero, which is the
// only thing you can do for a chunk that has no light yet, and it seeds its border
// from whatever its neighbours are storing at the time.
//
// Everything after that first pass is incremental and lives in lightengine.cpp:
// an edit, or a seam between two chunks that have both now been lit, is carried by
// a breadth-first add/remove across chunk borders rather than by rebuilding
// anybody. That split is what makes the whole system converge instead of iterate —
// see the header comment there.

#pragma once

#include <vector>

#include "world/chunk.h"

namespace hr::world {

// The nine light-array sources a relight reads. Same shape as the mesher's
// neighbourhood; a null entry is an unloaded chunk, which contributes nothing.
struct LightNeighbourhood {
  const ChunkData* grid[9] {};
  int cx = 0, cz = 0;

  const ChunkData* centre() const { return grid[4]; }
};

// Recomputes both channels for the centre chunk in place and refills `emitters`.
// Writes only to the centre chunk's arrays.
void computeLight(ChunkData& target, const LightNeighbourhood& nb,
                  std::vector<Emitter>& emitters);

// One cell on an incremental light queue, in WORLD coordinates — the whole point
// of the incremental passes is that they cross chunk borders, so a local index
// would not reach. `level` is what the cell held when it was queued: the add pass
// re-reads and spreads it, the remove pass uses it to tell light that came from
// the cell being darkened apart from light that was always someone else's.
struct LightNode {
  int x = 0, y = 0, z = 0;
  std::uint8_t level = 0;
};

// The two channels, as an index into the queue pairs in World. Named because
// `true` and `false` at a call site say nothing about which one is which.
inline constexpr int kSkyChannel = 0;
inline constexpr int kBlockChannel = 1;

}  // namespace hr::world
