// Shared movement for walking mobs, ported from js/game/entities/ai.js.
//
// Kept in one place so every land animal climbs hills the same way, none of them
// strolls into the ocean, and the passive grazers share one wander/flee brain.
//
// Wandering leans on the pathfinder: a mob plans a short A* route (budgeted
// through AIServices) to a spot that actually exists, rather than blindly walking
// a heading into a wall, and a stuck watchdog turns it away from whatever the
// auto-step cannot climb. The heavier backends — state machines, hearing, scent —
// stay dormant.

#pragma once

#include "game/entities/entity.h"

namespace hr::game {

// Would stepping along heading (c, s) put the mob into water or over a steep drop?
// Land mobs ask this every tick and turn away instead of drowning or falling in.
bool badGroundAhead(const world::World& world, const Entity& e, float c, float s);

// Gentle buoyancy: a mob that ends up in water — knockback, bad luck — bobs up and
// can paddle to shore instead of sinking to the bottom. True when in water.
bool floatInWater(const world::World& world, Entity& e);

// Damage a player's left click deals to a mob: swords hardest, other tools a
// little, fists least. A Minecraft-style CRIT applies when the attacker is falling
// mid-swing — not grounded, not flying, swimming or climbing — for x1.5 and a
// sharper snap.
float attackDamage(const Inventory& inventory, const Player* attacker);

// Stuck watchdog: true when the mob wanted to move but its feet barely did for a
// while, so it is time to pick a new direction instead of grinding against a
// two-block wall or a tree trunk. Call exactly once per tick — it records the
// position sample it compares against.
bool stuck(Entity& e, float dt, float wantSpeed, float limit = 0.55f);

// Walk along the entity's heading, turning away from shorelines, cliffs and (via
// the watchdog) walls. Sets velocity and yaw.
void steerHeading(Entity& e, float dt, EntityContext& ctx, float speed, bool inWater);

// One tick of idle wandering, shared by the grazers and the off-duty zombie.
void wanderStep(Entity& e, float dt, EntityContext& ctx, float speed, bool inWater);

struct GrazeOptions {
  float walkSpeed = 1.5f;
  float fleeSpeed = 3.0f;
};

// The passive-grazer brain: path-assisted wandering, and a flat run away from the
// player when spooked, veering around obstacles instead of pinning against them.
void grazeUpdate(Entity& e, float dt, EntityContext& ctx, const GrazeOptions& options);

// The shared "got hit by the player" reaction: damage, hurt flash, flee, knockback,
// tool wear, and the mob's own voice. True when the hit was fatal.
bool grazeHurt(Entity& e, EntityContext& ctx);

}  // namespace hr::game
