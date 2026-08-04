// Incremental lighting: everything after a chunk's one full pass.
//
// lighting.cpp rebuilds a chunk from zero. That is the right answer exactly once,
// when a chunk has just been generated and has no light at all, and the wrong
// answer every other time: a torch costs 49152 cells times two channels, and
// because a rebuild seeds its border from whatever the neighbours happen to hold
// at submit time, a system built only out of rebuilds is a fixed-point iteration
// over the loaded chunks — one that nothing drives, converges at one cell per
// round, and can land BELOW a value a neighbour already has.
//
// So the rule this file exists to enforce: a chunk is rebuilt once, and after that
// its light only ever moves by breadth-first search across the live chunks, freely
// over chunk borders. Two passes, the textbook pair:
//
//   add     spread level-1 into any neighbour holding less. Monotone, so it
//           computes the same answer whatever order the seeds arrive in.
//   remove  clear the cells whose light came from a source that has gone,
//           collecting the ones that were lit by something else as it goes, then
//           hand those to the add pass to fill the hole back in.
//
// Two properties fall out of that which the old scheme had to be argued into:
//
//   * ORDER DOES NOT MATTER. The add pass raises cells toward the unique least
//     fixed point of "every cell is at least its brightest neighbour minus one".
//     A full pass never overshoots it — it only ever uses real sources and real
//     neighbour values — so a full pass followed by any sequence of adds lands on
//     the same light whether the world was built on one thread or eight. That is
//     the property the 2.4.0 relight race was violating.
//   * A NEIGHBOUR CANNOT BE OWED LIGHT AND NOT GET IT. When a chunk's first pass
//     lands, seedSeams looks at both sides of all four borders and queues whatever
//     either side can raise in the other. Nothing has to notice a change later.
//
// The one thing to keep straight is that skylight 15 means "the sun reaches this
// cell down an open column" and nothing else can produce it — light loses a level
// per step, so the most a neighbour can hand over is 14. Every column rule below
// rests on that, including the fact that the remove pass can never accidentally
// clear a sunlit cell.

#include <algorithm>
#include <climits>

#include "world/world.h"

namespace hr::world {
namespace {

constexpr int kDx[6] = {1, -1, 0, 0, 0, 0};
constexpr int kDy[6] = {0, 0, 1, -1, 0, 0};
constexpr int kDz[6] = {0, 0, 0, 0, 1, -1};

constexpr int kSunlight = 15;

}  // namespace

// Resolves world cells to the chunk holding them, caching the last one. Reads and
// writes both go through here so that "may light go in this chunk at all" is asked
// in exactly one place.
class World::LightCursor {
 public:
  explicit LightCursor(World& w) : world_(w) {}

  // The chunk a light pass may touch at this column, or null. Null means leave it
  // alone, and covers three different situations that all want the same answer:
  // nothing loaded here, terrain still generating, or generated but not yet lit.
  // The last is the interesting one — a chunk awaiting its first pass will read
  // its neighbours when that pass runs, so pushing values at it now would be
  // overwritten anyway.
  LoadedChunk* chunk(int wx, int wz) {
    const int cx = World::floorDiv16(wx), cz = World::floorDiv16(wz);
    if (cx != cx_ || cz != cz_) {
      cx_ = cx;
      cz_ = cz;
      LoadedChunk* lc = world_.chunkAt(cx, cz);
      lc_ = (lc && lc->chunk.generated && lc->chunk.lit) ? lc : nullptr;
      noted_ = false;
    }
    return lc_;
  }

  // Whether light may not enter a cell. Deliberately true for a cell in a chunk
  // this pass may not touch, so the BFS simply stops at that border rather than
  // needing a separate check at every call site.
  bool blocked(int wx, int wy, int wz) {
    if (wy < 0 || wy >= WH) return true;
    LoadedChunk* lc = chunk(wx, wz);
    if (!lc) return true;
    return blocks().opaque(lc->chunk.data->voxels.get(cell(wx, wy, wz)));
  }

  // Opacity alone, for walking a column that is known to be inside one chunk.
  bool opaque(int wx, int wy, int wz) {
    LoadedChunk* lc = chunk(wx, wz);
    if (!lc) return true;
    return blocks().opaque(lc->chunk.data->voxels.get(cell(wx, wy, wz)));
  }

