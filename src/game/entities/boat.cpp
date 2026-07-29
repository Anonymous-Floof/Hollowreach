// Rideable boat, ported from js/game/entities/boat.js — the first use of the
// entity system's `rideable` slot.
//
//  * Right-click to mount, Shift to dismount.
//  * Left-click an empty boat to break it back into a boat item.
//  * Floats on water (a buoyancy spring pulls it to the surface), trudges on land.
//  * While ridden it moves where the player looks, W/S throttle and A/D strafe,
//    and carries the player in its seat.
//
// Vertical motion is handled here (the definition's gravity is 0) so the buoyancy
// spring and land gravity do not fight the manager's own. Horizontal velocity is
// set here too and swept by the manager next tick.

#include <cmath>

#include "core/input.h"
#include "game/entities/types.h"
#include "game/physics.h"
#include "game/player.h"
#include "world/blocks.h"
#include "world/world.h"

namespace hr::game {
namespace {

constexpr float kWaterSpeed = 6.0f;
constexpr float kLandSpeed = 1.5f;

// Read WASD relative to the rider's look; returns the horizontal velocity and
// whether the hull is sitting in water.
void steer(const Entity& e, const Player& player, const Input& input, const world::World& world,
           float& vx, float& vz, bool& inWater) {
  const int wx = static_cast<int>(std::floor(e.pos.x));
  const int wz = static_cast<int>(std::floor(e.pos.z));
  const int by = static_cast<int>(std::floor(e.pos.y + 0.05f));
  inWater = world.getBlock(wx, by, wz) == world::wk().water;

  const float sn = std::sin(player.yaw()), cs = std::cos(player.yaw());
  float fx = 0.0f, fz = 0.0f;
  if (input.down(Key::W)) { fx -= sn; fz -= cs; }
  if (input.down(Key::S)) { fx += sn; fz += cs; }
  if (input.down(Key::A)) { fx -= cs; fz += sn; }
  if (input.down(Key::D)) { fx += cs; fz -= sn; }
  const float len = std::sqrt(fx * fx + fz * fz);
  if (len > 0.0f) {
    fx /= len;
    fz /= len;
  }
  const float speed = inWater ? kWaterSpeed : kLandSpeed;
  vx = fx * speed;
  vz = fz * speed;
}

// Vertical: a buoyancy spring toward the water surface, else gravity.
void buoyancy(Entity& e, float dt, const world::World& world, bool inWater) {
  if (!inWater) {
    e.vel.y -= 24.0f * dt;
    return;
  }
  const int wx = static_cast<int>(std::floor(e.pos.x));
  const int wz = static_cast<int>(std::floor(e.pos.z));
  int sy = static_cast<int>(std::floor(e.pos.y + 0.05f));
  while (world.getBlock(wx, sy + 1, wz) == world::wk().water) ++sy;
  const float target = (sy + 0.85f) - 0.18f;  // the hull rides just below the surface
  e.vel.y = (target - e.pos.y) * 8.0f;
  e.vel.y = std::max(-4.0f, std::min(4.0f, e.vel.y));
}

void dismount(Entity& e, EntityContext& ctx) {
  Player& p = *ctx.player;
  p.setMount(0);
  e.data.rider = false;
  // Step off to the side the player is facing, lifted clear of the hull.
  const float sn = std::sin(p.yaw()), cs = std::cos(p.yaw());
  p.setPos(Vec3{e.pos.x - sn * 0.9f, e.pos.y + 0.6f, e.pos.z - cs * 0.9f});
  p.setVelocity(Vec3{});
  if (ctx.notify) ctx.notify("Dismounted");
}

void boatSpawn(Entity& e) { e.data.rider = false; }

void boatUpdate(Entity& e, float dt, EntityContext& ctx) {
  world::World& world = *ctx.world;

  if (e.data.rider && ctx.player && ctx.input) {
    Player& p = *ctx.player;
    if (ctx.input->pressed(Key::ShiftLeft)) {
      dismount(e, ctx);
    } else {
      float vx = 0.0f, vz = 0.0f;
      bool inWater = false;
      steer(e, p, *ctx.input, world, vx, vz, inWater);
      e.vel.x = vx;
      e.vel.z = vz;
      e.yaw = p.yaw();
      p.setPos(Vec3{e.pos.x, e.pos.y + kBoatSeatY, e.pos.z});  // carry them in the seat
      p.setVelocity(Vec3{});
    }
  } else {
    e.vel.x *= 0.92f;  // unridden: coast to a stop
    e.vel.z *= 0.92f;
  }

  const int wx = static_cast<int>(std::floor(e.pos.x));
  const int wz = static_cast<int>(std::floor(e.pos.z));
  const int by = static_cast<int>(std::floor(e.pos.y + 0.05f));
  buoyancy(e, dt, world, world.getBlock(wx, by, wz) == world::wk().water);
}

void boatInteract(Entity& e, EntityContext& ctx, InteractButton button) {
  if (e.data.rider) return;  // occupied: leave it alone
  if (button == InteractButton::Right) {
    e.data.rider = true;
    ctx.player->setMount(e.id);
    if (ctx.notify) ctx.notify("Riding a boat — Shift to dismount");
  } else {
    ctx.world->spawnDrop(e.pos.x, e.pos.y + 0.2f, e.pos.z, "boat", 1);
    e.dead = true;
  }
}

// Nobody is aboard a boat that has just been loaded: the rider is the player, and
// the player is restored separately, standing.
void boatLoad(Entity& e) { e.data.rider = false; }

}  // namespace

const EntityDef kBoatDef{
    "boat", 0.55f, 0.42f, /*physics=*/true, /*gravity=*/0.0f,
    EntityFlags{false, false, false, false, /*rideable=*/true},
    boatSpawn, boatUpdate, boatInteract, boatLoad,
};

}  // namespace hr::game
