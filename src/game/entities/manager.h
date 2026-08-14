// Owns every live entity for a world: spawn and remove, the per-frame tick (base
// physics plus the type's update hook), ray picking, and save/load.
//
// Ported from js/game/entities/manager.js. The web build hung this off
// `world.entities`; here the World stays free of it and the manager is passed
// through EntityContext instead, which keeps the world layer from depending on the
// game layer in both directions at once.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/entities/entity.h"
#include "game/entities/path.h"
#include "game/entities/senses.h"

namespace hr::game {

// Shared AI world-services, one per world.
//
//   sounds     — transient sound events mobs can hear. Anything may emit.
//   scent      — the decaying trail of player positions, for smell-tracking.
//   pathBudget — the shared A* expansion allowance, refilled every tick, so a
//                crowd of pathing mobs splits one frame's work instead of each
//                spiking it.
class AIServices {
 public:
  void tick(float dt, const EntityContext& ctx);

  // Budgeted, throttled pathfind for one entity: at most one fresh path every
  // `cooldown` seconds per entity, so calling it every tick is safe.
  bool requestPath(Entity& e, const Vec3& goal, const PathOptions& options, float cooldown,
                   const world::World& world, Path& out);

  SoundBus& sounds() { return sounds_; }
  const SoundBus& sounds() const { return sounds_; }
  ScentField& scent() { return scent_; }
  const ScentField& scent() const { return scent_; }
  PathBudget& pathBudget() { return budget_; }

 private:
  static constexpr int kBudgetPerTick = 1500;  // total A* expansions per frame
  static constexpr float kScentPeriod = 0.3f;  // seconds between player scent drops

  SoundBus sounds_;
  ScentField scent_;
  PathBudget budget_{kBudgetPerTick};
  float scentT_ = 0.0f;
};

struct EntityRayHit {
  Entity* entity = nullptr;
  float dist = 0;
};

// --- where monsters are allowed to appear ------------------------------------
//
// The light a spawn check sees, 0..15. Block light is absolute — a torch is a
// torch at every hour — while baked skylight is scaled by the day factor, which
// is the same thing the shader does to it (render/sky.h:daylight). So this
// number is, near enough, the darkness you are actually looking at: 15 on open
// ground at noon, 0 on the same ground at midnight, 0 in a sealed cave either
// way, and never 0 within reach of a torch.
//
// Returns 15 — "bright, do not spawn" — for a chunk whose light has not been
// computed yet, rather than reading its zeroed arrays as darkness.
int effectiveLight(const world::World& world, int wx, int wy, int wz, float dayFactor);

// Can a monster stand with its feet in this cell? Solid footing below, two clear
// cells for the body, and no light. This is the whole spawn rule, in one place,
// because the world spawner and the Evil Altar have to agree on it — an altar
// you can switch off with torches is the behaviour worth having.
bool monsterSpawnable(const world::World& world, int wx, int wy, int wz, float dayFactor);

// The subset of an entity that survives a save.
//
// The web build gave each type its own `serialize`/`deserialize` pair, each a
// whitelist of the fields that were allowed through (js/game/entities/manager.js:143
// falls back to a shallow copy of the whole bag when a type does not supply one).
// Here the whitelist IS this struct: nothing outside it is written, so a field added
// to EntityData is not silently persisted, and a type's fix-ups on the way back in
// live in its `EntityDef::load` hook.
//
// Every field is written for every entity rather than switching on the type. A
// cow's unused `key` costs two bytes and a drop's unused `health` costs four; a
// world holds tens of entities, not thousands, and in exchange the encoder has no
// per-type branch to keep in step with the registry.
struct EntitySave {
  EntityType type = EntityType::None;
  Vec3 pos;
  Vec3 vel;
  float yaw = 0.0f;

  // Drop.
  std::string key;
  int count = 1;
  int dura = -1;
  // The dye on the stack lying on the ground. Saved, or a dyed item dropped at
  // logout comes back the colour it was before anybody painted it.
  std::int32_t tint = -1;
  float despawn = 600.0f;
  bool instant = true;

  // Any mob.
  float health = 0.0f;

  // State-machine records. Empty for every shipped mob; present so that migrating
  // one onto a StateMachine does not need a format change.
  std::vector<FsmRecord> fsm;
};

class EntityManager {
 public:
  Entity* spawn(EntityType type, const Vec3& pos);
  // A ghost is network-mirrored: drawn and raycastable, never ticked here.
  Entity* spawnGhost(int netId, EntityType type, const Vec3& pos);
  void remove(Entity& e) { e.dead = true; }
  void clear() { entities_.clear(); }

  void tick(float dt, EntityContext& ctx);

  // Nearest entity whose AABB the ray enters within maxDist.
  bool raycast(const Vec3& origin, const Vec3& dir, float maxDist, EntityRayHit& out);

