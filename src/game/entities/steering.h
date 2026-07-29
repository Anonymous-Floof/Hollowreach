// Steering helpers for mob movement, ported from js/game/entities/ai/steering.js.
//
// AI backend: built for future advanced mobs, and deliberately not wired into the
// current ones. Everything here computes a HEADING and a SPEED; the caller applies
// them to velocity and yaw, or hands the pair to applyMove. They compose — pick a
// base heading with seek/flee/wander, then filter it with avoidHazards or push it
// apart with separation.
//
// The convention matches the shipped mobs: a heading h means travelling along
// (cos h, sin h) on xz, and the model yaw is pi/2 - h.

#pragma once

#include <vector>

#include "game/entities/entity.h"

namespace hr::game {

// Heading straight toward, or straight away from, a world point.
float seek(const Entity& e, const Vec3& target);
float flee(const Entity& e, const Vec3& threat);

// Speed ramp-down near a target, so a mob settles instead of orbiting it: full
// speed outside slowRadius, proportional inside, zero within stopRadius.
float arriveSpeed(const Entity& e, const Vec3& target, float speed, float stopRadius = 1.2f,
                  float slowRadius = 3.0f);

// The classic grazer amble, with its own state so it can drive anything.
struct WanderState {
  float timer = 0.0f;
  float heading = 0.0f;
  bool moving = false;
};
struct WanderOptions {
  float walkSpeed = 1.2f;
  float moveChance = 0.6f;
  float minPause = 2.0f;
  float varPause = 3.0f;
};
void wander(WanderState& state, float dt, const WanderOptions& options, float& heading,
            float& speed);

// Turns a desired heading away from shorelines and cliffs: probes the desired
// direction, then fans left and right in widening arcs, and finally reverses — so
// a chasing mob skirts a hazard instead of stopping dead at it. False when every
// direction is bad and the caller should idle.
bool avoidHazards(const world::World& world, const Entity& e, float heading, float& out,
                  int maxProbes = 5);

// A soft push apart, so a pack does not stack into one column. Returns a force on
// xz away from nearby entities of the same type.
void separation(const Entity& e, const std::vector<Entity>& entities, float& fx, float& fz,
                float radius = 1.2f, float strength = 2.5f);

// Applies a heading and speed the way the shipped mobs do — velocity when grounded
// or afloat, plus the model yaw — so a state machine has one idiom to call.
void applyMove(Entity& e, float heading, float speed, bool grounded = true);

}  // namespace hr::game
