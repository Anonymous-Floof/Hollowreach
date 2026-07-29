// Per-chunk lighting, ported from js/world/lighting.js.
//
// Skylight and block light, both breadth-first flood fills so light spreads
// horizontally into tunnels and cave mouths rather than only straight down. Values
// are baked into the mesh, which is why day and night only scale skylight in the
// shader and never force a remesh — and equally why relighting a chunk has to
// remesh its neighbours.
//
// This is deliberately an approximation, carried over as-is: lighting is chunk
// local with one-cell neighbour seeding, so light crosses a chunk border by one
// step per relight, and each pass rebuilds from zero rather than incrementally
// un-lighting. Editing a block dirties the touched border neighbours too, so local
// edits re-converge over a few frames.

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

}  // namespace hr::world
