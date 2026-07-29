#include "audio/director.h"

#include <cmath>

#include "audio/ambience.h"
#include "audio/engine.h"
#include "audio/sfx.h"
#include "core/prng.h"
#include "game/entities/manager.h"
#include "game/player.h"
#include "world/world.h"

namespace hr::audio {
namespace {
inline float R(float a, float b) { return a + static_cast<float>(randomUnit()) * (b - a); }
}  // namespace

Director& director() {
  static Director instance;
  return instance;
}

void Director::update(float dt, const DirectorContext& ctx) {
  if (!engine().ready() || !ctx.world || !ctx.player) return;
  const world::World& world = *ctx.world;
  const game::Player& player = *ctx.player;

  engine().updateListener(player.eye(), player.yaw());
  engine().setUnderwater(ctx.underwater);

  AmbienceContext amb;
  amb.world = ctx.world;
  amb.player = ctx.player;
  amb.dayFactor = ctx.dayFactor;
  amb.underwater = ctx.underwater;
  amb.active = ctx.active;
  ambience().update(dt, amb);

  if (!ctx.active) return;

  const Vec3 pos = player.pos();
  const Vec3 vel = player.velocity();
  const auto& blocks = world::BlockRegistry::get();
  const world::BlockId water = world::wk().water;

  // ---- footsteps: a stride-length accumulator over the block underfoot ----
  const float hspeed = std::sqrt(vel.x * vel.x + vel.z * vel.z);
  const bool feetInWater =
      world.getBlock(static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y + 0.05f)),
                     static_cast<int>(std::floor(pos.z))) == water;
  const bool walking = hspeed > 0.6f && !player.flying() &&
                       (player.onGround() || player.climbing()) && !player.swimming();
  if (walking) {
    stride_ += hspeed * dt;
    const float strideLen = player.sprinting() ? 2.4f : 1.9f;
    if (stride_ >= strideLen) {
      stride_ = 0.0f;
      if (feetInWater) {
        sfx::wadeStep();
      } else {
        // The block under the feet gives the step its material.
        const int bx = static_cast<int>(std::floor(pos.x));
        const int bz = static_cast<int>(std::floor(pos.z));
        world::BlockId id = world.getBlock(bx, static_cast<int>(std::floor(pos.y - 0.05f)), bz);
        if (id == world::kAir) {
          id = world.getBlock(bx, static_cast<int>(std::floor(pos.y - 1.05f)), bz);
        }
        if (player.climbing()) id = world::wk().ladder;
        if (id != world::kAir) sfx::step(blocks.def(id), player.sprinting());
      }
    }
  } else {
    stride_ *= 0.9f;
  }

  // ---- splash on entering water (harder fall = bigger splash) ----
  const bool inWater = player.inWater(world);
  if (inWater && !wasInWater_) sfx::splash(vel.y < -6.0f);
  wasInWater_ = inWater;

  // ---- ambient mob calls: each nearby mob keeps its own randomised clock ----
  mobCallCooldown_ -= dt;
  if (!ctx.entities) return;
  for (game::Entity& e : ctx.entities->all()) {
    if (e.dead) continue;
    // How far apart, in seconds, this type's idle calls schedule.
    float gapLo = 0.0f, gapHi = 0.0f;
    switch (e.type) {
      case game::EntityType::Sheep: gapLo = 8.0f; gapHi = 22.0f; break;
      case game::EntityType::Pig: gapLo = 7.0f; gapHi = 20.0f; break;
      case game::EntityType::Cow: gapLo = 11.0f; gapHi = 28.0f; break;
      case game::EntityType::Zombie: gapLo = 5.0f; gapHi = 14.0f; break;
      default: continue;
    }
    const float dx = e.pos.x - pos.x, dz = e.pos.z - pos.z;
    if (dx * dx + dz * dz > 24.0f * 24.0f) continue;

    if (e.data.callT < 0.0f) e.data.callT = R(1.5f, gapHi);
    e.data.callT -= dt;
    if (e.data.callT > 0.0f) continue;
    e.data.callT = R(gapLo, gapHi);
    if (mobCallCooldown_ > 0.0f) continue;  // never a whole choir at once
    mobCallCooldown_ = 0.9f;

    const Vec3 vpos{e.pos.x, e.pos.y + e.h * 0.7f, e.pos.z};
    switch (e.type) {
      case game::EntityType::Sheep: sfx::sheep(sfx::MobCall::Say, vpos, e.id); break;
      case game::EntityType::Pig: sfx::pig(sfx::MobCall::Say, vpos, e.id); break;
      case game::EntityType::Cow: sfx::cow(sfx::MobCall::Say, vpos, e.id); break;
      case game::EntityType::Zombie: sfx::zombie(sfx::MobCall::Say, vpos, e.id); break;
      default: break;
    }
  }
}

void Director::stop() {
  ambience().quiet();
  stride_ = 0.0f;
  wasInWater_ = false;
}

}  // namespace hr::audio
