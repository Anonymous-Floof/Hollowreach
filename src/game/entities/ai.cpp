#include "game/entities/ai.h"

#include <algorithm>
#include <cmath>

#include "audio/sfx.h"
#include "core/prng.h"
#include "game/entities/manager.h"
#include "game/inventory.h"
#include "game/items.h"
#include "game/player.h"
#include "world/blocks.h"
#include "world/world.h"

namespace hr::game {
namespace {

constexpr float kPi = 3.14159265358979f;

inline float R(float a, float b) { return a + static_cast<float>(randomUnit()) * (b - a); }

// The mob voice for a type, so the shared hurt reaction can speak without a switch
// at every call site.
void mobVoice(EntityType type, audio::sfx::MobCall kind, const Vec3& pos, int seed) {
  switch (type) {
    case EntityType::Sheep: audio::sfx::sheep(kind, pos, seed); break;
    case EntityType::Pig: audio::sfx::pig(kind, pos, seed); break;
    case EntityType::Cow: audio::sfx::cow(kind, pos, seed); break;
    case EntityType::Zombie: audio::sfx::zombie(kind, pos, seed); break;
    default: break;
  }
}

}  // namespace

bool badGroundAhead(const world::World& world, const Entity& e, float c, float s) {
  const int ax = static_cast<int>(std::floor(e.pos.x + c * 0.7f));
  const int az = static_cast<int>(std::floor(e.pos.z + s * 0.7f));
  const int fy = static_cast<int>(std::floor(e.pos.y + 0.1f));
  const world::BlockId water = world::wk().water;

  // Water at foot level or just under the lip is a shoreline: steer away.
  if (world.getBlock(ax, fy, az) == water) return true;
  if (world.getBlock(ax, fy - 1, az) == water) return true;

  // Scan down for a cliff: a drop of three or more air blocks, or water below.
  int drop = 0;
  for (int y = fy - 1; y >= fy - 4; --y) {
    const world::BlockId id = world.getBlock(ax, y, az);
    if (id == water) return true;
    if (world::blocks().solid(id)) break;
    ++drop;
  }
  return drop >= 3;
}

bool floatInWater(const world::World& world, Entity& e) {
  const int fy = static_cast<int>(std::floor(e.pos.y + 0.1f));
  if (world.getBlock(static_cast<int>(std::floor(e.pos.x)), fy,
                     static_cast<int>(std::floor(e.pos.z))) != world::wk().water) {
    return false;
  }
  if (e.vel.y < 2.2f) e.vel.y = 2.2f;  // rise toward the surface
  return true;
}

float attackDamage(const Inventory& inventory, const Player* attacker) {
  const ItemStack& slot = inventory.selectedSlot();
  const ItemDef* it = slot.empty() ? nullptr : getItem(slot.key);
  float dmg = 1.5f;
  if (it && it->type == ItemType::Tool && it->toolType == ToolKind::Sword) {
    dmg = 3.0f + it->tier;
  } else if (it && it->type == ItemType::Tool) {
    dmg = 2.0f;
  }
  if (attacker && attacker->velocity().y < -1.0f && !attacker->onGround() &&
      !attacker->flying() && !attacker->swimming() && !attacker->climbing()) {
    dmg *= 1.5f;
    audio::sfx::crit();
  }
  return dmg;
}

bool stuck(Entity& e, float dt, float wantSpeed, float limit) {
  EntityData& d = e.data;
  // The first call has no sample to compare against, so it reports no movement —
  // which is what the JS's `d._sx != null ? ... : 0` did, and it matters: a mob
  // spawned against a wall starts accumulating stuck time immediately.
  const float mx = d.haveSample ? e.pos.x - d.sampleX : 0.0f;
  const float mz = d.haveSample ? e.pos.z - d.sampleZ : 0.0f;
  d.sampleX = e.pos.x;
  d.sampleZ = e.pos.z;
  d.haveSample = true;

  if (wantSpeed > 0.0f && e.onGround &&
      std::sqrt(mx * mx + mz * mz) < wantSpeed * dt * 0.25f) {
    d.stuckT += dt;
    if (d.stuckT > limit) {
      d.stuckT = 0.0f;
      return true;
    }
  } else {
    d.stuckT = 0.0f;
  }
  return false;
}

void steerHeading(Entity& e, float dt, EntityContext& ctx, float speed, bool inWater) {
  EntityData& d = e.data;
  const bool jammed = stuck(e, dt, (e.onGround || inWater) ? speed : 0.0f);
  if (speed > 0.0f && (e.onGround || inWater)) {
    const float c = std::cos(d.heading), s = std::sin(d.heading);
    if (!inWater && badGroundAhead(*ctx.world, e, c, s)) {
      d.heading += kPi * R(0.5f, 1.0f);  // steer off the shoreline or cliff
      d.changeT = std::min(d.changeT, R(1.0f, 2.0f));
    } else if (jammed) {
      d.heading += kPi * R(0.35f, 1.15f);  // a wall: turn away
      d.changeT = std::min(d.changeT, R(1.0f, 2.0f));
    } else {
      e.vel.x = c * speed;  // hills are climbed by the manager's auto-step
      e.vel.z = s * speed;
    }
  }
  // The model's head points local +z, which the model matrix maps to world
  // (sin yaw, cos yaw); travel is (cos h, sin h), so yaw = pi/2 - heading.
  e.yaw = kPi / 2.0f - d.heading;
}

void wanderStep(Entity& e, float dt, EntityContext& ctx, float speed, bool inWater) {
  EntityData& d = e.data;

  if (d.hasFollower && !inWater) {
    stuck(e, dt, 0.0f);  // keep the watchdog's samples fresh
    const PathFollower::Status st = d.follower.step(e, dt, speed);
    if (st == PathFollower::Status::Moving) return;
    d.hasFollower = false;  // arrived, or the follower gave up
    d.moving = false;
    d.changeT = st == PathFollower::Status::Stuck ? R(0.2f, 0.6f) : R(1.0f, 3.5f);
    return;
  }
  if (inWater) d.hasFollower = false;  // a path does not survive a dunking

  d.changeT -= dt;
  if (d.changeT <= 0.0f) {
    d.changeT = R(2.0f, 5.0f);
    d.heading = static_cast<float>(randomUnit()) * kPi * 2.0f;
    d.moving = randomUnit() < 0.6;
    if (d.moving && !inWater && ctx.entities) {
      const float r = R(3.0f, 9.0f);
      const Vec3 goal{e.pos.x + std::cos(d.heading) * r, e.pos.y,
                      e.pos.z + std::sin(d.heading) * r};
      PathOptions options;
      options.maxFall = 2;
      options.maxDist = 14;
      options.maxExpand = 96;
      Path path;
      if (ctx.entities->ai().requestPath(e, goal, options, 0.8f, *ctx.world, path) &&
          path.points.size() > 1) {
        d.follower = PathFollower(std::move(path));
        d.hasFollower = true;
        return;
      }
    }
  }
  steerHeading(e, dt, ctx, d.moving ? speed : 0.0f, inWater);
}

void grazeUpdate(Entity& e, float dt, EntityContext& ctx, const GrazeOptions& options) {
  EntityData& d = e.data;
  d.hurtFlash = std::max(0.0f, d.hurtFlash - dt);
  d.flee = std::max(0.0f, d.flee - dt);
  const bool inWater = floatInWater(*ctx.world, e);

  if (d.flee > 0.0f) {  // panic: run, no route planning
    d.hasFollower = false;
    const Vec3 p = ctx.player->pos();
    d.devT = std::max(0.0f, d.devT - dt);
    d.heading = std::atan2(e.pos.z - p.z, e.pos.x - p.x) + (d.devT > 0.0f ? d.devAngle : 0.0f);
    const bool jammed = stuck(e, dt, (e.onGround || inWater) ? options.fleeSpeed : 0.0f);
    if (e.onGround || inWater) {
      const float c = std::cos(d.heading), s = std::sin(d.heading);
      if (jammed || (!inWater && badGroundAhead(*ctx.world, e, c, s))) {
        // Veer to one side and keep running, rather than stalling on the obstacle.
        d.devAngle = (randomUnit() < 0.5 ? 1.0f : -1.0f) * (kPi * 0.5f + R(0.0f, 0.5f));
        d.devT = 0.7f;
      } else {
        e.vel.x = c * options.fleeSpeed;
        e.vel.z = s * options.fleeSpeed;
      }
    }
    e.yaw = kPi / 2.0f - d.heading;
    return;
  }
  wanderStep(e, dt, ctx, options.walkSpeed, inWater);
}

bool grazeHurt(Entity& e, EntityContext& ctx) {
  e.data.health -= attackDamage(*ctx.inventory, ctx.player);
  e.data.hurtFlash = 0.35f;
  e.data.flee = 4.0f;

  const Vec3 vpos{e.pos.x, e.pos.y + e.h * 0.7f, e.pos.z};
  audio::sfx::thwack(vpos);
  const bool fatal = e.data.health <= 0.0f;
  mobVoice(e.type, fatal ? audio::sfx::MobCall::Death : audio::sfx::MobCall::Hurt, vpos, e.id);

  const Vec3 p = ctx.player->pos();
  const float dx = e.pos.x - p.x, dz = e.pos.z - p.z;
  const float l = std::max(1e-4f, std::sqrt(dx * dx + dz * dz));
  e.vel.x += (dx / l) * 5.0f;
  e.vel.z += (dz / l) * 5.0f;
  e.vel.y = 4.5f;

  const ItemStack& slot = ctx.inventory->selectedSlot();
  const ItemDef* held = slot.empty() ? nullptr : getItem(slot.key);
  if (held && held->type == ItemType::Tool) ctx.inventory->damageSelectedTool(1);
  return e.data.health <= 0.0f;
}

}  // namespace hr::game
