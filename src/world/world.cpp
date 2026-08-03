#include "world/world.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <limits>

#include "core/jobs.h"
#include "core/log.h"
#include "world/shapes.h"

namespace hr::world {
namespace {

double nowMs() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

// "install everything, however long it takes". A large finite number would not do:
// nowMs() counts from an unspecified epoch, so on a machine that has been up long
// enough any constant is already in the past.
constexpr double kNoDeadline = std::numeric_limits<double>::infinity();

}  // namespace

World::World(std::uint32_t seed, int renderDistance, int genVersion)
    : noise_(seed), genVersion_(genVersion), renderDistance_(std::clamp(renderDistance, 2, 32)) {
  log::info("world: seed %u, gen v%d, render distance %d", seed, genVersion_, renderDistance_);
}

World::~World() {
  // Not "cancel outstanding work" — wait for it. The results are dropped either
  // way, but a job that is halfway through pushing one is holding this object's
  // mutex and vectors.
  jobs::system().drain();
}

void World::setRenderDistance(int chunks) {
  const int r = std::clamp(chunks, 2, 32);
  if (r == renderDistance_) return;
  renderDistance_ = r;
  forceScan_ = true;
}

const std::vector<std::pair<int, int>>& World::ringOffsets(int radius) {
  auto it = ringCache_.find(radius);
  if (it != ringCache_.end()) return it->second;

  // Nearest first, clipped to a circle so the loaded area is round rather than a
  // square with far-away corners.
  std::vector<std::pair<int, int>> offsets;
  const double limit = (radius + 0.5) * (radius + 0.5);
  for (int dz = -radius; dz <= radius; ++dz) {
    for (int dx = -radius; dx <= radius; ++dx) {
      if (static_cast<double>(dx) * dx + static_cast<double>(dz) * dz <= limit) {
        offsets.emplace_back(dx, dz);
      }
    }
  }
  std::sort(offsets.begin(), offsets.end(), [](const auto& a, const auto& b) {
    const int da = a.first * a.first + a.second * a.second;
    const int db = b.first * b.first + b.second * b.second;
    if (da != db) return da < db;
    // A stable tiebreak keeps load order reproducible between runs.
    return a < b;
  });
  return ringCache_.emplace(radius, std::move(offsets)).first->second;
}

LoadedChunk* World::chunkAt(int cx, int cz) {
  auto it = chunks_.find(chunkKey(cx, cz));
  return it == chunks_.end() ? nullptr : it->second.get();
}

const LoadedChunk* World::chunkAt(int cx, int cz) const {
  auto it = chunks_.find(chunkKey(cx, cz));
  return it == chunks_.end() ? nullptr : it->second.get();
}

LoadedChunk& World::ensureChunk(int cx, int cz) {
  const ChunkKey key = chunkKey(cx, cz);
  auto it = chunks_.find(key);
  if (it != chunks_.end()) return *it->second;

  auto lc = std::make_unique<LoadedChunk>();
  lc->chunk.cx = cx;
  lc->chunk.cz = cz;
  LoadedChunk& ref = *lc;
  chunks_.emplace(key, std::move(lc));
  return ref;
}

ChunkData& World::mutableData(LoadedChunk& lc) {
  // The copy-on-write test. A count above one means a worker is holding this
  // snapshot, so writing through it would race a mesher or a lighter reading it.
  // The count can only fall behind our back — a worker finishing releases its
  // reference — and cloning when we did not have to is harmless, so this needs no
  // synchronisation of its own.
  //
  // Note what this gets for free: after one clone the count is back to one, so a
  // run of edits inside a single frame pays for exactly one copy no matter how
  // many cells it touches. That is the "defer the swap to the end of a batch"
  // behaviour the plan asked for, obtained by ordering — jobs are only handed out
  // once per update — rather than by an explicit batch API.
  if (lc.chunk.data.use_count() > 1) {
    lc.chunk.data = std::make_shared<ChunkData>(*lc.chunk.data);
  }
  return *lc.chunk.data;
}

const ChunkData* World::dataFor(int cx, int cz) const {
  const LoadedChunk* lc = chunkAt(cx, cz);
  return lc && lc->chunk.generated ? lc->chunk.data.get() : nullptr;
}

bool World::neighboursGenerated(int cx, int cz) const {
  return dataFor(cx + 1, cz) && dataFor(cx - 1, cz) && dataFor(cx, cz + 1) &&
         dataFor(cx, cz - 1);
}

void World::applyEdits(LoadedChunk& lc) {
  // Re-apply any saved player edits on top of freshly generated terrain. This is
  // what makes regeneration from a seed safe: the world is deterministic, and the
  // edits are the only thing that is not.
  auto it = edits_.find(chunkKey(lc.chunk.cx, lc.chunk.cz));
  if (it == edits_.end()) return;
  ChunkData& d = mutableData(lc);
  for (const auto& [index, packed] : it->second) {
    d.voxels[index] = static_cast<BlockId>(packed & 0xFFFF);
    d.meta[index] = static_cast<std::uint8_t>((packed >> 16) & 0xFF);
  }
}

// ---- submitting -------------------------------------------------------------

void World::submitGen(LoadedChunk& lc) {
  lc.chunk.genInFlight = true;
  ++genInFlight_;
  const int cx = lc.chunk.cx;
  const int cz = lc.chunk.cz;
  // The noise set is immutable for the world's lifetime and every sampler on it is
  // const with no cached state, so a reference is all a worker needs — twelve
  // permutation tables do not want copying per job.
  const NoiseSet* noise = &noise_;
  const int ver = genVersion_;
  jobs::system().submit(jobs::Lane::Generate, [this, cx, cz, noise, ver] {
    Chunk scratch;
    scratch.cx = cx;
    scratch.cz = cz;
    generate(scratch, *noise, ver);
    std::lock_guard<std::mutex> lock(resultsMutex_);
    genDone_.push_back(GenResult{chunkKey(cx, cz), std::move(scratch.data)});
  });
}

void World::submitLight(LoadedChunk& lc) {
  lc.chunk.lightDirty = false;
  lc.chunk.lightInFlight = true;
  ++lightInFlight_;

  const int cx = lc.chunk.cx;
  const int cz = lc.chunk.cz;
  // Nine snapshots, held alive by the job for as long as it runs. This is the
  // whole of the job's input: it cannot observe a chunk change underneath it,
  // because a change produces a new ChunkData rather than editing this one.
  auto held = std::make_shared<std::array<std::shared_ptr<const ChunkData>, 9>>();
  for (int dz = -1; dz <= 1; ++dz) {
    for (int dx = -1; dx <= 1; ++dx) {
      const LoadedChunk* n = chunkAt(cx + dx, cz + dz);
      if (n && n->chunk.generated) (*held)[(dz + 1) * 3 + (dx + 1)] = n->chunk.data;
    }
  }
  // The lighter writes into a chunk, so it gets a copy of the centre to write
  // into. Only the two light arrays come back out of it.
  auto working = std::make_shared<ChunkData>(*lc.chunk.data);

  jobs::system().submit(jobs::Lane::Light, [this, cx, cz, held, work = std::move(working)] {
    LightNeighbourhood nb;
    nb.cx = cx;
    nb.cz = cz;
    for (int i = 0; i < 9; ++i) nb.grid[i] = (*held)[i].get();
    nb.grid[4] = work.get();  // the centre is the copy being written

    std::vector<Emitter> emitters;
    computeLight(*work, nb, emitters);

    std::lock_guard<std::mutex> lock(resultsMutex_);
    lightDone_.push_back(LightResult{chunkKey(cx, cz), std::move(work), std::move(emitters)});
  });
}

void World::submitMesh(LoadedChunk& lc) {
  // A headless world has no atlas and so no tile table. Declining here keeps every
  // call site safe; the scan has its own check so it does not offer the same work
  // round after round.
  if (!tiles_) return;
  lc.chunk.meshDirty = false;
  lc.chunk.meshInFlight = true;
  ++meshInFlight_;

  const int cx = lc.chunk.cx;
  const int cz = lc.chunk.cz;
  auto held = std::make_shared<std::array<std::shared_ptr<const ChunkData>, 9>>();
  for (int dz = -1; dz <= 1; ++dz) {
    for (int dx = -1; dx <= 1; ++dx) {
      const LoadedChunk* n = chunkAt(cx + dx, cz + dz);
      if (n && n->chunk.generated) (*held)[(dz + 1) * 3 + (dx + 1)] = n->chunk.data;
    }
  }
  // The tile table is built once with the atlas and never written again.
  const BlockTileTable* tiles = tiles_;

  jobs::system().submit(jobs::Lane::Mesh, [this, cx, cz, held, tiles] {
    MeshNeighbourhood nb;
    nb.cx = cx;
    nb.cz = cz;
    for (int i = 0; i < 9; ++i) nb.grid[i] = (*held)[i].get();

    MeshResult mesh = meshChunk(nb, *tiles);
    std::lock_guard<std::mutex> lock(resultsMutex_);
    meshDone_.push_back(MeshResultSlot{chunkKey(cx, cz), std::move(mesh)});
  });
}

// ---- installing -------------------------------------------------------------

namespace {

// Would relighting the neighbour on one face of this chunk actually change
// anything? `axis` is 0 for an x face and 1 for a z face; `at` is 0 or 15, the
// local coordinate of this chunk's plane, and `mirror` the coordinate of the
// neighbour's touching plane.
//
// Two reasons to say yes, one per direction light can move:
//
//   * this face is now brighter than the neighbour's touching cell by more than
//     the one level a step across the border costs, so the neighbour has light
//     coming to it that it does not have;
//   * this face got *darker*, so whatever the neighbour is holding there may have
//     come from a source that is now gone — the removed torch that keeps glowing
//     through the seam.
//
// Deliberately not a plain "did my border change at all". On open ground both
// sides sit at full skylight and stay there, so a chunk lighting for the first
// time next to an already-lit one has a face that *changed* (0 -> 15) and yet
// gives its neighbour nothing. Answering the sharper question is what keeps the
// initial world load from re-lighting almost every chunk a second time to
// discover it had nothing to do.
//
// The opaque test is not an optimisation, it is what makes this terminate.
// seedBorders refuses to seed an opaque cell, so a wall on the neighbour's side of
// the line can never accept the light being offered — and without this guard the
// offer stands forever: each chunk sees a face brighter than the rock facing it,
// says "you would change", and dirties the other back for eternity. Ask only about
// cells that can actually take the light.
bool neighbourWouldChange(const ChunkData& before, const ChunkData& after,
                          const ChunkData& neighbour, int axis, int at, int mirror) {
  const BlockRegistry& reg = blocks();
  for (int y = 0; y < WH; ++y) {
    for (int t = 0; t < 16; ++t) {
      const int n = axis == 0 ? localIdx(mirror, y, t) : localIdx(t, y, mirror);
      if (reg.opaque(neighbour.voxels[n])) continue;
      const int i = axis == 0 ? localIdx(at, y, t) : localIdx(t, y, at);
      if (after.skylight[i] > neighbour.skylight[n] + 1) return true;
      if (after.blocklight[i] > neighbour.blocklight[n] + 1) return true;
      if (after.skylight[i] < before.skylight[i]) return true;
      if (after.blocklight[i] < before.blocklight[i]) return true;
    }
  }
  return false;
}

}  // namespace

bool World::installResults(double deadlineMs) {
  std::vector<GenResult> gen;
  std::vector<LightResult> light;
  std::vector<MeshResultSlot> mesh;
  {
    std::lock_guard<std::mutex> lock(resultsMutex_);
    gen.swap(genDone_);
    light.swap(lightDone_);
    mesh.swap(meshDone_);
  }
  const bool any = !gen.empty() || !light.empty() || !mesh.empty();

  // Every stage of installing costs the main thread something — a light install is
  // two 32 KB copies over a possible 160 KB clone, a mesh install is a GL upload —
  // and with eight workers feeding one installer the results arrive in batches.
  // So all three stages share the frame's budget and whatever does not fit goes
  // back on the queue rather than being forced through. Nothing is dropped: a
  // result is still valid a frame later, it has simply not been applied yet.
  // Always at least one of each, or a budget of zero would never make progress.
  std::size_t doneGen = 0;
  std::size_t doneLight = 0;
  std::size_t doneMesh = 0;
  const auto outOfTime = [deadlineMs](std::size_t done) {
    return done > 0 && nowMs() >= deadlineMs;
  };

  for (GenResult& r : gen) {
    if (outOfTime(doneGen)) break;
    ++doneGen;
    --genInFlight_;
    LoadedChunk* lc = chunkAt(keyCx(r.key), keyCz(r.key));
    if (!lc) continue;  // unloaded while the job ran
    lc->chunk.genInFlight = false;
    lc->chunk.data = std::move(r.data);
    lc->chunk.generated = true;
    applyEdits(*lc);

    // The Atlas only maps ground the player has actually been near, so generating
    // a chunk is what lifts its fog — and the tile it drew from a seed prediction
    // is now stale, which is what makes a preview tile upgrade to the real thing.
    explored_.insert(r.key);
    mapDirty_.insert(r.key);

    lc->chunk.lightDirty = true;
    lc->chunk.meshDirty = true;
    // A new chunk changes its neighbours' border faces and border light.
    for (int dz = -1; dz <= 1; ++dz) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (!dx && !dz) continue;
        if (LoadedChunk* n = chunkAt(lc->chunk.cx + dx, lc->chunk.cz + dz)) {
          n->chunk.lightDirty = true;
          n->chunk.meshDirty = true;
        }
      }
    }
  }

  for (LightResult& r : light) {
    if (outOfTime(doneLight)) break;
    ++doneLight;
    --lightInFlight_;
    LoadedChunk* lc = chunkAt(keyCx(r.key), keyCz(r.key));
    if (!lc) continue;
    lc->chunk.lightInFlight = false;
    // Dirty again means something invalidated the chunk while the job ran, so what
    // came back describes a world that no longer exists. Drop it; the scan will
    // resubmit.
    if (lc->chunk.lightDirty || !lc->chunk.generated) continue;

    // Only the light arrays, never the whole working copy: a metadata edit (a door
    // opening) does not dirty light, so it can land mid-job and must survive.
    ChunkData& d = mutableData(*lc);

    // Which of the four faces this chunk shows its neighbours changed. Measured
    // before the copy, because `d` is about to stop being the old values.
    //
    // computeLight rebuilds a chunk from zero and seeds its border from whatever
    // its neighbours are storing *at submit time*, which makes the whole system a
    // fixed-point iteration over the loaded chunks — and an iteration nothing was
    // driving. A chunk's light install re-meshed its neighbours (so their faces
    // resampled) but never re-lit them, so light only ever crossed a seam when the
    // two chunks happened to be lit in the right order. A torch a block from a
    // border lit its own chunk and stopped at the line; take that torch away again
    // and the far side kept its glow, because the neighbour was still reading the
    // snapshot from before. Both halves of that are the same missing edge.
    // Only a neighbour that has ALREADY been lit needs asking: one that has not
    // will read these new values when its own first pass runs. And it settles
    // rather than ringing, because light loses a level per cell and a chunk is
    // sixteen wide — a value cannot cross a border, travel the chunk and come back
    // to raise anything it started from.
    auto relightIfNeeded = [&](int dx, int dz, int axis, int at, int mirror) {
      LoadedChunk* n = chunkAt(lc->chunk.cx + dx, lc->chunk.cz + dz);
      if (!n || n->chunk.lightDirty) return;  // already going to be redone
      // A neighbour with a job OUT read a snapshot of this chunk taken before the
      // values being installed here existed, so whatever it returns is answering a
      // question that has since changed. It cannot be compared against — the data
      // it is holding is pre-light — so it is dirtied unconditionally, which makes
      // installResults discard the stale result rather than store it.
      //
      // Skipping these was a real bug and a subtle one: `lit` is false for a first
      // pass in flight, so the old guard treated "has not been lit yet" and "is
      // being lit right now off older data" as the same case. Inline jobs finish at
      // submit and can never be in flight, so the fault appeared only with workers
      // — surfacing as the world hash differing between 0 and 8 threads about once
      // in twenty-five runs.
      if (n->chunk.lightInFlight) {
        n->chunk.lightDirty = true;
        return;
      }
      // Never lit and nothing out: it will read these values when its turn comes.
      if (!n->chunk.lit) return;
      if (neighbourWouldChange(d, *r.working, *n->chunk.data, axis, at, mirror)) {
        n->chunk.lightDirty = true;
      }
    };
    relightIfNeeded(-1, 0, 0, 0, CX - 1);
    relightIfNeeded(1, 0, 0, CX - 1, 0);
    relightIfNeeded(0, -1, 1, 0, CZ - 1);
    relightIfNeeded(0, 1, 1, CZ - 1, 0);

    d.skylight = r.working->skylight;
    d.blocklight = r.working->blocklight;
    lc->chunk.emitters = std::move(r.emitters);
    lc->chunk.meshDirty = true;
    lc->chunk.lit = true;

    // Light is baked into vertices, and a face samples the neighbour cell it looks
    // into — so a relit chunk invalidates its neighbours' meshes as well. Skipping
    // this is what produced black border faces and torches that "needed a nudge".
    for (int dz = -1; dz <= 1; ++dz) {
      for (int dx = -1; dx <= 1; ++dx) {
        if (!dx && !dz) continue;
        if (LoadedChunk* n = chunkAt(lc->chunk.cx + dx, lc->chunk.cz + dz)) {
          if (!n->chunk.lightDirty) n->chunk.meshDirty = true;
        }
      }
    }
  }

  // The only part of this that touches GL.
  for (MeshResultSlot& r : mesh) {
    if (outOfTime(doneMesh)) break;
    ++doneMesh;
    --meshInFlight_;
    LoadedChunk* lc = chunkAt(keyCx(r.key), keyCz(r.key));
    if (!lc) continue;
    lc->chunk.meshInFlight = false;
    if (lc->chunk.meshDirty) continue;  // stale, as above
    lc->opaqueMesh.upload(r.mesh.opaque, r.mesh.opaqueSections);
    lc->waterMesh.upload(r.mesh.water, r.mesh.waterSections);
  }

  // Whatever did not fit goes back at the front, so the oldest results are the
  // next ones tried and nothing can be starved by a steady stream of new arrivals.
  if (doneGen < gen.size() || doneLight < light.size() || doneMesh < mesh.size()) {
    std::lock_guard<std::mutex> lock(resultsMutex_);
    genDone_.insert(genDone_.begin(), std::make_move_iterator(gen.begin() + doneGen),
                    std::make_move_iterator(gen.end()));
    lightDone_.insert(lightDone_.begin(), std::make_move_iterator(light.begin() + doneLight),
                      std::make_move_iterator(light.end()));
    meshDone_.insert(meshDone_.begin(), std::make_move_iterator(mesh.begin() + doneMesh),
                     std::make_move_iterator(mesh.end()));
  }
  return any;
}