  // --- spawning ---
  // A mined block or a spilled container becomes an instant-collect drop with a
  // small pop; a tossed or death drop is thrown along `dir` and must be walked
  // over. Both are the sinks the World's spawnDrop seam feeds.
  //
  // `tint` is required by both, and defaulted by neither. It was defaulted on
  // spawnDrop and absent from spawnTossed, which made every thrown item white: a
  // dye discarded on the way out looks exactly like a dye that was never there, so
  // the item came back from the floor as plain wool with no error anywhere. The
  // twins take the same arguments so the next thing a stack learns to carry cannot
  // reach one of them and miss the other.
  Entity* spawnDrop(const Vec3& pos, const std::string& key, int count, int dura,
                    std::int32_t tint);
  Entity* spawnTossed(const Vec3& pos, const Vec3& dir, const std::string& key, int count,
                      int dura, std::int32_t tint);

  // Occasionally place a passive grazer on grass near the player in daylight, or a
  // zombie anywhere dark enough, each under its own cap. Called on a four-second
  // timer, and each call is itself a coin flip — the world fills up slowly rather
  // than all at once.
  //
  // The caps below are what actually decide how many mobs a world holds: the timer
  // and the coin flip only set how fast it fills to them. The web build's were
  // 8 / 6 / 5 (js/world/world.js:240-242) and 6 for zombies (world.js:246), which
  // put nineteen head of livestock inside the spawn radius and made a meadow read as
  // a stockyard. These are a quarter of those, rounded to nearest with a floor of
  // one so no species disappears entirely.
  static constexpr int kMaxSheep = 2;
  static constexpr int kMaxPig = 2;
  static constexpr int kMaxCow = 1;
  // Zombies are no longer bounded by dawn — a cave is dark at noon — so this cap
  // is now the only thing standing between the player and an endless queue of
  // them. Two at a time near you, same as before, plus whatever an altar is
  // making, which is counted separately and deliberately.
  static constexpr int kMaxZombies = 2;

  // How far a hostile may drift from the player before it is culled. Under the
  // old rules the sun did this job: every zombie burned away at dawn, so the cap
  // cleared itself daily. Cave spawns never see the sun, and a mob in an unloaded
  // chunk is frozen rather than removed, so without this the two slots fill once
  // with mobs nobody will ever meet again and the night goes quiet forever.
  // Comfortably outside the 40-block spawn ring, so nothing is culled the moment
  // it appears.
  static constexpr float kHostileDespawn = 72.0f;

  // The ceiling on drops lying about in the world, and the one thing that bounds
  // a world's entity list at all. See cullExcessDrops for why it is a count and
  // not a clock, and why it is set this high.
  static constexpr std::size_t kMaxDrops = 1024;

  // How far up and down a column the spawn search sweeps, around the player's own
  // height. Deeper than it is tall because that is where the dark is: the ceiling
  // of the band only has to clear a hill you are standing at the foot of.
  static constexpr int kSpawnBandDown = 32;
  static constexpr int kSpawnBandUp = 16;

  // Evil Altar. Range is the half-width of the box searched around the player, so
  // an altar wakes at 7 blocks; the crowd limit is how many zombies may already be
  // standing within kAltarCrowdRadius of it before it stops adding more.
  static constexpr int kAltarRange = 7;
  static constexpr int kAltarCrowd = 4;
  static constexpr float kAltarCrowdRadius = 12.0f;

  void trySpawnGrazer(world::World& world, EntityType type, float px, float pz, float dayFactor,
                      int cap);
  // `player` rather than a flat (x, z): the search is a vertical band around the
  // player now, not a walk down to the surface, so it needs their height too.
  void trySpawnZombie(world::World& world, const Vec3& player, float dayFactor);
  // Every Evil Altar within range of the player gets a chance to breed. Separate
  // from trySpawnZombie because an altar answers to its own local cap: a dungeon
  // that stopped at the world's two-zombie ceiling would not be a dungeon.
  // Kills off the oldest drops once there are more than kMaxDrops of them.
  void cullExcessDrops();
  void tickEvilAltars(world::World& world, const Vec3& player, float dayFactor);

  std::vector<Entity>& all() { return entities_; }
  const std::vector<Entity>& all() const { return entities_; }
  Entity* byId(int id);
  int count() const;

  // --- save / load ---
  // Ghosts are skipped: they belong to the network, not to the world.
  std::vector<EntitySave> serialize() const;
  // Replaces every entity. Ids are re-issued rather than restored, exactly as in
  // the web build — nothing outside this vector refers to an entity by id across a
  // save, because the one thing that does (the player's mount) is not saved.
  void load(const std::vector<EntitySave>& saved);

  AIServices& ai() { return ai_; }
  const AIServices& ai() const { return ai_; }

 private:
  // One altar's turn: its own crowd check, then four tries at a cell around it.
  void spawnFromAltar(world::World& world, int ax, int ay, int az, float dayFactor);

  std::vector<Entity> entities_;
  AIServices ai_;
  int nextId_ = 1;
};

// Convenience for the senses, which only need the two buses.
inline SoundBus& sounds(EntityManager& m) { return m.ai().sounds(); }
inline const SoundBus& sounds(const EntityManager& m) { return m.ai().sounds(); }
inline ScentField& scent(EntityManager& m) { return m.ai().scent(); }
inline const ScentField& scent(const EntityManager& m) { return m.ai().scent(); }

}  // namespace hr::game
