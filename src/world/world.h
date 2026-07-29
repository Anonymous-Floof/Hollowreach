// Chunk storage and streaming, ported from js/world/world.js.
//
// Owns the loaded chunks, generates and lights them as the player moves, keeps
// their GL buffers, and applies player edits. Only edits are persisted — everything
// else regenerates from the seed, which is what keeps a save a few kilobytes.
//
// Streaming shape, kept from the original: a nearest-first ring of chunk offsets
// clipped to a circle, a per-frame cap on how many chunks may be generated, and a
// wall-clock budget for the work the main thread still has to do.
//
// Generation, lighting and meshing run on the shared job system (core/jobs). What
// stays on the main thread is everything that must: submitting work, installing
// results, and the GL uploads. A worker only ever reads immutable ChunkData
// snapshots and returns a fresh buffer, so there is no lock anywhere in the chunk
// pipeline — see the note on Chunk::data. With `--threads 0` the job system runs
// jobs inline and this file behaves exactly as it did before, which is both the
// escape hatch and the reference the determinism check compares against.

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "game/blockentities.h"
#include "render/chunkmesh.h"
#include "world/chunk.h"
#include "world/lighting.h"
#include "world/mesher.h"
#include "world/shapes.h"
#include "world/water.h"
#include "world/worldgen.h"

namespace hr::world {

// A chunk plus its render-side state.
struct LoadedChunk {
  Chunk chunk;
  render::ChunkMeshBuffer opaqueMesh;
  render::ChunkMeshBuffer waterMesh;
};

class World {
 public:
  World(std::uint32_t seed, int renderDistance, int genVersion = kGenVersion);
  // Waits for every outstanding job. A worker holds a raw pointer to this object
  // so it can push its result, so the world may not go away underneath one —
  // which is exactly what quitting to the menu does while chunks are streaming.
  ~World();

  World(const World&) = delete;
  World& operator=(const World&) = delete;

  std::uint32_t seed() const { return noise_.seed(); }
  int genVersion() const { return genVersion_; }
  const NoiseSet& noise() const { return noise_; }

  void setRenderDistance(int chunks);
  int renderDistance() const { return renderDistance_; }

  // Atlas-dependent tile lookups. Must be set before the first mesh build.
  void setTileTable(const BlockTileTable* tiles) { tiles_ = tiles; }

  // Installs whatever the workers finished, then hands out the next round of work
  // near (px, pz). `maxGen` caps how many chunks may be *submitted* for generation
  // per call (scaled by the worker count); `buildMs` bounds the main thread's own
  // share, which is now the GL uploads rather than the meshing itself.
  void update(float px, float pz, int maxGen = 4, double buildMs = 6.0);

  // Generates, lights and meshes the 3x3 around a position and blocks until it is
  // done, so the player never spawns inside unloaded space. Threaded, so it is
  // faster than the single-threaded version it replaces, not slower.
  void primeSpawn(float px, float pz);

  // Runs the pipeline to completion for the current position: everything that can
  // be generated, lit and meshed is. Used by primeSpawn, by the self-test's
  // determinism check, and by anything that needs a settled world rather than one
  // that is still streaming.
  void waitForIdle(float px, float pz);

  LoadedChunk* chunkAt(int cx, int cz);
  const LoadedChunk* chunkAt(int cx, int cz) const;
  // A chunk exists from the moment its generation job is handed out, so "is there
  // a LoadedChunk here" stopped meaning "is there terrain here" when generation
  // moved off the main thread. Everything outside this class that wanted the
  // second question asks it with this.
  bool chunkReady(int cx, int cz) const {
    const LoadedChunk* lc = chunkAt(cx, cz);
    return lc && lc->chunk.generated;
  }
  const std::unordered_map<ChunkKey, std::unique_ptr<LoadedChunk>>& chunks() const {
    return chunks_;
  }

  // --- block access, world coordinates ---
  BlockId getBlock(int wx, int wy, int wz) const;
  int getMeta(int wx, int wy, int wz) const;
  int getSky(int wx, int wy, int wz) const;
  int getBlockLight(int wx, int wy, int wz) const;

  // Applies a player edit: writes the cell, records it for saving, and dirties the
  // affected chunks. Emitter changes dirty the whole 3x3 because block light has to
  // be rebuilt across the neighbourhood.
  void setBlock(int wx, int wy, int wz, BlockId id, int meta = 0);

  // Rewrites the metadata of a cell without touching its block id — opening a
  // door, flipping a trapdoor. Cheaper than setBlock because neither light nor
  // opacity can have changed, so only the mesh is dirtied.
  void setMeta(int wx, int wy, int wz, int meta);

  // The water simulation's own write: the same recorded edit as setBlock, but no
  // relight, because water is neither opaque nor an emitter and moving it changes
  // no illumination. Public because WaterSim is the only caller and lives outside
  // this class, exactly as in the web build.
  void setWaterCell(int wx, int wy, int wz, BlockId id, int meta);
  // Advances the flowing-water automaton. Separate from update() because the host
  // owns water in multiplayer and a guest must not run it.
  void tickWater(float dt) { water_.tick(dt); }
  WaterSim& water() { return water_; }
  const WaterSim& water() const { return water_; }