int World::scanAndSubmit(int pcx, int pcz, int genBudget, int stageBudget, int maxInFlight) {
  int generated = 0;
  int lit = 0;
  int meshed = 0;
  bool anyMissing = false;

  const std::vector<std::pair<int, int>>& ring = ringOffsets(renderDistance_);
  // Generation reaches one chunk FURTHER than anything is ever lit, meshed or
  // drawn. Lighting and meshing both refuse a chunk whose four cardinal
  // neighbours are not all generated (below), and generation used to stop at the
  // same radius the render distance did — so every chunk on the outer edge of the
  // ring had a neighbour that could never exist, sat permanently lightDirty and
  // meshDirty, and was never meshed at all. That cost three things: the outermost
  // ring of the world had no geometry, its all-zero light arrays were sampled by
  // the ring inside it and drew a dark seam there, and `pendingCount` could never
  // reach zero (68 of 489 chunks at render distance 12, a floor that grew with
  // the perimeter and so looked like a streamer that would not converge).
  // unloadFar already keeps renderDistance_ + 2, so the apron is never unloaded
  // out from under the ring it exists to serve.
  const std::vector<std::pair<int, int>>& apron = ringOffsets(renderDistance_ + 1);

  // --- generation: only scanned when something might be missing --------------
  if (forceScan_) {
    for (const auto& [dx, dz] : apron) {
      const int cx = pcx + dx, cz = pcz + dz;
      if (chunkAt(cx, cz)) continue;
      if (generated >= genBudget || genInFlight_ >= maxInFlight) {
        anyMissing = true;
        break;
      }
      submitGen(ensureChunk(cx, cz));
      ++generated;
    }
    forceScan_ = anyMissing;
  }

  // --- lighting and meshing, nearest first -----------------------------------
  for (const auto& [dx, dz] : ring) {
    if (lit >= stageBudget && meshed >= stageBudget) break;
    LoadedChunk* lc = chunkAt(pcx + dx, pcz + dz);
    if (!lc || !lc->chunk.generated) continue;
    // Lighting reads all four neighbours, so wait until they exist rather than
    // baking a dark border that would need redoing.
    if (!neighboursGenerated(lc->chunk.cx, lc->chunk.cz)) continue;

    if (lc->chunk.lightDirty) {
      if (lc->chunk.lightInFlight || lit >= stageBudget || lightInFlight_ >= maxInFlight) {
        continue;
      }
      submitLight(*lc);
      ++lit;
      continue;  // its mesh waits for the light it is about to get
    }
    if (lc->chunk.lightInFlight) continue;
    // No tile table means no meshes to build — a headless world has one of these
    // and would otherwise be handed the same mesh work every round forever, since
    // submitMesh would decline it without clearing the flag.
    if (tiles_ && lc->chunk.meshDirty) {
      // Meshing gets a tighter allowance than the other two lanes. Its results are
      // the expensive ones to install — a fresh chunk's first upload allocates a
      // VBO, and the driver's allocation is not something a deadline between
      // uploads can interrupt — so letting eight workers finish sixty-four of them
      // into one frame turns a 6 ms budget into a 40 ms spike. Half as many in
      // flight spreads the same total work over more frames.
      if (lc->chunk.meshInFlight || meshed >= stageBudget ||
          meshInFlight_ >= maxInFlight / 4) {
        continue;
      }
      submitMesh(*lc);
      ++meshed;
    }
  }
  return generated + lit + meshed;
}

