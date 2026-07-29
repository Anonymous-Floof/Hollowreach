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
  Entity* spawnDrop(const Vec3& pos, const std::string& key, int count, int dura);
  Entity* spawnTossed(const Vec3& pos, const Vec3& dir, const std::string& key, int count,
                      int dura);

  // Occasionally place a passive grazer on grass near the player in daylight, or a
  // zombie on any walkable ground at night, each under its own cap. Called on a
  // four-second timer, and each call is itself a coin flip — the world fills up
  // slowly rather than all at once.
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
  static constexpr int kMaxZombies = 2;

  void trySpawnGrazer(world::World& world, EntityType type, float px, float pz, float dayFactor,
                      int cap);
  void trySpawnZombie(world::World& world, float px, float pz, float dayFactor);

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