  int emit(int wx, int wy, int wz) {
    LoadedChunk* lc = chunk(wx, wz);
    if (!lc) return 0;
    return blocks().emit(lc->chunk.data->voxels.get(cell(wx, wy, wz)));
  }

  int level(int channel, int wx, int wy, int wz) {
    if (wy < 0 || wy >= WH) return 0;
    LoadedChunk* lc = chunk(wx, wz);
    if (!lc) return 0;
    const ChunkData& d = *lc->chunk.data;
    const int i = cell(wx, wy, wz);
    return channel == kSkyChannel ? d.skylight.get(i) : d.blocklight.get(i);
  }

  void setLevel(int channel, int wx, int wy, int wz, int value) {
    LoadedChunk* lc = chunk(wx, wz);
    if (!lc) return;
    // Copy-on-write, exactly as an edit would: a mesher or a lighter may be
    // holding this snapshot, and the clone is charged once per chunk per batch
    // rather than once per cell.
    ChunkData& d = world_.mutableData(*lc);
    const int i = cell(wx, wy, wz);
    (channel == kSkyChannel ? d.skylight : d.blocklight)
        .set(i, static_cast<std::uint8_t>(value));
    ++world_.lightWrites_;
    // Noted once per visit to a chunk rather than once per cell. A flood is
    // spatially coherent, so this collapses tens of thousands of hash inserts
    // into a handful.
    if (!noted_) {
      world_.lightTouched_.insert(chunkKey(cx_, cz_));
      noted_ = true;
    }
  }

 private:
  static int cell(int wx, int wy, int wz) { return localIdx(wx & 15, wy, wz & 15); }