void World::update(float px, float pz, int maxGen, double buildMs) {
  const int pcx = floorDiv16(static_cast<int>(std::floor(px)));
  const int pcz = floorDiv16(static_cast<int>(std::floor(pz)));
  const bool crossed = pcx != lastPcx_ || pcz != lastPcz_;
  if (crossed) forceScan_ = true;

  const double deadline = nowMs() + buildMs;

  // Everything scales with the pool: one worker's worth of work per frame keeps a
  // single core busy and starves eight.
  const int lanes = jobs::system().threaded() ? jobs::system().threadCount() : 1;
  const int genBudget = maxGen * lanes;
  const int stageBudget = 4 * lanes;
  // In flight, not per frame. A teleport dirties the whole ring at once, and a
  // hundred finished meshes waiting for the GL thread is tens of megabytes of
  // vertex data sitting in a queue.
  const int maxInFlight = 8 * lanes;

  // Install what finished, then hand out what that unblocked. With workers the new
  // jobs land on a later frame and one pass is all there is to do. Without them,
  // submit() ran the jobs inline and their results are already waiting, so the loop
  // keeps going while anything comes back — which is what keeps `--threads 0`
  // converging in a single update the way this function did before the milestone,
  // and is why a screenshot run does not need more frames than it used to.
  int passes = 0;
  for (;;) {
    installResults(deadline);
    scanAndSubmit(pcx, pcz, genBudget, stageBudget, maxInFlight);
    if (jobs::system().threaded() || ++passes >= 3) break;
    std::lock_guard<std::mutex> lock(resultsMutex_);
    if (genDone_.empty() && lightDone_.empty() && meshDone_.empty()) break;
  }

  if (crossed) {
    // Two chunks of hysteresis, so walking back and forth over a boundary does not
    // thrash generation.
    unloadFar(pcx, pcz, renderDistance_ + 2);
    lastPcx_ = pcx;
    lastPcz_ = pcz;
  }
}

