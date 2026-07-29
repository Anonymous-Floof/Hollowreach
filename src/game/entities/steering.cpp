#include "game/entities/steering.h"

#include <cmath>

#include "core/prng.h"
#include "game/entities/ai.h"

namespace hr::game {
namespace {
constexpr float kPi = 3.14159265358979f;
}

float seek(const Entity& e, const Vec3& target) {
  return std::atan2(target.z - e.pos.z, target.x - e.pos.x);
}

float flee(const Entity& e, const Vec3& threat) {
  return std::atan2(e.pos.z - threat.z, e.pos.x - threat.x);
}

float arriveSpeed(const Entity& e, const Vec3& target, float speed, float stopRadius,
                  float slowRadius) {
  const float dx = target.x - e.pos.x, dz = target.z - e.pos.z;
  const float d = std::sqrt(dx * dx + dz * dz);
  if (d <= stopRadius) return 0.0f;
  if (d >= slowRadius) return speed;
  return speed * (d - stopRadius) / (slowRadius - stopRadius);
}

void wander(WanderState& state, float dt, const WanderOptions& options, float& heading,
            float& speed) {
  state.timer -= dt;
  if (state.timer <= 0.0f) {
    state.timer = options.minPause + static_cast<float>(randomUnit()) * options.varPause;
    state.heading = static_cast<float>(randomUnit()) * kPi * 2.0f;
    state.moving = randomUnit() < options.moveChance;
  }
  heading = state.heading;
  speed = state.moving ? options.walkSpeed : 0.0f;
}

bool avoidHazards(const world::World& world, const Entity& e, float heading, float& out,
                  int maxProbes) {
  const auto ok = [&](float h) {
    return !badGroundAhead(world, e, std::cos(h), std::sin(h));
  };
  if (ok(heading)) {
    out = heading;
    return true;
  }
  for (int i = 1; i <= maxProbes; ++i) {
    const float off = (kPi / 5.0f) * i;
    if (ok(heading + off)) {
      out = heading + off;
      return true;
    }
    if (ok(heading - off)) {
      out = heading - off;
      return true;
    }
  }
  const float back = heading + kPi;
  if (!ok(back)) return false;
  out = back;
  return true;
}

void separation(const Entity& e, const std::vector<Entity>& entities, float& fx, float& fz,
                float radius, float strength) {
  fx = 0.0f;
  fz = 0.0f;
  for (const Entity& o : entities) {
    if (&o == &e || o.dead || o.type != e.type) continue;
    const float dx = e.pos.x - o.pos.x, dz = e.pos.z - o.pos.z;
    const float d = std::sqrt(dx * dx + dz * dz);
    if (d > radius || d < 1e-4f) continue;
    const float w = (1.0f - d / radius) / d;
    fx += dx * w;
    fz += dz * w;
  }
  fx *= strength;
  fz *= strength;
}

void applyMove(Entity& e, float heading, float speed, bool grounded) {
  if (speed > 0.0f && grounded) {
    e.vel.x = std::cos(heading) * speed;
    e.vel.z = std::sin(heading) * speed;
  }
  e.yaw = kPi / 2.0f - heading;
}

}  // namespace hr::game