  World& world_;
  int cx_ = INT_MIN, cz_ = INT_MIN;
  LoadedChunk* lc_ = nullptr;
  bool noted_ = false;
};

// ---- the two passes ---------------------------------------------------------

void World::runLightQueues() {
  for (int channel = 0; channel < 2; ++channel) {
    // Remove first, and in the same channel: it is what discovers the cells that
    // were lit by somebody else, and it pushes those onto the add queue.
    {
      LightCursor cur(*this);
      std::vector<LightNode>& queue = lightRemove_[channel];
      for (std::size_t head = 0; head < queue.size(); ++head) {
        const LightNode n = queue[head];
        for (int d = 0; d < 6; ++d) {
          const int x = n.x + kDx[d], y = n.y + kDy[d], z = n.z + kDz[d];
          if (y < 0 || y >= WH) continue;
          if (!cur.chunk(x, z)) continue;
          const int held = cur.level(channel, x, y, z);
          if (held == 0) continue;
          if (held < n.level) {
            // Dimmer than what is being taken away, so this is that light, one
            // step further out. Clear it and carry on — unless the cell makes
            // light of its own, which only block light can, in which case it is a
            // source and goes straight back on the add queue.
            const int own = channel == kBlockChannel ? cur.emit(x, y, z) : 0;
            cur.setLevel(channel, x, y, z, own);
            if (own > 0) {
              lightAdd_[channel].push_back({x, y, z, static_cast<std::uint8_t>(own)});
            }
            queue.push_back({x, y, z, static_cast<std::uint8_t>(held)});
          } else {
            // As bright or brighter, so it was never ours to take: it belongs to
            // another source and will light the hole back up.
            lightAdd_[channel].push_back({x, y, z, static_cast<std::uint8_t>(held)});
          }
        }
      }
      queue.clear();
    }

    {
      LightCursor cur(*this);
      std::vector<LightNode>& queue = lightAdd_[channel];
      for (std::size_t head = 0; head < queue.size(); ++head) {
        const LightNode n = queue[head];
        // Re-read rather than trusting the queued value: the cell may have been
        // raised again since, and spreading the higher number now saves a second
        // visit to everything downstream.
        const int here = cur.level(channel, n.x, n.y, n.z);
        if (here <= 1) continue;
        const int give = here - 1;
        for (int d = 0; d < 6; ++d) {
          const int x = n.x + kDx[d], y = n.y + kDy[d], z = n.z + kDz[d];
          if (cur.blocked(x, y, z)) continue;
          if (cur.level(channel, x, y, z) >= give) continue;
          cur.setLevel(channel, x, y, z, give);
          queue.push_back({x, y, z, static_cast<std::uint8_t>(give)});
        }
      }
      queue.clear();
    }
  }
  flushLightTouched();
}

void World::flushLightTouched() {
  for (const ChunkKey key : lightTouched_) {
    const int cx = keyCx(key), cz = keyCz(key);
    // The neighbours too: a border face samples the light of the cell it looks
    // into, which lives in the chunk next door. Skipping this is what used to
    // leave black faces along a seam after a torch went up beside it.
    for (int dz = -1; dz <= 1; ++dz) {
      for (int dx = -1; dx <= 1; ++dx) dirtyMesh(cx + dx, cz + dz);
    }
  }
  lightTouched_.clear();
}

// ---- an edit ----------------------------------------------------------------

void World::relightAfterEdit(int wx, int wy, int wz, BlockId before, BlockId after) {
  const BlockRegistry& reg = blocks();
  const bool wasOpaque = reg.opaque(before), nowOpaque = reg.opaque(after);
  const int wasEmit = reg.emit(before), nowEmit = reg.emit(after);
  // Nothing about the illumination moved. This is the overwhelmingly common case
  // — every block of terrain a player mines that was neither solid-to-light nor a
  // lamp, and every cell the water simulation writes.
  if (wasOpaque == nowOpaque && wasEmit == nowEmit) return;

  LightCursor cur(*this);
  if (!cur.chunk(wx, wz)) return;  // awaiting its first pass; that pass will see it

  // --- block light ---
  {
    const int held = cur.level(kBlockChannel, wx, wy, wz);
    if (held > nowEmit) {
      // Losing light: its lamp went out, or it just turned solid. Whatever it was
      // handing to the neighbourhood has to come back out of the neighbourhood,
      // not merely out of this cell.
      cur.setLevel(kBlockChannel, wx, wy, wz, nowEmit);
      lightRemove_[kBlockChannel].push_back({wx, wy, wz, static_cast<std::uint8_t>(held)});
    }
    if (nowEmit > 0) {
      cur.setLevel(kBlockChannel, wx, wy, wz, std::max(held, nowEmit));
      lightAdd_[kBlockChannel].push_back({wx, wy, wz, static_cast<std::uint8_t>(nowEmit)});
    }
    // A cell that has just become passable has to be filled from around it, which
    // the six neighbours do by spreading into it on the add pass.
    if (!nowOpaque && wasOpaque) {
      for (int d = 0; d < 6; ++d) {
        const int x = wx + kDx[d], y = wy + kDy[d], z = wz + kDz[d];
        if (y < 0 || y >= WH) continue;
        const int n = cur.level(kBlockChannel, x, y, z);
        if (n > 0) lightAdd_[kBlockChannel].push_back({x, y, z, static_cast<std::uint8_t>(n)});
      }
    }
  }

  // --- skylight ---
  //
  // Only opacity can move the sun. An emitter is not opaque and casts no shadow,
  // which is why swapping a torch for a lantern skips all of this.
  if (wasOpaque != nowOpaque) {
    if (nowOpaque) {
      // The column below stops being sunlit, down to whatever was already
      // shadowing it. `== kSunlight` is the whole test: only an open column can
      // hold 15, so this walks exactly the stretch that has just gone into shade
      // and stops of its own accord at the first cell that was lit some other way.
      for (int y = wy; y >= 0; --y) {
        if (cur.level(kSkyChannel, wx, y, wz) != kSunlight) break;
        cur.setLevel(kSkyChannel, wx, y, wz, 0);
        lightRemove_[kSkyChannel].push_back({wx, y, wz, kSunlight});
      }
      // A cell that was only lit from the side loses that as well.
      const int held = cur.level(kSkyChannel, wx, wy, wz);
      if (held > 0) {
        cur.setLevel(kSkyChannel, wx, wy, wz, 0);
        lightRemove_[kSkyChannel].push_back({wx, wy, wz, static_cast<std::uint8_t>(held)});
      }
    } else {
      // Opened up. If nothing above is opaque the sun now falls all the way down
      // this column until something else stops it.
      bool open = true;
      for (int y = wy + 1; y < WH && open; ++y) open = !cur.opaque(wx, y, wz);
      if (open) {
        for (int y = wy; y >= 0; --y) {
          if (cur.opaque(wx, y, wz)) break;
          if (cur.level(kSkyChannel, wx, y, wz) >= kSunlight) break;
          cur.setLevel(kSkyChannel, wx, y, wz, kSunlight);
          lightAdd_[kSkyChannel].push_back({wx, y, wz, kSunlight});
        }
      }
      // Sideways light gets in too, whether or not the column is open.
      for (int d = 0; d < 6; ++d) {
        const int x = wx + kDx[d], y = wy + kDy[d], z = wz + kDz[d];
        if (y < 0 || y >= WH) continue;
        const int n = cur.level(kSkyChannel, x, y, z);
        if (n > 0) lightAdd_[kSkyChannel].push_back({x, y, z, static_cast<std::uint8_t>(n)});
      }
    }
  }

  // The emitter list the deferred renderer draws its point lights from. Kept in
  // step here rather than rebuilt, because rebuilding it was the only remaining
  // reason an edit needed a whole-chunk pass.
  if (wasEmit != nowEmit) {
    LoadedChunk* lc = chunkAt(floorDiv16(wx), floorDiv16(wz));
    if (lc) {
      std::vector<Emitter>& list = lc->chunk.emitters;
      const float ex = static_cast<float>(wx) + 0.5f;
      const float ey = static_cast<float>(wy) + 0.5f;
      const float ez = static_cast<float>(wz) + 0.5f;
      if (wasEmit > 0) {
        list.erase(std::remove_if(list.begin(), list.end(),
                                  [&](const Emitter& e) {
                                    return e.x == ex && e.y == ey && e.z == ez;
                                  }),
                   list.end());
      }
      if (nowEmit > 0) list.push_back({ex, ey, ez, after});
    }
  }

  runLightQueues();
}

// ---- a seam -----------------------------------------------------------------

void World::seedSeams(LoadedChunk& lc) {
  // The four cardinal faces, as {dx, dz, our local coordinate, theirs}.
  static constexpr int kFaces[4][4] = {
      {-1, 0, 0, CX - 1}, {1, 0, CX - 1, 0}, {0, -1, 0, CZ - 1}, {0, 1, CZ - 1, 0}};

  const BlockRegistry& reg = blocks();
  const ChunkData& ours = *lc.chunk.data;

  for (const auto& face : kFaces) {
    const LoadedChunk* n = chunkAt(lc.chunk.cx + face[0], lc.chunk.cz + face[1]);
    // A neighbour still waiting for its own first pass needs nothing from us: it
    // will read these values directly when that pass runs, and it will call this
    // from the other side when it lands.
    if (!n || !n->chunk.generated || !n->chunk.lit) continue;
    const ChunkData& theirs = *n->chunk.data;

    const bool xFace = face[1] == 0;
    const int ourAt = face[2], theirAt = face[3];
    const int ourBaseX = lc.chunk.cx * CX, ourBaseZ = lc.chunk.cz * CZ;
    const int theirBaseX = n->chunk.cx * CX, theirBaseZ = n->chunk.cz * CZ;

    for (int y = 0; y < WH; ++y) {
      for (int t = 0; t < 16; ++t) {
        const int oi = xFace ? localIdx(ourAt, y, t) : localIdx(t, y, ourAt);
        const int ti = xFace ? localIdx(theirAt, y, t) : localIdx(t, y, theirAt);
        const bool oBlocked = reg.opaque(ours.voxels.get(oi));
        const bool tBlocked = reg.opaque(theirs.voxels.get(ti));
        if (oBlocked && tBlocked) continue;

        const int ox = ourBaseX + (xFace ? ourAt : t);
        const int oz = ourBaseZ + (xFace ? t : ourAt);
        const int tx = theirBaseX + (xFace ? theirAt : t);
        const int tz = theirBaseZ + (xFace ? t : theirAt);

        for (int channel = 0; channel < 2; ++channel) {
          const auto& oArr = channel == kSkyChannel ? ours.skylight : ours.blocklight;
          const auto& tArr = channel == kSkyChannel ? theirs.skylight : theirs.blocklight;
          const int a = oArr.get(oi), b = tArr.get(ti);
          // Queue only a side that can actually raise the other. On open ground
          // both sit at full skylight forever, and this is what keeps a world
          // load from queueing a million cells to discover they all agree.
          if (!tBlocked && a > b + 1) {
            lightAdd_[channel].push_back({ox, y, oz, static_cast<std::uint8_t>(a)});
          }
          if (!oBlocked && b > a + 1) {
            lightAdd_[channel].push_back({tx, y, tz, static_cast<std::uint8_t>(b)});
          }
        }
      }
    }
  }
}

}  // namespace hr::world