void World::waitForIdle(float px, float pz) {
  const int pcx = floorDiv16(static_cast<int>(std::floor(px)));
  const int pcz = floorDiv16(static_cast<int>(std::floor(pz)));
  constexpr int kNoLimit = 1 << 20;

  // A round hands out everything outstanding, waits for it, and installs it.
  // Installing dirties neighbours, so it takes several rounds to settle; the loop
  // ends when a round finds nothing to submit and nothing came back. Bounded
  // rather than a bare `while (true)`, because a bug that leaves a chunk
  // permanently dirty should surface as a warning and not as a hang.
  for (int round = 0; round < 4096; ++round) {
    const int submitted = scanAndSubmit(pcx, pcz, kNoLimit, kNoLimit, kNoLimit);
    jobs::system().drain();
    const bool installed = installResults(kNoDeadline);
    if (submitted == 0 && !installed && jobsInFlight() == 0 && !forceScan_) return;
  }
  log::warn("world: waitForIdle gave up with %d job(s) outstanding", jobsInFlight());
}

void World::primeSpawn(float px, float pz) {
  const int pcx = floorDiv16(static_cast<int>(std::floor(px)));
  const int pcz = floorDiv16(static_cast<int>(std::floor(pz)));

  // A 5x5 so the centre 3x3 all have their four neighbours and can be lit. All of
  // them go out at once and the whole batch is waited on, so this is faster with
  // workers than the sequential version it replaces rather than merely equivalent.
  for (int dz = -2; dz <= 2; ++dz) {
    for (int dx = -2; dx <= 2; ++dx) {
      LoadedChunk& lc = ensureChunk(pcx + dx, pcz + dz);
      if (!lc.chunk.generated && !lc.chunk.genInFlight) submitGen(lc);
    }
  }
  jobs::system().drain();
  installResults(kNoDeadline);

  for (int dz = -1; dz <= 1; ++dz) {
    for (int dx = -1; dx <= 1; ++dx) {
      LoadedChunk* lc = chunkAt(pcx + dx, pcz + dz);
      if (lc && lc->chunk.generated) submitLight(*lc);
    }
  }
  jobs::system().drain();
  installResults(kNoDeadline);

  for (int dz = -1; dz <= 1; ++dz) {
    for (int dx = -1; dx <= 1; ++dx) {
      LoadedChunk* lc = chunkAt(pcx + dx, pcz + dz);
      if (lc && lc->chunk.generated) submitMesh(*lc);
    }
  }
  jobs::system().drain();
  installResults(kNoDeadline);

  forceScan_ = true;
}