  // Leaf decay: after a log is removed, any *natural* leaf that is no longer
  // within four leaf-steps of a log dissolves. Leaves flagged persistent in
  // metadata (placed by the player) never decay.
  void decayLeavesAround(int wx, int wy, int wz);

  // --- block entities (forge / chest state) ---
  game::BlockEntity* getBlockEntity(int wx, int wy, int wz);
  game::BlockEntity* getOrCreateBlockEntity(int wx, int wy, int wz, game::BlockEntityKind kind);
  void removeBlockEntity(int wx, int wy, int wz);
  // Advances every forge, so they smelt with their interface closed.
  void tickBlockEntities(float dt);
  const std::unordered_map<game::BlockEntityKey, game::BlockEntity>& blockEntities() const {
    return blockEntities_;
  }
  // Mutable, for the save layer to install a loaded set.
  std::unordered_map<game::BlockEntityKey, game::BlockEntity>& blockEntities() {
    return blockEntities_;
  }

  // --- persistence -----------------------------------------------------------
  // Player edits, keyed by chunk then flat cell index, packed as id | meta << 16.
  // These are the only part of a world that cannot be regenerated from the seed,
  // and so the only part a save has to carry cell by cell.
  using EditMap = std::unordered_map<ChunkKey, std::unordered_map<int, std::uint32_t>>;
  const EditMap& edits() const { return edits_; }
  // Installs a loaded set. Must happen before chunks stream in: generateChunk
  // replays whatever is here on top of fresh terrain, which is what makes
  // regenerating a world from its seed safe.
  void setEdits(EditMap edits) { edits_ = std::move(edits); }
  // Restores the fog of war. Chunks generated later add themselves as usual.
  void setExplored(const std::vector<ChunkKey>& keys) {
    explored_.insert(keys.begin(), keys.end());
  }

  // Mined blocks and spilled containers become item drops. A drop is an entity,
  // and the entity system is a later milestone, so the World owns only the seam:
  // whatever sink is installed receives them. With no sink the call is a no-op,
  // which is exactly what a headless tool wants.
  using DropSink = std::function<void(float x, float y, float z, const std::string& key,
                                      int count, int dura)>;
  void setDropSink(DropSink sink) { dropSink_ = std::move(sink); }
  // Readable so a caller can swap in its own for the length of one call and put
  // the old one back — which is how the multiplayer host credits a guest with
  // what its own attack knocked loose.
  const DropSink& dropSink() const { return dropSink_; }

  // Every local edit, for the net layer to relay. The same seam the web build had
  // as `world.onNetEdit`, and it deliberately fires for water's own writes too: a
  // guest does not simulate water, so the host's spreading cells have to arrive as
  // ordinary edits or the two worlds diverge the first time a bucket is poured.
  using EditSink = std::function<void(int x, int y, int z, BlockId id, int meta)>;
  void setEditSink(EditSink sink) { editSink_ = std::move(sink); }
  // Applies an edit that came from the network without echoing it back out.
  void applyRemoteEdit(int wx, int wy, int wz, BlockId id, int meta);
  void spawnDrop(float wx, float wy, float wz, const std::string& key, int count = 1,
                 int dura = -1) const;

  // --- fluids ---
  // Surface height of a water cell in blocks, or 0 when it is not water. A cell
  // with water directly above is a full column; a source or a falling cell sits
  // just below the top; a flowing cell thins with its metadata level.
  float fluidHeight(int wx, int wy, int wz) const;
  // Horizontal flow at a water cell, pointing downstream — from full water toward
  // thinner water, open edges and drops — with a magnitude that grows with the
  // height gradient. This is what shoves the player and floating entities along.
  void waterFlow(int wx, int wy, int wz, float& fx, float& fz) const;

  // Where a player standing at (wx, wz) should be put: two blocks above the
  // terrain, or above the sea when the terrain is under it. The sea clamp is the
  // whole point (js/world/world.js:555) — a column whose ground is below sea level
  // is ocean floor, and dropping a player two blocks above THAT starts them
  // underwater with a drowning clock already running.
  int spawnHeight(int wx, int wz) const;

  // Collision boxes for a cell, in world space. Returns an empty span for a
  // non-solid cell, and a solid floor below y = 0.
  const std::vector<Box>& collisionBoxesAt(int wx, int wy, int wz) const;

  // --- Atlas support ---------------------------------------------------------
  // Chunks the player has actually generated. The Atlas only maps explored ground, so
  // this is the fog of war — and it must outlive a chunk unload, which is why it is a
  // key set rather than a flag on LoadedChunk.
  const std::unordered_set<ChunkKey>& explored() const { return explored_; }
  bool isExplored(int cx, int cz) const;
  // Chunks whose map tile is stale, because an edit or a chunk load changed them. The
  // Atlas drains this: it owns the tile cache, so it is the only thing that can act on
  // the invalidation.
  std::unordered_set<ChunkKey>& mapDirty() { return mapDirty_; }

