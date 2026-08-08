// The three passive grazers and the zombie, ported from js/game/entities/.
//
// The grazers differ only in their numbers and what they drop, so they share the
// wander/flee brain in ai.cpp and a single hurt reaction. The zombie has its own
// update: true voxel line-of-sight, a memory of where its target was last seen, an
// A* chase, and a burn timer that keeps it to the night.

#include <algorithm>
#include <cmath>

#include "audio/sfx.h"
#include "core/prng.h"
#include "game/entities/ai.h"
#include "game/entities/manager.h"
#include "game/entities/senses.h"
#include "game/entities/types.h"
#include "game/inventory.h"
#include "game/items.h"
#include "game/player.h"
#include "render/sky.h"
#include "world/world.h"

namespace hr::game {
namespace {

constexpr float kPi = 3.14159265358979f;

inline float R(float a, float b) { return a + static_cast<float>(randomUnit()) * (b - a); }

void mobSpawn(Entity& e, float maxHealth) {
  if (e.data.health <= 0.0f) e.data.health = maxHealth;
  e.data.changeT = R(0.0f, 2.0f);
  e.data.heading = static_cast<float>(randomUnit()) * kPi * 2.0f;
  e.data.moving = false;
  e.data.flee = 0.0f;
  e.data.hurtFlash = 0.0f;
}

void notify(EntityContext& ctx, const char* message) {
  if (ctx.notify) ctx.notify(message);
}

// ---------------------------------------------------------------------------
// Sheep: drops the white wool that, with planks, makes a bed.
// ---------------------------------------------------------------------------

constexpr float kSheepHp = 8.0f;

void sheepSpawn(Entity& e) { mobSpawn(e, kSheepHp); }

void sheepUpdate(Entity& e, float dt, EntityContext& ctx) {
  grazeUpdate(e, dt, ctx, GrazeOptions{1.7f, 3.4f});
}

void sheepInteract(Entity& e, EntityContext& ctx, InteractButton button) {
  if (button != InteractButton::Left) return;
  if (grazeHurt(e, ctx)) {
    e.dead = true;
    ctx.world->spawnDrop(e.pos.x, e.pos.y + 0.4f, e.pos.z, "wool", 1);
    notify(ctx, "The sheep drops its wool.");
  }
}

// ---------------------------------------------------------------------------
// Pig: a raw porkchop, better smelted.
// ---------------------------------------------------------------------------

constexpr float kPigHp = 10.0f;

void pigSpawn(Entity& e) { mobSpawn(e, kPigHp); }

void pigUpdate(Entity& e, float dt, EntityContext& ctx) {
  grazeUpdate(e, dt, ctx, GrazeOptions{1.6f, 3.2f});
}

void pigInteract(Entity& e, EntityContext& ctx, InteractButton button) {
  if (button != InteractButton::Left) return;
  if (grazeHurt(e, ctx)) {
    e.dead = true;
    ctx.world->spawnDrop(e.pos.x, e.pos.y + 0.4f, e.pos.z, "pork_raw", 1);
    notify(ctx, "The pig drops a porkchop.");
  }
}

// ---------------------------------------------------------------------------
// Cow: the biggest grazer. Beef and leather, and milkable with an empty bucket.
// ---------------------------------------------------------------------------

constexpr float kCowHp = 12.0f;

void cowSpawn(Entity& e) { mobSpawn(e, kCowHp); }

void cowUpdate(Entity& e, float dt, EntityContext& ctx) {
  grazeUpdate(e, dt, ctx, GrazeOptions{1.4f, 3.0f});
}

void cowInteract(Entity& e, EntityContext& ctx, InteractButton button) {
  if (button == InteractButton::Right) {
    // Milking: swap an empty bucket for a full one.
    const ItemStack& slot = ctx.inventory->selectedSlot();
    if (!slot.empty() && slot.key == "bucket") {
      ctx.inventory->consumeSelected();
      ctx.inventory->give("milk_bucket", 1);
      audio::sfx::splash(false);
      notify(ctx, "You milk the cow.");
    }
    return;
  }
  if (button != InteractButton::Left) return;
  if (grazeHurt(e, ctx)) {
    e.dead = true;
    const int beef = 1 + (randomUnit() < 0.5 ? 1 : 0);
    ctx.world->spawnDrop(e.pos.x, e.pos.y + 0.5f, e.pos.z, "beef_raw", beef);
    const int hide = static_cast<int>(randomUnit() * 3.0);  // 0-2
    if (hide > 0) ctx.world->spawnDrop(e.pos.x, e.pos.y + 0.5f, e.pos.z, "leather", hide);
    notify(ctx, hide > 0 ? "The cow drops beef and leather." : "The cow drops beef.");
  }
}

// ---------------------------------------------------------------------------
// Zombie
// ---------------------------------------------------------------------------

constexpr float kZombieHp = 16.0f;
constexpr float kAggro = 16.0f;   // blocks: how far it can notice the player
constexpr float kReach = 1.7f;    // blocks: how close before it can hit
constexpr float kHitDamage = 3.0f;
constexpr float kMemory = 8.0f;   // seconds it hunts the last-seen spot
constexpr float kEyeY = 1.62f;

void zombieSpawn(Entity& e) {
  if (e.data.health <= 0.0f) e.data.health = kZombieHp;
  e.data.changeT = R(0.0f, 2.0f);
  e.data.heading = static_cast<float>(randomUnit()) * kPi * 2.0f;
  e.data.moving = false;
  e.data.attackCooldown = 0.0f;
  e.data.burn = 0.0f;
  e.data.hurtFlash = 0.0f;
}

void zombieUpdate(Entity& e, float dt, EntityContext& ctx) {
  EntityData& d = e.data;
  d.hurtFlash = std::max(0.0f, d.hurtFlash - dt);
  d.attackCooldown = std::max(0.0f, d.attackCooldown - dt);

  // ---- burn in direct sunlight, which is what keeps them to the night ----
  if (ctx.sky && ctx.sky->dayFactor() > 0.5f) {
    const int hy = static_cast<int>(std::floor(e.pos.y + 1.6f));
    const bool exposed = ctx.world->getSky(static_cast<int>(std::floor(e.pos.x)), hy,
                                           static_cast<int>(std::floor(e.pos.z))) >= 15;
    if (exposed) {
      d.burn += dt;
      if (d.burn >= 1.0f) {
        d.burn = 0.0f;
        d.health -= 2.0f;
        d.hurtFlash = 0.3f;
        audio::sfx::sizzle(e.pos);
      }
      if (d.health <= 0.0f) {
        e.dead = true;  // burned up: no drop
        return;
      }
    } else {
      d.burn = 0.0f;
    }
  }

  // ---- target: the player, when alive ----
  // The multiplayer host will supply a list here; single-player has exactly one.
  Player* target = nullptr;
  float dist = 1e30f, dx = 0.0f, dz = 0.0f, dy = 0.0f;
  if (ctx.player && ctx.player->health() > 0.0f) {
    const Vec3 p = ctx.player->pos();
    dx = p.x - e.pos.x;
    dz = p.z - e.pos.z;
    dist = std::sqrt(dx * dx + dz * dz);
    dy = std::fabs(p.y - e.pos.y);
    target = ctx.player;
  }

  // ---- sight: throttled true line-of-sight ----
  // No x-ray vision: a wall between eye and target means not spotted, and a target
  // that breaks the line leaves only a fading memory.
  const bool inWater = floatInWater(*ctx.world, e);
  if (d.lookT < 0.0f) d.lookT = static_cast<float>(randomUnit()) * 0.25f;
  d.lookT -= dt;
  if (target && dist < kAggro && dy < 6.0f) {
    if (d.lookT <= 0.0f) {
      d.lookT = 0.25f;
      const Vec3 eye{e.pos.x, e.pos.y + kEyeY, e.pos.z};
      const Vec3 teye{target->pos().x, target->pos().y + 1.5f, target->pos().z};
      d.sees = lineOfSight(*ctx.world, eye, teye, kAggro + 4.0f);
    }
  } else {
    d.sees = false;
  }
  if (d.sees && target) {
    d.memX = target->pos().x;
    d.memY = target->pos().y;
    d.memZ = target->pos().z;
    d.memT = kMemory;
  } else {
    d.memT = std::max(0.0f, d.memT - dt);
  }

  if (!((d.sees && target) || d.memT > 0.0f)) {
    // ---- no target: path-assisted wandering, shared with the grazers ----
    wanderStep(e, dt, ctx, 1.2f, inWater);
    return;
  }

  // ---- hunt: the target if visible, else where it was last seen ----
  const bool seen = d.sees && target != nullptr;
  const float gx = seen ? target->pos().x : d.memX;
  const float gy = seen ? target->pos().y : d.memY;
  const float gz = seen ? target->pos().z : d.memZ;
  const float gdx = gx - e.pos.x, gdz = gz - e.pos.z;
  const float gdist = std::sqrt(gdx * gdx + gdz * gdz);

  if (seen && dist < kReach && dy < 2.0f && d.attackCooldown <= 0.0f) {
    d.attackCooldown = 1.0f;
    const float l = std::max(1e-4f, dist);
    const float defense = ctx.inventory ? static_cast<float>(ctx.inventory->totalDefense()) : 0.0f;
    // Built from the real options rather than from nothing. This site used to
    // fabricate a bare PlayerOptions carrying only the armour figure, which was
    // harmless while every field it dropped was cosmetic — and stopped being
    // harmless the moment one of them was "cannot be hurt". A zombie was the one
    // thing in the world that could still kill an invulnerable player.
    PlayerOptions hit = ctx.playerOptions ? *ctx.playerOptions : PlayerOptions{};
    hit.defense = defense;
    const bool wasHurt = !hit.invulnerable;
    target->damage(kHitDamage, hit);
    if (!wasHurt) return;  // no hit, no shove, no notification
    Vec3 v = target->velocity();  // shove the target back
    v.x += (dx / l) * 4.0f;
    v.z += (dz / l) * 4.0f;
    v.y = std::max(v.y, 3.0f);
    target->setVelocity(v);
    notify(ctx, "A zombie claws at you!");
  }

  if (seen && dist < kReach) {
    // Planted on the player: stop, so it does not walk through them, and face them.
    d.hasFollower = false;
    d.heading = std::atan2(dz, dx);
    stuck(e, dt, 0.0f);
    e.yaw = kPi / 2.0f - d.heading;
    return;
  }
  if (!seen && gdist < 1.4f) {
    // Reached a cold trail with nobody there: give up and mill about.
    d.memT = 0.0f;
    d.hasFollower = false;
    wanderStep(e, dt, ctx, 1.2f, inWater);
    return;
  }

  // Path toward the goal, replanning when it drifts or the route dies.
  const PathPoint* end = d.hasFollower ? d.follower.end() : nullptr;
  const bool needNew = !d.hasFollower || d.follower.done() ||
                       (end && std::sqrt((end->x - gx) * (end->x - gx) +
                                         (end->z - gz) * (end->z - gz)) > 3.0f);
  if (needNew && ctx.entities && !inWater) {
    PathOptions options;
    options.maxFall = 3;
    options.maxDist = 24;
    Path path;
    if (ctx.entities->ai().requestPath(e, Vec3{gx, gy, gz}, options, 0.5f, *ctx.world, path) &&
        path.points.size() > 1) {
      d.follower = PathFollower(std::move(path));
      d.hasFollower = true;
    }
  }
  if (d.hasFollower && !inWater) {
    stuck(e, dt, 0.0f);
    const PathFollower::Status st = d.follower.step(e, dt, 2.4f);
    if (st != PathFollower::Status::Moving) {
      d.hasFollower = false;
      if (st == PathFollower::Status::Stuck) d.pathNext = 0.0;  // replan next tick
    }
  } else {
    // No path yet — cooldown or budget — or paddling: go straight at it.
    d.heading = std::atan2(gdz, gdx);
    steerHeading(e, dt, ctx, 2.4f, inWater);
  }
}

void zombieInteract(Entity& e, EntityContext& ctx, InteractButton button) {
  if (button != InteractButton::Left) return;
  e.data.health -= attackDamage(*ctx.inventory, ctx.player);
  e.data.hurtFlash = 0.35f;

  const Vec3 vpos{e.pos.x, e.pos.y + 1.6f, e.pos.z};
  audio::sfx::thwack(vpos);
  audio::sfx::zombie(
      e.data.health <= 0.0f ? audio::sfx::MobCall::Death : audio::sfx::MobCall::Hurt, vpos, e.id);

  const Vec3 p = ctx.player->pos();  // knock it back
  const float dx = e.pos.x - p.x, dz = e.pos.z - p.z;
  const float l = std::max(1e-4f, std::sqrt(dx * dx + dz * dz));
  e.vel.x += (dx / l) * 4.0f;
  e.vel.z += (dz / l) * 4.0f;
  e.vel.y = 4.0f;

  const ItemStack& slot = ctx.inventory->selectedSlot();
  const ItemDef* held = slot.empty() ? nullptr : getItem(slot.key);
  if (held && held->type == ItemType::Tool) ctx.inventory->damageSelectedTool(1);

  if (e.data.health <= 0.0f) {
    e.dead = true;
    ctx.world->spawnDrop(e.pos.x, e.pos.y + 0.6f, e.pos.z, "rotten_flesh", 1);
  }
}

// Health is the only field a mob carries across a save (js/game/entities/sheep.js:38
// whitelists exactly that). A value outside the type's range means a corrupt or
// hand-edited file, and a mob that loads at full health is the harmless reading.
void clampHealth(Entity& e, float maxHealth) {
  if (!(e.data.health > 0.0f) || e.data.health > maxHealth) e.data.health = maxHealth;
}
void sheepLoad(Entity& e) { clampHealth(e, kSheepHp); }
void pigLoad(Entity& e) { clampHealth(e, kPigHp); }
void cowLoad(Entity& e) { clampHealth(e, kCowHp); }
void zombieLoad(Entity& e) { clampHealth(e, kZombieHp); }

}  // namespace

const EntityDef kSheepDef{
    "sheep", 0.45f, 1.0f, true, 26.0f,
    EntityFlags{true, true, false, false, false},
    sheepSpawn, sheepUpdate, sheepInteract, sheepLoad,
};

const EntityDef kPigDef{
    "pig", 0.45f, 0.9f, true, 26.0f,
    EntityFlags{true, true, false, false, false},
    pigSpawn, pigUpdate, pigInteract, pigLoad,
};

const EntityDef kCowDef{
    "cow", 0.55f, 1.35f, true, 26.0f,
    EntityFlags{true, true, false, false, false},
    cowSpawn, cowUpdate, cowInteract, cowLoad,
};

const EntityDef kZombieDef{
    "zombie", 0.4f, 1.9f, true, 26.0f,
    EntityFlags{true, true, true, false, false},
    zombieSpawn, zombieUpdate, zombieInteract, zombieLoad,
};

// Another player, seen over the network. Always a ghost — net-driven, with no
// local physics or hooks — so combat against one routes through the host at M11
// and it deliberately has no interact hook of its own.
const EntityDef kRemotePlayerDef{
    "remote_player", 0.3f, 1.8f, false, 0.0f, EntityFlags{}, nullptr, nullptr, nullptr,
};

}  // namespace hr::game