void World::unloadFar(int pcx, int pcz, int radius) {
  const int limit = radius * radius;
  for (auto it = chunks_.begin(); it != chunks_.end();) {
    const int dx = keyCx(it->first) - pcx;
    const int dz = keyCz(it->first) - pcz;
    if (dx * dx + dz * dz > limit) {
      // Edits live in `edits_`, not in the chunk, so dropping it loses nothing:
      // regeneration is deterministic and the edits are re-applied.
      it = chunks_.erase(it);
    } else {
      ++it;
    }
  }
}

BlockId World::getBlock(int wx, int wy, int wz) const {
  if (wy < 0 || wy >= WH) return kAir;
  const ChunkData* d = dataFor(floorDiv16(wx), floorDiv16(wz));
  if (!d) return kAir;
  return d->voxels[localIdx(wx & 15, wy, wz & 15)];
}

int World::topSolidY(int wx, int wz) const {
  if (!chunkReady(floorDiv16(wx), floorDiv16(wz))) return -1;
  // From the sky down, so the first hit is the surface rather than the floor of a
  // cave. Stops at 1: bedrock is not somewhere to stand or surface.
  for (int y = WH - 1; y >= 1; --y) {
    if (blocks().solid(getBlock(wx, y, wz))) return y;
  }
  return -1;
}

int World::getMeta(int wx, int wy, int wz) const {
  if (wy < 0 || wy >= WH) return 0;
  const ChunkData* d = dataFor(floorDiv16(wx), floorDiv16(wz));
  if (!d) return 0;
  return d->meta[localIdx(wx & 15, wy, wz & 15)];
}

int World::getSky(int wx, int wy, int wz) const {
  if (wy < 0) return 0;
  if (wy >= WH) return 15;
  const ChunkData* d = dataFor(floorDiv16(wx), floorDiv16(wz));
  if (!d) return 15;
  return d->skylight[localIdx(wx & 15, wy, wz & 15)];
}

int World::getBlockLight(int wx, int wy, int wz) const {
  if (wy < 0 || wy >= WH) return 0;
  const ChunkData* d = dataFor(floorDiv16(wx), floorDiv16(wz));
  if (!d) return 0;
  return d->blocklight[localIdx(wx & 15, wy, wz & 15)];
}

void World::dirtyLight(int cx, int cz) {
  if (LoadedChunk* lc = chunkAt(cx, cz)) lc->chunk.lightDirty = true;
}

void World::dirtyMesh(int cx, int cz) {
  if (LoadedChunk* lc = chunkAt(cx, cz)) lc->chunk.meshDirty = true;
}

void World::setBlock(int wx, int wy, int wz, BlockId id, int meta) {
  if (wy < 0 || wy >= WH) return;
  const int cx = floorDiv16(wx), cz = floorDiv16(wz);
  LoadedChunk* lc = chunkAt(cx, cz);
  if (!lc) return;

  const int lx = wx & 15, lz = wz & 15;
  const int index = localIdx(lx, wy, lz);
  ChunkData& d = mutableData(*lc);

  const BlockId previous = d.voxels[index];
  d.voxels[index] = id;
  d.meta[index] = static_cast<std::uint8_t>(meta & 0xFF);

  edits_[chunkKey(cx, cz)][index] =
      static_cast<std::uint32_t>(id) | (static_cast<std::uint32_t>(meta & 0xFF) << 16);
  mapDirty_.insert(chunkKey(cx, cz));

  const BlockRegistry& reg = blocks();

  // A painting's picture belongs to the block, so it goes when the block does —
  // here rather than beside the two places that clean up block entities on a
  // break, because a canvas can also be washed off a wall by a flood or lose the
  // wall it hung on, and neither of those goes through the mining path. Gated on
  // the OLD block being a canvas so the water simulation, which calls this
  // thousands of times a tick, never touches the map at all.
  if (previous == wk().canvas && id != wk().canvas) removePainting(wx, wy, wz);

  const bool emitterChanged = reg.emit(previous) != reg.emit(id);
  const bool opacityChanged = reg.opaque(previous) != reg.opaque(id);

  if (emitterChanged || opacityChanged) {
    // Block light and skylight both reach across chunk borders, so an emitter or
    // opacity change has to relight the whole neighbourhood, not just this chunk.
    for (int dz = -1; dz <= 1; ++dz) {
      for (int dx = -1; dx <= 1; ++dx) dirtyLight(cx + dx, cz + dz);
    }
  } else {
    dirtyLight(cx, cz);
    // A cell on a border also changes the neighbour's border light.
    if (lx == 0) dirtyLight(cx - 1, cz);
    if (lx == 15) dirtyLight(cx + 1, cz);
    if (lz == 0) dirtyLight(cx, cz - 1);
    if (lz == 15) dirtyLight(cx, cz + 1);
  }

  dirtyMesh(cx, cz);
  if (lx == 0) dirtyMesh(cx - 1, cz);
  if (lx == 15) dirtyMesh(cx + 1, cz);
  if (lz == 0) dirtyMesh(cx, cz - 1);
  if (lz == 15) dirtyMesh(cx, cz + 1);

  water_.onEdit(wx, wy, wz);
  blockUpdates_.onEdit(wx, wy, wz);
  if (editSink_ && !applyingRemote_) editSink_(wx, wy, wz, id, meta & 0xFF);
}

