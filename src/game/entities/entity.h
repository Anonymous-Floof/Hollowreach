// One live entity, and the registry describing its kind.
//
// The web build carried per-type state in an untyped `e.data` bag. Here it is a
// flat struct: every field any entity type uses, in one place. That looks wasteful
// next to a variant until you count what it buys — the tick loop touches these
// fields every frame for every mob, saving a hash lookup each time; the serializer
// can whitelist per type without reflection; and the whole thing stays a plain
// copyable aggregate, which the multiplayer snapshot at M11 will want.
//
// The registry itself is the same data-driven pattern as BLOCKS / ITEMS / RECIPES:
// a definition describes a *kind*, an instance carries only its state.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <vector>

#include "core/mat4.h"
#include "game/entities/blackboard.h"
#include "game/entities/path.h"

namespace hr::world {
class World;
}
namespace hr::render {
class Sky;
}

namespace hr {
class Input;
}

namespace hr::game {

class Player;
class Inventory;
class EntityManager;

enum class EntityType : std::uint8_t {
  None = 0,
  Drop,
  Boat,
  Sheep,
  Pig,
  Cow,
  Zombie,
  RemotePlayer,
  FallingBlock,
  Count,
};

const char* entityTypeKey(EntityType type);
EntityType entityTypeFromKey(std::string_view key);

// Which mouse button drove an interaction.
enum class InteractButton : std::uint8_t { Left, Right };

struct EntityData {
  // --- dropped item ---
  std::string key;
  int count = 1;
  int dura = -1;
  float despawn = 600.0f;  // 10 minutes
  float pickupDelay = 0.4f;
  float bob = 0.0f;
  bool instant = true;

  // --- shared mob state ---
  float health = 0.0f;
  float changeT = 0.0f;
  float heading = 0.0f;
  bool moving = false;
  float flee = 0.0f;
  float hurtFlash = 0.0f;

  // Stuck watchdog. `haveSample` reproduces the JS's `d._sx != null` guard, which
  // makes the very first tick report no movement rather than a huge one.
  float sampleX = 0.0f, sampleZ = 0.0f;
  bool haveSample = false;
  float stuckT = 0.0f;

  // Flee veer: a sideways deviation held for a moment so a panicking animal runs
  // around an obstacle instead of grinding into it.
  float devAngle = 0.0f, devT = 0.0f;

  PathFollower follower;
  bool hasFollower = false;
  double pathNext = -1.0;  // AI-clock time this entity may replan again

  // --- zombie ---
  float attackCooldown = 0.0f;
  float burn = 0.0f;
  float lookT = -1.0f;  // negative means "not yet initialised", as in the JS
  bool sees = false;
  float memX = 0.0f, memY = 0.0f, memZ = 0.0f, memT = 0.0f;

  // How long until this mob makes its next idle noise. Owned by the audio director
  // rather than the AI, and negative until the director seeds it.
  float callT = -1.0f;

  // --- boat ---
  bool rider = false;

  // State-machine records, one per machine slot. Empty for every shipped mob — the
  // sheep, pig, cow and zombie still run their ad-hoc brains — and empty is the
  // whole cost until one is migrated onto a StateMachine. It lives here rather than
  // in the machine because it has to survive a save; see blackboard.h.
  std::vector<FsmRecord> fsm;
};

struct Entity {
  int id = 0;
  EntityType type = EntityType::None;
  Vec3 pos;
  Vec3 vel;
  float yaw = 0.0f, pitch = 0.0f;
  float hw = 0.3f, h = 1.8f;
  bool onGround = false;
  float age = 0.0f;
  bool dead = false;

  // A ghost is a network-mirrored entity: drawn and raycastable, never ticked
  // here — the net layer drives it by interpolating remote snapshots. Ghost ids
  // are negative so they can never collide with local ones.
  bool ghost = false;
  int netId = -1;

  EntityData data;
};

// Everything an entity hook can reach. The web build passed a `ctx` object with
// exactly these members.
struct EntityContext {
  world::World* world = nullptr;
  Player* player = nullptr;
  Inventory* inventory = nullptr;
  EntityManager* entities = nullptr;
  const render::Sky* sky = nullptr;
  const Input* input = nullptr;  // only the rideable boat reads this
  std::function<void(const std::string&)> notify;
  // Whether other people are in this world. `player` is the local body and the
  // only one an entity can see; in a shared world it is one of several, and the
  // rules that quietly assume it is the only one have to stop.
  //
  // Exactly one of those today, and it is worth naming: an "instant" drop — the
  // kind a mined block makes — is vacuumed up with no distance check at all, so
  // that mining feels immediate. With guests in the world that stopped meaning
  // "straight into your hands" and started meaning "straight into the host's,
  // from wherever the host happens to be standing". A guest could watch its own
  // ore fly off across the map.
  bool sharedWorld = false;
};

struct EntityFlags {
  bool ai = false;
  bool health = false;
  bool hostile = false;
  bool pickup = false;
  bool rideable = false;
};

struct EntityDef {
  const char* key = "";
  float hw = 0.3f, h = 1.8f;
  bool physics = false;
  float gravity = 24.0f;
  EntityFlags flags;

  void (*spawn)(Entity&) = nullptr;
  void (*update)(Entity&, float dt, EntityContext&) = nullptr;
  void (*interact)(Entity&, EntityContext&, InteractButton) = nullptr;

  // Runs after a saved entity has had its fields restored, for the fix-ups a load
  // needs that a fresh spawn does not: a reloaded drop is collectable immediately,
  // a reloaded boat has no rider, a mob whose saved health is nonsense gets its
  // type's maximum back. This is the second half of the web build's per-type
  // `deserialize` (js/game/entities/drop.js:68) — the first half, which fields
  // survive at all, is EntitySave.
  void (*load)(Entity&) = nullptr;
};

// Null for EntityType::None or an out-of-range value.
const EntityDef* defOf(EntityType type);

}  // namespace hr::game
