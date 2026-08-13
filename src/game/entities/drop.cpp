// Dropped item entity, ported from js/game/entities/drop.js.
//
// Two flavours, distinguished by `instant`:
//  * instant (mined blocks, spilled containers) — vacuumed into the inventory the
//    moment there is room, with no distance check, so mining feels immediate.
//  * non-instant (Q-tossed items, death drops) — must be walked over, and waits
//    longer before it can be picked up, so throwing something away discards it and
//    you can run back for your things after dying.
//
// Either way they fall, bob, spin, merge with nearby like drops, and despawn after
// ten minutes.

#include <cmath>

#include "audio/sfx.h"
#include "core/prng.h"
#include "game/entities/manager.h"
#include "game/entities/types.h"
#include "game/inventory.h"
#include "game/player.h"

namespace hr::game {
namespace {

constexpr float kMergeRange2 = 1.6f * 1.6f;
constexpr float kPickupRange2 = 1.8f * 1.8f;
constexpr float kDespawn = 600.0f;  // ten minutes
constexpr float kPi = 3.14159265358979f;

void dropSpawn(Entity& e) {
  if (e.data.count <= 0) e.data.count = 1;
  e.data.despawn = kDespawn;
  e.data.pickupDelay = e.data.instant ? 0.4f : 1.0f;
  e.data.bob = static_cast<float>(randomUnit()) * kPi * 2.0f;
}

void dropUpdate(Entity& e, float dt, EntityContext& ctx) {
  e.data.bob += dt * 3.0f;
  e.yaw += dt * 1.6f;

  if (e.age >= e.data.despawn) {
    e.dead = true;
    return;
  }
  if (e.age < e.data.pickupDelay) return;

  // Merge with a nearby like drop; the higher id absorbs the lower.
  if (ctx.entities) {
    for (Entity& o : ctx.entities->all()) {
      if (&o == &e || o.dead || o.type != EntityType::Drop) continue;
      // Colour is part of "a like drop". Two piles of wool on the floor merge only
      // if they are the same colour; without the tint test a red one lying next to a
      // blue one would absorb it and the second dye would simply be gone.
      if (o.data.key != e.data.key || o.data.tint != e.data.tint ||
          o.data.instant != e.data.instant || o.id <= e.id) {
        continue;
      }
      const float dx = o.pos.x - e.pos.x, dy = o.pos.y - e.pos.y, dz = o.pos.z - e.pos.z;
      if (dx * dx + dy * dy + dz * dz > kMergeRange2) continue;
      o.data.count += e.data.count;
      e.dead = true;
      return;
    }
  }

  // An instant drop collects from anywhere; a tossed one needs proximity. In a
  // shared world both need it: "from anywhere" is only generous while there is
  // one pair of hands it could go to, and with guests present it means the host
  // silently harvests everything anyone mines, anywhere in the world. It is the
  // same rule underneath — an instant drop still has the shorter pickup delay,
  // so mining is still immediate for the person stood over the block.
  if (!e.data.instant || ctx.sharedWorld) {
    const Vec3 p = ctx.player->pos();
    const float dx = p.x - e.pos.x, dy = (p.y + 0.9f) - e.pos.y, dz = p.z - e.pos.z;
    if (dx * dx + dy * dy + dz * dz > kPickupRange2) return;
  }
  const int left = ctx.inventory->give(e.data.key, e.data.count, e.data.dura, e.data.tint);
  if (left < e.data.count) audio::sfx::pickup();
  if (left <= 0) e.dead = true;
  else e.data.count = left;
}

// A drop that was in the world when it was saved has long since settled, so it is
// collectable the moment the world comes back rather than after another pickup
// delay — js/game/entities/drop.js:68 does the same.
void dropLoad(Entity& e) {
  e.data.pickupDelay = 0.0f;
  if (e.data.count <= 0) e.data.count = 1;
  if (e.data.despawn <= 0.0f) e.data.despawn = kDespawn;
}

}  // namespace

const EntityDef kDropDef{
    "drop", 0.18f, 0.36f, /*physics=*/true, /*gravity=*/26.0f,
    EntityFlags{/*ai=*/false, /*health=*/false, /*hostile=*/false, /*pickup=*/true,
                /*rideable=*/false},
    dropSpawn, dropUpdate, nullptr, dropLoad,
};

}  // namespace hr::game