void World::applyRemoteEdit(int wx, int wy, int wz, BlockId id, int meta) {
  // Saved and restored rather than simply cleared, so that if a write ever does
  // reach this from inside another one the outer call keeps its silence instead of
  // having it switched off underneath it.
  const bool was = applyingRemote_;
  applyingRemote_ = true;
  setBlock(wx, wy, wz, id, meta);
  applyingRemote_ = was;
}

// The water simulation's own write. Deliberately not setBlock: it must not relight
// (water is neither opaque nor an emitter, so nothing about the illumination
// changes when it moves) and a batch of a thousand of these must not do a thousand
// neighbourhood relights.
void World::setWaterCell(int wx, int wy, int wz, BlockId id, int meta) {
  if (wy < 0 || wy >= WH) return;
  const int cx = floorDiv16(wx), cz = floorDiv16(wz);
  LoadedChunk* lc = chunkAt(cx, cz);
  if (!lc || !lc->chunk.generated) return;

  const int lx = wx & 15, lz = wz & 15;
  const int index = localIdx(lx, wy, lz);
  const std::uint8_t m = static_cast<std::uint8_t>(meta & 0xFF);
  // A no-op write would still dirty a mesh and reschedule the neighbourhood, which
  // is how a settled pool would keep ticking forever.
  if (lc->chunk.data->voxels[index] == id && lc->chunk.data->meta[index] == m) return;

  ChunkData& d = mutableData(*lc);
  d.voxels[index] = id;
  d.meta[index] = m;

  edits_[chunkKey(cx, cz)][index] =
      static_cast<std::uint32_t>(id) | (static_cast<std::uint32_t>(m) << 16);
  mapDirty_.insert(chunkKey(cx, cz));

  dirtyMesh(cx, cz);
  if (lx == 0) dirtyMesh(cx - 1, cz);
  if (lx == 15) dirtyMesh(cx + 1, cz);
  if (lz == 0) dirtyMesh(cx, cz - 1);
  if (lz == 15) dirtyMesh(cx, cz + 1);

  water_.onEdit(wx, wy, wz);
  if (editSink_ && !applyingRemote_) editSink_(wx, wy, wz, id, m);
}

void World::setMeta(int wx, int wy, int wz, int meta) {
  if (wy < 0 || wy >= WH) return;
  const int cx = floorDiv16(wx), cz = floorDiv16(wz);
  LoadedChunk* lc = chunkAt(cx, cz);
  if (!lc) return;

  const int lx = wx & 15, lz = wz & 15;
  const int index = localIdx(lx, wy, lz);
  ChunkData& d = mutableData(*lc);
  d.meta[index] = static_cast<std::uint8_t>(meta & 0xFF);

  edits_[chunkKey(cx, cz)][index] = static_cast<std::uint32_t>(d.voxels[index]) |
                                    (static_cast<std::uint32_t>(meta & 0xFF) << 16);

  // Only the shape changed, so light is untouched; the mesh (and its neighbours,
  // whose face culling read this cell) still has to be rebuilt.
  mapDirty_.insert(chunkKey(cx, cz));
  dirtyMesh(cx, cz);
  if (lx == 0) dirtyMesh(cx - 1, cz);
  if (lx == 15) dirtyMesh(cx + 1, cz);
  if (lz == 0) dirtyMesh(cx, cz - 1);
  if (lz == 15) dirtyMesh(cx, cz + 1);

  water_.onEdit(wx, wy, wz);
  if (editSink_ && !applyingRemote_) editSink_(wx, wy, wz, d.voxels[index], meta & 0xFF);
}

