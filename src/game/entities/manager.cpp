#include "game/entities/manager.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/prng.h"
#include "game/physics.h"
#include "game/player.h"
#include "world/blocks.h"
#include "world/chunk.h"
#include "world/world.h"

namespace hr::game {
namespace {

constexpr float kMobStep = 1.0f;  // walking mobs auto-climb a full block
constexpr float kEntityFlow = 26.0f;  // how hard a current drags a floating entity

// Slab-method ray versus AABB. Returns the entry distance, or false for a miss.
bool rayAABB(const Vec3& o, const Vec3& d, const Vec3& lo, const Vec3& hi, float& tOut) {
  const float origin[3] = {o.x, o.y, o.z};
  const float dir[3] = {d.x, d.y, d.z};
  const float low[3] = {lo.x, lo.y, lo.z};
  const float high[3] = {hi.x, hi.y, hi.z};

  float tmin = 0.0f;
  float tmax = std::numeric_limits<float>::infinity();
  for (int i = 0; i < 3; ++i) {
    if (std::fabs(dir[i]) < 1e-8f) {
      if (origin[i] < low[i] || origin[i] > high[i]) return false;
      continue;
    }
    float t1 = (low[i] - origin[i]) / dir[i];
    float t2 = (high[i] - origin[i]) / dir[i];
    if (t1 > t2) std::swap(t1, t2);
    tmin = std::max(tmin, t1);
    tmax = std::min(tmax, t2);
    if (tmin > tmax) return false;
  }
  tOut = tmin;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// AIServices
// ---------------------------------------------------------------------------

void AIServices::tick(float dt, const EntityContext& ctx) {
  budget_.left = kBudgetPerTick;
  sounds_.tick(dt);
  scent_.tick(dt);
  // The player lays scent continuously while alive — grounded or not, since a
  // trail over a jump is still a trail.
  scentT_ -= dt;
  if (scentT_ <= 0.0f && ctx.player && ctx.player->health() > 0.0f) {
    scentT_ = kScentPeriod;
    scent_.deposit(ctx.player->pos(), "player");
  }
}

bool AIServices::requestPath(Entity& e, const Vec3& goal, const PathOptions& options,
                             float cooldown, const world::World& world, Path& out) {
  // The scent field's clock doubles as a monotonic AI clock.
  if (e.data.pathNext >= 0.0 && e.data.pathNext > scent_.now()) return false;
  e.data.pathNext = scent_.now() + cooldown;
  return findPath(world, e.pos, goal, options, &budget_, out);
}

// ---------------------------------------------------------------------------
// EntityManager
// ---------------------------------------------------------------------------

Entity* EntityManager::spawn(EntityType type, const Vec3& pos) {
  const EntityDef* def = defOf(type);
  if (!def) return nullptr;

  Entity e;
  e.id = nextId_++;
  e.type = type;
  e.pos = pos;
  e.hw = def->hw;
  e.h = def->h;
  entities_.push_back(std::move(e));
  Entity& out = entities_.back();
  if (def->spawn) def->spawn(out);
  return &out;
}

Entity* EntityManager::spawnGhost(int netId, EntityType type, const Vec3& pos) {
  const EntityDef* def = defOf(type);
  Entity e;
  e.id = -netId - 1;  // negative, so a ghost id can never collide with a local one
  e.netId = netId;
  e.type = type;
  e.ghost = true;
  e.pos = pos;
  e.hw = def ? def->hw : 0.3f;
  e.h = def ? def->h : 1.8f;
  entities_.push_back(std::move(e));
  return &entities_.back();
}

Entity* EntityManager::byId(int id) {
  for (Entity& e : entities_) {
    if (e.id == id && !e.dead) return &e;
  }
  return nullptr;
}

int EntityManager::count() const {
  int n = 0;
  for (const Entity& e : entities_) {
    if (!e.dead) ++n;
  }
  return n;
}

void EntityManager::tick(float dt, EntityContext& ctx) {
  world::World& world = *ctx.world;
  ai_.tick(dt, ctx);  // refresh the path budget, decay sounds, lay player scent

  bool anyDead = false;
  // Indexed rather than ranged: an update hook can spawn (a drop, on death), and
  // that reallocates the vector out from under an iterator. New entities are not
  // ticked until the next frame, which is what the JS did by appending too.
  const std::size_t liveCount = entities_.size();
  for (std::size_t i = 0; i < liveCount; ++i) {
    if (entities_[i].dead) {
      anyDead = true;
      continue;
    }
    // Ghosts are driven by remote snapshots, never simulated here.
    if (entities_[i].ghost) continue;

    // Hostiles that have got a long way from the player are removed rather than
    // frozen. This has to come BEFORE the unloaded-chunk skip below, because a
    // far mob is in an unloaded chunk almost by definition — it is exactly the
    // one the skip would leave sitting there forever, holding a spawn slot.
    // Nothing else despawns: an animal you walked home stays where you put it.
    // Measured from the host's own player in a multiplayer world, which is the
    // same player the spawner works around — one limitation, not two.
    {
      const EntityDef* d = defOf(entities_[i].type);
      if (d && d->flags.hostile && ctx.player) {
        const Vec3 p = ctx.player->pos();
        const float dx = entities_[i].pos.x - p.x;
        const float dy = entities_[i].pos.y - p.y;
        const float dz = entities_[i].pos.z - p.z;
        if (dx * dx + dy * dy + dz * dz > kHostileDespawn * kHostileDespawn) {
          entities_[i].dead = true;
          anyDead = true;
          continue;
        }
      }
    }

    // Freeze entities whose chunk is not loaded — a death drop you walked away
    // from stays put, with no physics and no ageing, until you come back.
    {
      const Entity& e = entities_[i];
      const int cx = static_cast<int>(std::floor(e.pos.x)) >> 4;
      const int cz = static_cast<int>(std::floor(e.pos.z)) >> 4;
      if (!world.chunkReady(cx, cz)) continue;
    }

    const EntityDef* def = defOf(entities_[i].type);
    if (!def) continue;
    entities_[i].age += dt;

    if (def->physics) {
      Entity& e = entities_[i];
      if (def->gravity != 0.0f) e.vel.y -= def->gravity * dt;

      // A water current carries floating and submerged entities downstream;
      // gravity-bound ones also get partial buoyancy so they bob rather than sink.
      const int bx = static_cast<int>(std::floor(e.pos.x));
      const int bz = static_cast<int>(std::floor(e.pos.z));
      const int by = static_cast<int>(std::floor(e.pos.y + e.h * 0.5f));
      if (world.getBlock(bx, by, bz) == world::wk().water) {
        if (def->gravity != 0.0f) {
          e.vel.y += def->gravity * dt * 0.65f;
          if (e.vel.y < -1.4f) e.vel.y = -1.4f;
        }
        float fx = 0.0f, fz = 0.0f;
        world.waterFlow(bx, by, bz, fx, fz);
        e.vel.x += fx * kEntityFlow * dt;
        e.vel.z += fz * kEntityFlow * dt;
        e.vel.x *= 0.92f;  // drag keeps the drift bounded
        e.vel.z *= 0.92f;
      }

      const bool grounded = e.onGround;  // last frame's footing gates the auto-step
      e.onGround = false;

      Body body{e.pos, e.hw, e.h};
      if (grounded && def->flags.ai) {
        stepSweep(world, body, 0, e.vel.x * dt, kMobStep);
        stepSweep(world, body, 2, e.vel.z * dt, kMobStep);
      } else {
        sweepAxis(world, body, 0, e.vel.x * dt);
        sweepAxis(world, body, 2, e.vel.z * dt);
      }
      const bool hitY = sweepAxis(world, body, 1, e.vel.y * dt);
      e.pos = body.pos;
      if (hitY) {
        if (e.vel.y < 0.0f) e.onGround = true;
        e.vel.y = 0.0f;
      }
      if (e.onGround) {  // ground friction
        e.vel.x *= 0.5f;
        e.vel.z *= 0.5f;
      }
    }

    if (def->update) def->update(entities_[i], dt, ctx);
    if (entities_[i].dead) anyDead = true;
  }

  if (anyDead || entities_.size() > liveCount) {
    entities_.erase(std::remove_if(entities_.begin(), entities_.end(),
                                   [](const Entity& e) { return e.dead; }),
                    entities_.end());
  }
}

namespace {

inline float R(float a, float b) { return a + static_cast<float>(randomUnit()) * (b - a); }

// Was a copy of the same walk; it lives on World now, because the wayshard asks
// the same question and two answers to "where is the ground here" is one too many.
// Note the old copy used `wx >> 4` for the chunk, which is right for negatives by
// accident of arithmetic shift and wrong the moment anyone changes CX.
int topSolidY(const world::World& world, int wx, int wz) { return world.topSolidY(wx, wz); }

}  // namespace

Entity* EntityManager::spawnDrop(const Vec3& pos, const std::string& key, int count, int dura) {
  Entity* e = spawn(EntityType::Drop, pos);
  if (!e) return nullptr;
  e->data.key = key;
  e->data.count = count;
  e->data.dura = dura;
  e->data.instant = true;
  if (const EntityDef* def = defOf(EntityType::Drop); def && def->spawn) def->spawn(*e);
  e->vel = Vec3{R(-1.0f, 1.0f), 2.2f, R(-1.0f, 1.0f)};
  return e;
}

Entity* EntityManager::spawnTossed(const Vec3& pos, const Vec3& dir, const std::string& key,
                                   int count, int dura) {
  Entity* e = spawn(EntityType::Drop, pos);
  if (!e) return nullptr;
  e->data.key = key;
  e->data.count = count;
  e->data.dura = dura;
  e->data.instant = false;
  if (const EntityDef* def = defOf(EntityType::Drop); def && def->spawn) def->spawn(*e);
  e->vel = Vec3{dir.x * 5.0f + R(-0.5f, 0.5f), 3.0f + dir.y * 3.0f,
                dir.z * 5.0f + R(-0.5f, 0.5f)};
  return e;
}

void EntityManager::trySpawnGrazer(world::World& world, EntityType type, float px, float pz,
                                   float dayFactor, int cap) {
  if (dayFactor < 0.4f || randomUnit() > 0.5) return;
  int n = 0;
  for (const Entity& e : entities_) {
    if (e.type == type && !e.dead) ++n;
  }
  if (n >= cap) return;

  for (int t = 0; t < 12; ++t) {
    const float ang = static_cast<float>(randomUnit()) * 6.28318530718f;
    const float dist = R(16.0f, 32.0f);
    const int wx = static_cast<int>(std::floor(px + std::cos(ang) * dist));
    const int wz = static_cast<int>(std::floor(pz + std::sin(ang) * dist));
    const int ts = topSolidY(world, wx, wz);
    if (ts < 0 || world.getBlock(wx, ts, wz) != world::wk().turf) continue;
    if (world.getBlock(wx, ts + 1, wz) != world::kAir) continue;
    if (world.getBlock(wx, ts + 2, wz) != world::kAir) continue;
    spawn(type, Vec3{wx + 0.5f, static_cast<float>(ts + 1), wz + 0.5f});
    return;
  }
}

int effectiveLight(const world::World& world, int wx, int wy, int wz, float dayFactor) {
  if (!world.lightReady(world::World::floorDiv16(wx), world::World::floorDiv16(wz))) return 15;
  // Truncation, not rounding: skylight has to fall the whole way to zero before a
  // cell counts as dark, so the surface stops being spawnable at the first hint of
  // dawn rather than halfway through it. 15 * dayFactor reaches 1 at a day factor
  // of 0.067, which is a sun still below the horizon.
  const int sky = static_cast<int>(static_cast<float>(world.getSky(wx, wy, wz)) * dayFactor);
  return std::max(world.getBlockLight(wx, wy, wz), sky);
}

bool monsterSpawnable(const world::World& world, int wx, int wy, int wz, float dayFactor) {
  if (wy < 1 || wy + 1 >= world::WH) return false;
  // Footing. `solid` already excludes water and every plant, and an unloaded
  // chunk reads as air — so this one test is also what keeps a spawn out of the
  // sea, out of a flower bed, and out of terrain that does not exist yet.
  if (!world::blocks().solid(world.getBlock(wx, wy - 1, wz))) return false;
  // Room for the body. Air, strictly: water here would drown it, and a slab or a
  // ladder would leave it clipping.
  if (world.getBlock(wx, wy, wz) != world::kAir) return false;
  if (world.getBlock(wx, wy + 1, wz) != world::kAir) return false;
  return effectiveLight(world, wx, wy, wz, dayFactor) == 0;
}

void EntityManager::trySpawnZombie(world::World& world, const Vec3& player, float dayFactor) {
  if (randomUnit() > 0.5) return;
  int n = 0;
  for (const Entity& e : entities_) {
    if (e.type == EntityType::Zombie && !e.dead) ++n;
  }
  if (n >= kMaxZombies) return;

  // Darkness is the whole rule now, so the search cannot be "walk down to the
  // surface" any more — the surface is the one place that is lit at noon. Pick a
  // column in the ring, sweep a band of it from well below the player to a little
  // above, and take one of the dark ledges it passes.
  //
  // Reservoir sampling rather than "first hit going up": a column through a
  // cave system offers several floors, and always taking the lowest would put
  // every spawn at the bottom of the deepest cavern under you.
  const int loY = std::max(1, static_cast<int>(std::floor(player.y)) - kSpawnBandDown);
  const int hiY = std::min(world::WH - 2, static_cast<int>(std::floor(player.y)) + kSpawnBandUp);

  for (int t = 0; t < 12; ++t) {
    const float ang = static_cast<float>(randomUnit()) * 6.28318530718f;
    const float dist = R(20.0f, 40.0f);
    const int wx = static_cast<int>(std::floor(player.x + std::cos(ang) * dist));
    const int wz = static_cast<int>(std::floor(player.z + std::sin(ang) * dist));

    int chosen = -1;
    int seen = 0;
    for (int wy = loY; wy <= hiY; ++wy) {
      if (!monsterSpawnable(world, wx, wy, wz, dayFactor)) continue;
      ++seen;
      if (randomUnit() * seen < 1.0) chosen = wy;
    }
    if (chosen < 0) continue;
    spawn(EntityType::Zombie, Vec3{wx + 0.5f, static_cast<float>(chosen), wz + 0.5f});
    return;
  }
}

void EntityManager::tickEvilAltars(world::World& world, const Vec3& player, float dayFactor) {
  const world::BlockId altar = world::blocks().idOf("evil_altar");
  if (altar == world::kAir) return;

  const int px = static_cast<int>(std::floor(player.x));
  const int py = static_cast<int>(std::floor(player.y));
  const int pz = static_cast<int>(std::floor(player.z));

  // A box scan rather than a list of altars kept somewhere. It is 15^3 reads on a
  // four-second timer, which is nothing, and it costs no save format, no block
  // entity and no bookkeeping to go stale — an altar mined out of the world stops
  // working because it is no longer there to be found.
  for (int dy = -kAltarRange; dy <= kAltarRange; ++dy) {
    const int ay = py + dy;
    if (ay < 0 || ay >= world::WH) continue;
    for (int dz = -kAltarRange; dz <= kAltarRange; ++dz) {
      for (int dx = -kAltarRange; dx <= kAltarRange; ++dx) {
        if (world.getBlock(px + dx, ay, pz + dz) != altar) continue;
        spawnFromAltar(world, px + dx, ay, pz + dz, dayFactor);
      }
    }
  }
}

void EntityManager::spawnFromAltar(world::World& world, int ax, int ay, int az,
                                   float dayFactor) {
  // Its own crowd limit, counted in a sphere around the altar. This is what makes
  // an altar a threat instead of a trickle: the world cap of two would otherwise
  // be spent on whatever the night had already produced elsewhere.
  // `crowd`, not `near`: `near` and `far` are macros in the older Windows headers
  // and this file is one include away from meeting them.
  int crowd = 0;
  for (const Entity& e : entities_) {
    if (e.dead || e.type != EntityType::Zombie) continue;
    const float dx = e.pos.x - (ax + 0.5f);
    const float dy = e.pos.y - static_cast<float>(ay);
    const float dz = e.pos.z - (az + 0.5f);
    if (dx * dx + dy * dy + dz * dz <= kAltarCrowdRadius * kAltarCrowdRadius) ++crowd;
  }
  if (crowd >= kAltarCrowd) return;

  // Four tries for one zombie, in the 9x3x9 box Minecraft's spawner uses. Note
  // this goes through monsterSpawnable like everything else, so an altar in a
  // torch-lit room is inert — the same way a player disarms a spawner by lighting
  // the room rather than by digging it out.
  for (int t = 0; t < 4; ++t) {
    const int wx = ax + static_cast<int>(std::floor(R(-4.0f, 5.0f)));
    const int wy = ay + static_cast<int>(std::floor(R(-1.0f, 2.0f)));
    const int wz = az + static_cast<int>(std::floor(R(-4.0f, 5.0f)));
    if (!monsterSpawnable(world, wx, wy, wz, dayFactor)) continue;
    spawn(EntityType::Zombie, Vec3{wx + 0.5f, static_cast<float>(wy), wz + 0.5f});
    return;
  }
}

bool EntityManager::raycast(const Vec3& origin, const Vec3& dir, float maxDist,
                            EntityRayHit& out) {
  Entity* best = nullptr;
  float bestT = maxDist;
  for (Entity& e : entities_) {
    if (e.dead) continue;
    const Vec3 lo{e.pos.x - e.hw, e.pos.y, e.pos.z - e.hw};
    const Vec3 hi{e.pos.x + e.hw, e.pos.y + e.h, e.pos.z + e.hw};
    float t = 0.0f;
    if (rayAABB(origin, dir, lo, hi, t) && t < bestT) {
      bestT = t;
      best = &e;
    }
  }
  if (!best) return false;
  out.entity = best;
  out.dist = bestT;
  return true;
}

// ---- save / load -----------------------------------------------------------

std::vector<EntitySave> EntityManager::serialize() const {
  std::vector<EntitySave> out;
  out.reserve(entities_.size());
  for (const Entity& e : entities_) {
    if (e.dead || e.ghost) continue;  // ghosts belong to the network, not the save
    EntitySave s;
    s.type = e.type;
    s.pos = e.pos;
    s.vel = e.vel;
    s.yaw = e.yaw;
    s.key = e.data.key;
    s.count = e.data.count;
    s.dura = e.data.dura;
    s.despawn = e.data.despawn;
    s.instant = e.data.instant;
    s.health = e.data.health;
    s.fsm = e.data.fsm;
    out.push_back(std::move(s));
  }
  return out;
}

void EntityManager::load(const std::vector<EntitySave>& saved) {
  entities_.clear();
  for (const EntitySave& s : saved) {
    const EntityDef* def = defOf(s.type);
    if (!def) continue;  // a type this build no longer has: drop it, don't fail
    Entity* e = spawn(s.type, s.pos);
    if (!e) continue;
    e->vel = s.vel;
    e->yaw = s.yaw;
    e->data.key = s.key;
    e->data.count = s.count;
    e->data.dura = s.dura;
    e->data.despawn = s.despawn;
    e->data.instant = s.instant;
    e->data.health = s.health;
    e->data.fsm = s.fsm;
    // The type's own fix-ups, after everything is in place.
    if (def->load) def->load(*e);
  }
}

}  // namespace hr::game