  // Counts, for the debug overlay.
  std::size_t loadedChunkCount() const { return chunks_.size(); }
  std::size_t pendingCount() const;
  // Chunk jobs queued or running, which is what the overlay's "streaming" number
  // should actually show now that the work is not on this thread.
  int jobsInFlight() const { return genInFlight_ + lightInFlight_ + meshInFlight_; }

 private:
  static int floorDiv16(int v) { return v >= 0 ? v >> 4 : ~((~v) >> 4); }

  LoadedChunk& ensureChunk(int cx, int cz);
  // The one place a chunk's voxels, metadata or light may be written. Clones first
  // if a worker is holding the current snapshot, which is what makes the whole
  // pipeline lock-free.
  ChunkData& mutableData(LoadedChunk& lc);
  // A chunk's data only if it has actually been generated. An ensureChunk'd but
  // ungenerated chunk exists — its generation job is out — and feeding its empty
  // arrays to the lighter or the mesher would bake a hole into the world.
  const ChunkData* dataFor(int cx, int cz) const;

  void submitGen(LoadedChunk& lc);
  void submitLight(LoadedChunk& lc);
  void submitMesh(LoadedChunk& lc);
  // Applies the saved edits for a chunk that has just come back from generation.
  void applyEdits(LoadedChunk& lc);
  // Drains the completion queues. Returns true if anything was installed, which is
  // how the inline path knows there may be more work to hand out.
  bool installResults(double deadlineMs);
  // Returns how many jobs went out, which is what tells waitForIdle that a round
  // found nothing left to do.
  int scanAndSubmit(int pcx, int pcz, int genBudget, int stageBudget, int maxInFlight);

  bool neighboursGenerated(int cx, int cz) const;
  void dirtyLight(int cx, int cz);
  void dirtyMesh(int cx, int cz);
  void unloadFar(int pcx, int pcz, int radius);
  const std::vector<std::pair<int, int>>& ringOffsets(int radius);

  NoiseSet noise_;
  int genVersion_;
  int renderDistance_;
  const BlockTileTable* tiles_ = nullptr;

  std::unordered_map<ChunkKey, std::unique_ptr<LoadedChunk>> chunks_;

  // Atlas state. `explored_` grows once per chunk ever generated and is part of the
  // save; `mapDirty_` is transient and drained by whoever owns a tile cache.
  std::unordered_set<ChunkKey> explored_;
  std::unordered_set<ChunkKey> mapDirty_;

  // Packed as id | meta << 16. The web build packed into 16 bits total, capping the
  // game at 1024 blocks and 64 metadata values; 32 bits removes that ceiling.
  EditMap edits_;

  // Keyed by packed position, so state outlives the chunk it sits in.
  std::unordered_map<game::BlockEntityKey, game::BlockEntity> blockEntities_;
  DropSink dropSink_;
  EditSink editSink_;
  // Set while a remote edit is being applied, so the sink does not send it back to
  // the peer it came from.
  bool applyingRemote_ = false;

  // Holds a reference back to this world, so it must be declared after everything
  // it reaches through and must not outlive it.
  WaterSim water_{*this};

  // --- job results ------------------------------------------------------------
  // Workers push here and the main thread drains it in installResults(). This
  // mutex is the only one in the chunk pipeline, it is held for a push or a swap
  // and never across the work itself, and it protects nothing a worker reads.
  struct GenResult {
    ChunkKey key;
    std::shared_ptr<ChunkData> data;
  };
  struct LightResult {
    ChunkKey key;
    // The worker's own copy. Only its two light arrays are read out on install, so
    // an edit that landed while the job was running is not clobbered by it. Shared
    // rather than unique because std::function requires a copyable callable, and
    // the job has to carry this into the worker.
    std::shared_ptr<ChunkData> working;
    std::vector<Emitter> emitters;
  };
  struct MeshResultSlot {
    ChunkKey key;
    MeshResult mesh;
  };

  mutable std::mutex resultsMutex_;
  std::vector<GenResult> genDone_;
  std::vector<LightResult> lightDone_;
  std::vector<MeshResultSlot> meshDone_;

  // Main-thread counters, so a lane can be throttled without walking every chunk.
  // Mesh results are the one thing worth capping: a teleport dirties the whole
  // ring at once, and a hundred finished meshes waiting to be uploaded is tens of
  // megabytes of vertex data sitting in a queue.
  int genInFlight_ = 0;
  int lightInFlight_ = 0;
  int meshInFlight_ = 0;

  // Cached nearest-first offsets per radius.
  std::unordered_map<int, std::vector<std::pair<int, int>>> ringCache_;

  int lastPcx_ = INT_MIN, lastPcz_ = INT_MIN;
  bool forceScan_ = true;

  // Returned by collisionBoxesAt for non-solid cells, so callers can always hold a
  // reference.
  std::vector<Box> emptyBoxes_;
  mutable std::vector<Box> boxScratch_;
};

}  // namespace hr::world