void World::decayLeavesAround(int wx, int wy, int wz) {
  constexpr int kSupport = 4;               // survives within 4 leaf-steps of a log
  constexpr int kCheckRadius = 5;           // re-check leaves this close to the change
  constexpr int kSearch = kCheckRadius + kSupport;  // logs this far out can still support them
  static constexpr int kNeighbours[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                            {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};

  const BlockRegistry& reg = blocks();
  const auto isLog = [&reg](BlockId id) { return reg.def(id).isLog; };
  const auto isLeaf = [&reg](BlockId id) { return reg.def(id).isLeaf; };

  // Multi-source breadth-first search of "support distance" outward from every
  // nearby log, travelling only through leaves. The visited set reuses the block
  // entity position packer purely as a cell key — the JS built an "x,y,z" string
  // here, and this is the same thing without the allocation.
  struct Node {
    int x, y, z, d;
  };
  std::unordered_map<game::BlockEntityKey, int> dist;
  std::vector<Node> queue;
  for (int dx = -kSearch; dx <= kSearch; ++dx) {
    for (int dy = -kSearch; dy <= kSearch; ++dy) {
      for (int dz = -kSearch; dz <= kSearch; ++dz) {
        const int x = wx + dx, y = wy + dy, z = wz + dz;
        if (!isLog(getBlock(x, y, z))) continue;
        dist[game::blockEntityKey(x, y, z)] = 0;
        queue.push_back({x, y, z, 0});
      }
    }
  }
  for (std::size_t head = 0; head < queue.size(); ++head) {
    const Node n = queue[head];
    if (n.d >= kSupport) continue;
    for (const auto& off : kNeighbours) {
      const int nx = n.x + off[0], ny = n.y + off[1], nz = n.z + off[2];
      const game::BlockEntityKey k = game::blockEntityKey(nx, ny, nz);
      if (dist.count(k)) continue;
      if (!isLeaf(getBlock(nx, ny, nz))) continue;
      dist[k] = n.d + 1;
      queue.push_back({nx, ny, nz, n.d + 1});
    }
  }

  // Dissolve unsupported natural leaves in the inner region.
  for (int dx = -kCheckRadius; dx <= kCheckRadius; ++dx) {
    for (int dy = -kCheckRadius; dy <= kCheckRadius; ++dy) {
      for (int dz = -kCheckRadius; dz <= kCheckRadius; ++dz) {
        const int x = wx + dx, y = wy + dy, z = wz + dz;
        if (!isLeaf(getBlock(x, y, z))) continue;
        if (getMeta(x, y, z) & 1) continue;  // player-placed, so permanent
        if (!dist.count(game::blockEntityKey(x, y, z))) setBlock(x, y, z, kAir, 0);
      }
    }
  }
}

game::BlockEntity* World::getBlockEntity(int wx, int wy, int wz) {
  auto it = blockEntities_.find(game::blockEntityKey(wx, wy, wz));
  return it == blockEntities_.end() ? nullptr : &it->second;
}

game::BlockEntity* World::getOrCreateBlockEntity(int wx, int wy, int wz,
                                                 game::BlockEntityKind kind) {
  if (kind == game::BlockEntityKind::None) return nullptr;
  const game::BlockEntityKey key = game::blockEntityKey(wx, wy, wz);
  auto it = blockEntities_.find(key);
  if (it != blockEntities_.end()) return &it->second;
  return &blockEntities_.emplace(key, game::makeEntity(kind)).first->second;
}

void World::removeBlockEntity(int wx, int wy, int wz) {
  blockEntities_.erase(game::blockEntityKey(wx, wy, wz));
}

game::Painting* World::painting(int wx, int wy, int wz) {
  auto it = paintings_.find(game::blockEntityKey(wx, wy, wz));
  return it == paintings_.end() ? nullptr : &it->second;
}

const game::Painting* World::painting(int wx, int wy, int wz) const {
  auto it = paintings_.find(game::blockEntityKey(wx, wy, wz));
  return it == paintings_.end() ? nullptr : &it->second;
}

void World::setPainting(int wx, int wy, int wz, game::Painting art) {
  // Stamped here rather than by the caller, so every route in — the local click,
  // a guest's relayed request, the host's broadcast — gets one and none of them
  // can forget. Without it the renderer's cache, which is keyed by position, has
  // no way to notice that the picture at a position it already holds has changed:
  // it kept showing the first one until the painting was broken and rebuilt.
  art.stamp = game::nextPaintingStamp();
  paintings_[game::blockEntityKey(wx, wy, wz)] = std::move(art);
  ++paintingRevision_;
}

void World::installPaintings(std::unordered_map<game::BlockEntityKey, game::Painting> loaded) {
  paintings_ = std::move(loaded);
  // A save carries pixels but not stamps — they are runtime identity, not world
  // state. Fresh ones here, so a renderer that outlived the last world cannot
  // match one of its entries against a painting hanging in the same place in
  // this one.
  for (auto& [key, art] : paintings_) art.stamp = game::nextPaintingStamp();
  ++paintingRevision_;
}

void World::removePainting(int wx, int wy, int wz) {
  if (paintings_.erase(game::blockEntityKey(wx, wy, wz)) > 0) ++paintingRevision_;
}

void World::tickBlockEntities(float dt) {
  for (auto& [key, be] : blockEntities_) {
    if (be.kind == game::BlockEntityKind::Forge) game::tickForge(be, dt);
  }
}

void World::spawnDrop(float wx, float wy, float wz, const std::string& key, int count,
                      int dura) const {
  if (dropSink_) dropSink_(wx, wy, wz, key, count, dura);
}

void World::breakBlockInto(int wx, int wy, int wz) {
  const BlockId id = getBlock(wx, wy, wz);
  if (id == kAir) return;
  const BlockDef& def = blocks().def(id);
  // setBlock before the drop, so the drop lands in air rather than inside the
  // block it came from and gets shoved out by the collision resolver.
  setBlock(wx, wy, wz, kAir, 0);
  if (!def.drop.empty()) {
    spawnDrop(wx + 0.5f, wy + 0.5f, wz + 0.5f, def.drop, def.dropCount);
  }
}

void World::beginFall(int wx, int wy, int wz) {
  const BlockId id = getBlock(wx, wy, wz);
  if (id == kAir) return;
  // Without a sink there is nothing to hand the block to, and clearing the cell
  // would simply delete it. A headless world leaves the block standing, which is
  // the honest outcome rather than a silent loss.
  if (!fallSink) return;
  const int meta = getMeta(wx, wy, wz);
  setBlock(wx, wy, wz, kAir, 0);
  // The cell is cleared FIRST. setBlock schedules its neighbours, which is what
  // lets the block above this one notice it has lost its floor and follow it down
  // — a column of sand collapses because of this ordering, not in spite of it.
  fallSink(wx + 0.5f, static_cast<float>(wy), wz + 0.5f, id, meta);
}

int World::spawnHeight(int wx, int wz) const {
  const int ground = heightAt(noise_, wx, wz, genVersion_);
  const int sea = seaLevel(genVersion_);
  return (ground > sea ? ground : sea) + 2;
}

// Where a new world actually drops you.
//
// The browser build spawned at a fixed (8.5, 8.5) whatever was there, and since
// roughly half of any seed is ocean, half of all new worlds started you treading
// water on a beach — which is why ten worlds felt like two. Nothing about the
// terrain changes here; this only chooses a better column of it, so every seed
// still generates exactly the world it always did and no generator version moves.
//
// Searched as a widening ring so the spawn stays as near the origin as it can:
// the Atlas, the origin waypoint and every "go back to spawn" habit are all built
// around 0,0, and wandering a thousand blocks to find perfect ground would trade
// one annoyance for another.
Vec3 World::findSpawn(float preferX, float preferZ) const {
  const int sea = seaLevel(genVersion_);
  const int px = static_cast<int>(std::floor(preferX));
  const int pz = static_cast<int>(std::floor(preferZ));

  // Dry, and not on the waterline: +3 keeps you off tidal sand that a wave of
  // flowing water would reach, and off the one-block islands in a river mouth.
  const auto dryEnough = [&](int x, int z) {
    return heightAt(noise_, x, z, genVersion_) >= sea + 3;
  };
  // Clear of ravines, which is the one check that is not about comfort. A ravine
  // is carved after the heightmap out of ground the heightmap still calls solid,
  // so no amount of sampling heightAt can see one — the first version of this
  // function tried, and a canyon at spawn survived it untouched. What that costs
  // a player is not an ugly view: you spawn on the lip, fall thirty blocks, die,
  // and respawn on the same lip. The crack itself is only two or three blocks
  // across, so the grid steps 2 rather than 3 — at 3 a ravine can pass clean
  // between two probes — and reaches 8 out, which is roughly how far you drift
  // before you have taken stock of where you are.
  const auto clearOfRavines = [&](int x, int z) {
    constexpr int kReach = 8;
    constexpr int kStepP = 2;
    for (int dz = -kReach; dz <= kReach; dz += kStepP) {
      for (int dx = -kReach; dx <= kReach; dx += kStepP) {
        if (ravineAt(noise_, x + dx, z + dz, genVersion_)) return false;
      }
    }
    return true;
  };
  // Flat enough not to be a cliff edge: a large drop within a short distance is
  // what makes ground feel like a hole even when it is not one.
  const auto settled = [&](int x, int z) {
    const int h = heightAt(noise_, x, z, genVersion_);
    int lo = h, hi = h;
    const int kProbe = 3;
    const int dx[4] = {kProbe, -kProbe, 0, 0};
    const int dz[4] = {0, 0, kProbe, -kProbe};
    for (int i = 0; i < 4; ++i) {
      const int n = heightAt(noise_, x + dx[i], z + dz[i], genVersion_);
      lo = n < lo ? n : lo;
      hi = n > hi ? n : hi;
    }
    return hi - lo <= 6;
  };

  // Two passes: the first insists on flat dry ground, the second settles for dry.
  // Splitting them rather than scoring in one pass keeps "as close to origin as
  // possible" the tie-break, which a combined score would quietly trade away.
  // The ravine test is in BOTH passes: flatness is a preference the fallback can
  // trade away, and a lethal drop is not.
  constexpr int kMaxRing = 24;   // chunks
  constexpr int kStep = 8;       // blocks between candidates
  for (int pass = 0; pass < 2; ++pass) {
    for (int ring = 0; ring <= kMaxRing; ++ring) {
      const int r = ring * kStep;
      for (int dz = -r; dz <= r; dz += kStep) {
        for (int dx = -r; dx <= r; dx += kStep) {
          // Only the perimeter of this ring; the inside was covered already.
          if (ring > 0 && std::abs(dx) != r && std::abs(dz) != r) continue;
          const int x = px + dx, z = pz + dz;
          // Cheapest first: one column, then five, then eighty-one.
          if (!dryEnough(x, z)) continue;
          if (pass == 0 && !settled(x, z)) continue;
          if (!clearOfRavines(x, z)) continue;
          return Vec3{x + 0.5f, static_cast<float>(spawnHeight(x, z)), z + 0.5f};
        }
      }
    }
  }
  // An all-ocean neighbourhood for 1500 blocks. Vanishingly unlikely, and the old
  // behaviour is the right answer for it: stand at the origin, on the sea.
  return Vec3{preferX, static_cast<float>(spawnHeight(px, pz)), preferZ};
}

float World::fluidHeight(int wx, int wy, int wz) const {
  if (getBlock(wx, wy, wz) != wk().water) return 0.0f;
  if (getBlock(wx, wy + 1, wz) == wk().water) return 1.0f;  // submerged: a full column
  const int m = getMeta(wx, wy, wz);
  if (m == 0 || (m & kWaterFalling)) return 0.875f;  // source or falling
  return static_cast<float>(8 - (m & 7)) / 9.0f;     // flowing, 1..7
}

void World::waterFlow(int wx, int wy, int wz, float& fx, float& fz) const {
  fx = 0.0f;
  fz = 0.0f;
  if (getBlock(wx, wy, wz) != wk().water) return;
  const float h = fluidHeight(wx, wy, wz);
  static constexpr int kNeighbours[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  for (const auto& n : kNeighbours) {
    const int nx = wx + n[0], nz = wz + n[1];
    const BlockId id = getBlock(nx, wy, nz);
    if (id == wk().water) {
      float nh = fluidHeight(nx, wy, nz);
      // Water that can fall away is a sink, not a neighbour of equal depth.
      if (getBlock(nx, wy - 1, nz) == kAir) nh = 0.0f;
      fx += n[0] * (h - nh);
      fz += n[1] * (h - nh);
    } else if (id == kAir) {
      const float w = getBlock(nx, wy - 1, nz) == kAir ? h : h * 0.5f;
      fx += n[0] * w;  // spill toward the opening
      fz += n[1] * w;
    }
  }
}

const std::vector<Box>& World::collisionBoxesAt(int wx, int wy, int wz) const {
  // Below the world is a solid floor, so a fall cannot escape into nothing.
  if (wy < 0) {
    boxScratch_.assign(1, Box {static_cast<float>(wx), static_cast<float>(wy),
                               static_cast<float>(wz), static_cast<float>(wx + 1),
                               static_cast<float>(wy + 1), static_cast<float>(wz + 1)});
    return boxScratch_;
  }

  const BlockId id = getBlock(wx, wy, wz);
  const BlockDef& def = blocks().def(id);
  if (!def.solid) return emptyBoxes_;

  const float fx = static_cast<float>(wx);
  const float fy = static_cast<float>(wy);
  const float fz = static_cast<float>(wz);

  if (isShaped(def.render)) {
    const std::vector<Box> local = collisionBoxes(def.render, getMeta(wx, wy, wz));
    boxScratch_.clear();
    boxScratch_.reserve(local.size());
    for (const Box& b : local) {
      boxScratch_.push_back({fx + b.x0, fy + b.y0, fz + b.z0, fx + b.x1, fy + b.y1,
                             fz + b.z1});
    }
    return boxScratch_;
  }

  boxScratch_.assign(1, Box {fx, fy, fz, fx + 1, fy + 1, fz + 1});
  return boxScratch_;
}

bool World::isExplored(int cx, int cz) const {
  return explored_.find(chunkKey(cx, cz)) != explored_.end();
}

std::size_t World::pendingCount() const {
  std::size_t n = 0;
  // Only chunks inside the render distance count. The generation apron one ring
  // further out exists purely so the outermost DRAWN ring has the four neighbours
  // lighting and meshing require; an apron chunk is deliberately never lit or
  // meshed, so its standing lightDirty/meshDirty flags are the intended state and
  // not outstanding work. Counting them would leave this reading a permanent
  // non-zero — which is exactly the false alarm the apron was added to remove.
  const int r = renderDistance_;
  for (const auto& [key, lc] : chunks_) {
    const int dx = lc->chunk.cx - lastPcx_;
    const int dz = lc->chunk.cz - lastPcz_;
    if (dx * dx + dz * dz > r * r) continue;
    const Chunk& c = lc->chunk;
    if (c.lightDirty || c.meshDirty || c.genInFlight || c.lightInFlight || c.meshInFlight) ++n;
  }
  return n;
}

}  // namespace hr::world
