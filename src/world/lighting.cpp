#include "world/lighting.h"

#include <array>
#include <cstdint>

namespace hr::world {
namespace {

// Flat direction tables, so the BFS inner loop has no per-iteration structure.
constexpr int kDirX[6] = {1, -1, 0, 0, 0, 0};
constexpr int kDirY[6] = {0, 0, 1, 0, 0, -1};
constexpr int kDirZ[6] = {0, 0, 0, 1, -1, 0};

// A flat index-based FIFO. Queue entries are localIdx values, so a neighbour is a
// plain offset: +/-1 in x, +/-16 in z, +/-256 in y. The edge guards decode
// coordinates with shifts, which is only valid because CX == CZ == 16.
using Queue = std::vector<std::int32_t>;

// Drains `queue`, spreading level - 1 into non-opaque neighbours within the chunk.
void flood(Queue& queue, std::array<std::uint8_t, kCellsPerChunk>& level,
           const std::array<BlockId, kCellsPerChunk>& voxels) {
  const BlockRegistry& reg = blocks();
  std::size_t head = 0;
  while (head < queue.size()) {
    const std::int32_t i = queue[head++];
    const int cur = level[i];
    if (cur <= 1) continue;
    const std::uint8_t nv = static_cast<std::uint8_t>(cur - 1);
    const int x = idxX(i), z = idxZ(i), y = idxY(i);

    auto spread = [&](std::int32_t ni) {
      if (reg.opaque(voxels[ni])) return;
      if (level[ni] >= nv) return;
      level[ni] = nv;
      queue.push_back(ni);
    };

    if (x < 15) spread(i + 1);
    if (x > 0) spread(i - 1);
    if (z < 15) spread(i + 16);
    if (z > 0) spread(i - 16);
    if (y < WH - 1) spread(i + 256);
    if (y > 0) spread(i - 256);
  }
}

// Raises border cells from already-lit neighbour chunks. `aboveValue` is what cells
// above the world contribute: full daylight for sky, nothing for block light.
void seedBorders(const std::array<BlockId, kCellsPerChunk>& voxels,
                 std::array<std::uint8_t, kCellsPerChunk>& level, Queue& queue,
                 const LightNeighbourhood& nb, bool skyChannel, int aboveValue) {
  const BlockRegistry& reg = blocks();

  auto sample = [&](int lx, int ly, int lz) -> int {
    if (ly < 0) return 0;
    if (ly >= WH) return aboveValue;
    int gx = 1, gz = 1;
    if (lx < 0) gx = 0;
    else if (lx >= CX) gx = 2;
    if (lz < 0) gz = 0;
    else if (lz >= CZ) gz = 2;
    const ChunkData* c = nb.grid[gz * 3 + gx];
    if (!c) return 0;  // an unloaded neighbour contributes nothing
    const int i = localIdx((lx + CX) & 15, ly, (lz + CZ) & 15);
    return skyChannel ? c->skylight[i] : c->blocklight[i];
  };

  auto seed = [&](int x, int y, int z) {
    const int i = localIdx(x, y, z);
    if (reg.opaque(voxels[i])) return;
    int best = 0;
    for (int d = 0; d < 6; ++d) {
      const int nl = sample(x + kDirX[d], y + kDirY[d], z + kDirZ[d]) - 1;
      if (nl > best) best = nl;
    }
    if (best > level[i]) {
      level[i] = static_cast<std::uint8_t>(best);
      queue.push_back(i);
    }
  };

  for (int y = 0; y < WH; ++y) {
    for (int x = 0; x < CX; ++x) {
      seed(x, y, 0);
      seed(x, y, CZ - 1);
    }
    for (int z = 0; z < CZ; ++z) {
      seed(0, y, z);
      seed(CX - 1, y, z);
    }
  }
}

}  // namespace

void computeLight(ChunkData& target, const LightNeighbourhood& nb,
                  std::vector<Emitter>& emitters) {
  const BlockRegistry& reg = blocks();
  const auto& voxels = target.voxels;

  target.skylight.fill(0);
  target.blocklight.fill(0);

  // ---- skylight: the sun pours straight down at full strength, then floods ----
  Queue skyQueue;
  skyQueue.reserve(4096);
  for (int z = 0; z < CZ; ++z) {
    for (int x = 0; x < CX; ++x) {
      bool lit = true;
      for (int y = WH - 1; y >= 0; --y) {
        const int i = localIdx(x, y, z);
        if (lit && reg.opaque(voxels[i])) lit = false;
        if (lit) {
          target.skylight[i] = 15;
          skyQueue.push_back(i);
        }
      }
    }
  }
  seedBorders(voxels, target.skylight, skyQueue, nb, /*skyChannel=*/true, 15);
  flood(skyQueue, target.skylight, voxels);

  // ---- block light: emitters plus neighbour seeding ----
  Queue blockQueue;
  emitters.clear();
  const int baseX = nb.cx * CX;
  const int baseZ = nb.cz * CZ;
  // A linear scan: (y, z, x) iteration order *is* localIdx order, so walk the array
  // directly and decode coordinates only for the rare emitter hit.
  for (int i = 0; i < kCellsPerChunk; ++i) {
    const int level = reg.emit(voxels[i]);
    if (level <= 0) continue;
    target.blocklight[i] = static_cast<std::uint8_t>(level);
    blockQueue.push_back(i);
    emitters.push_back({static_cast<float>(baseX + idxX(i)) + 0.5f,
                        static_cast<float>(idxY(i)) + 0.5f,
                        static_cast<float>(baseZ + idxZ(i)) + 0.5f, voxels[i]});
  }
  seedBorders(voxels, target.blocklight, blockQueue, nb, /*skyChannel=*/false, 0);
  flood(blockQueue, target.blocklight, voxels);
}

}  // namespace hr::world
