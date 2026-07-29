#include "game/entities/senses.h"

#include <algorithm>
#include <cmath>

#include "core/prng.h"
#include "game/entities/entity.h"
#include "game/entities/manager.h"
#include "game/player.h"
#include "world/blocks.h"
#include "world/world.h"

namespace hr::game {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kSoundLife = 2.5f;

inline int sign(float v) { return v > 0.0f ? 1 : (v < 0.0f ? -1 : 0); }

}  // namespace

bool lineOfSight(const world::World& world, const Vec3& a, const Vec3& b, float maxDist) {
  const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
  const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (dist < 1e-6f) return true;
  if (dist > maxDist) return false;

  const float d[3] = {dx / dist, dy / dist, dz / dist};
  const float origin[3] = {a.x, a.y, a.z};
  int cell[3] = {static_cast<int>(std::floor(a.x)), static_cast<int>(std::floor(a.y)),
                 static_cast<int>(std::floor(a.z))};
  const int start[3] = {cell[0], cell[1], cell[2]};
  const int end[3] = {static_cast<int>(std::floor(b.x)), static_cast<int>(std::floor(b.y)),
                      static_cast<int>(std::floor(b.z))};

  const float kInf = std::numeric_limits<float>::infinity();
  float tDelta[3], tMax[3];
  int step[3];
  for (int i = 0; i < 3; ++i) {
    step[i] = sign(d[i]);
    tDelta[i] = d[i] != 0.0f ? std::fabs(1.0f / d[i]) : kInf;
    if (step[i] == 0) {
      tMax[i] = kInf;
    } else {
      const float bound = step[i] > 0 ? (std::floor(origin[i]) + 1.0f - origin[i])
                                      : (origin[i] - std::floor(origin[i]));
      tMax[i] = bound * tDelta[i];
    }
  }

  float t = 0.0f;
  while (t <= dist) {
    if (cell[0] == end[0] && cell[1] == end[1] && cell[2] == end[2]) return true;
    // The cell the ray starts in never blocks: a mob standing in tall grass, or
    // with its eye inside a slab, can still see out.
    const bool atStart = cell[0] == start[0] && cell[1] == start[1] && cell[2] == start[2];
    if (!atStart && world::blocks().solid(world.getBlock(cell[0], cell[1], cell[2]))) return false;

    if (tMax[0] < tMax[1] && tMax[0] < tMax[2]) {
      cell[0] += step[0];
      t = tMax[0];
      tMax[0] += tDelta[0];
    } else if (tMax[1] < tMax[2]) {
      cell[1] += step[1];
      t = tMax[1];
      tMax[1] += tDelta[1];
    } else {
      cell[2] += step[2];
      t = tMax[2];
      tMax[2] += tDelta[2];
    }
  }
  return true;
}

// ---------------------------------------------------------------------------

void SoundBus::emit(const Vec3& pos, float loudness, const char* type) {
  events_.push_back(SoundEvent{pos.x, pos.y, pos.z, loudness, type, 0.0f});
  if (events_.size() > 256) events_.erase(events_.begin());  // hard cap, oldest out
}

void SoundBus::tick(float dt) {
  for (SoundEvent& ev : events_) ev.age += dt;
  events_.erase(std::remove_if(events_.begin(), events_.end(),
                               [](const SoundEvent& ev) { return ev.age >= kSoundLife; }),
                events_.end());
}

bool SoundBus::loudestAt(const Vec3& pos, float hearMult, HeardSound& out) const {
  const SoundEvent* best = nullptr;
  float bestStrength = 0.0f;
  for (const SoundEvent& ev : events_) {
    const float dx = ev.x - pos.x, dy = ev.y - pos.y, dz = ev.z - pos.z;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float range = ev.loudness * hearMult;
    if (d >= range) continue;
    const float s = (1.0f - d / range) * (1.0f - ev.age / kSoundLife);
    if (s > bestStrength) {
      bestStrength = s;
      best = &ev;
    }
  }
  if (!best) return false;
  out = HeardSound{best->x, best->y, best->z, best->type, best->age, bestStrength};
  return true;
}

// ---------------------------------------------------------------------------

std::uint64_t ScentField::key(int x, int y, int z) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x) & 0xFFFFFFFu) << 36) |
         (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & 0xFFFFFFFu) << 8) |
         static_cast<std::uint64_t>(static_cast<std::uint32_t>(y) & 0xFFu);
}

void ScentField::deposit(const Vec3& pos, const char* emitter) {
  const int x = static_cast<int>(std::floor(pos.x));
  const int y = static_cast<int>(std::floor(pos.y + 0.01f));
  const int z = static_cast<int>(std::floor(pos.z));
  cells_[key(x, y, z)] = Cell{now_, emitter};
  if (cells_.size() > 4096) {  // bounded: drop the stalest half
    const double cut = now_ - life_ * 0.5;
    for (auto it = cells_.begin(); it != cells_.end();) {
      it = it->second.time < cut ? cells_.erase(it) : std::next(it);
    }
  }
}

void ScentField::tick(float dt) {
  now_ += dt;
  // Amortised cleanup: a full sweep is cheap at this size, so do one every ~4 s.
  sweepT_ += dt;
  if (sweepT_ > 4.0f) {
    sweepT_ = 0.0f;
    const double cut = now_ - life_;
    for (auto it = cells_.begin(); it != cells_.end();) {
      it = it->second.time < cut ? cells_.erase(it) : std::next(it);
    }
  }
}

float ScentField::sample(int x, int y, int z, const char* emitter) const {
  auto it = cells_.find(key(x, y, z));
  if (it == cells_.end()) return 0.0f;
  if (emitter && it->second.emitter != emitter) return 0.0f;
  return std::max(0.0f, 1.0f - static_cast<float>((now_ - it->second.time) / life_));
}

bool ScentField::freshestNear(const Vec3& pos, int r, const char* emitter, Vec3& out,
                              float& strength) const {
  const int cx = static_cast<int>(std::floor(pos.x));
  const int cy = static_cast<int>(std::floor(pos.y + 0.01f));
  const int cz = static_cast<int>(std::floor(pos.z));
  double bestTime = -1e30;
  bool found = false;
  for (int dx = -r; dx <= r; ++dx) {
    for (int dz = -r; dz <= r; ++dz) {
      for (int dy = -1; dy <= 1; ++dy) {
        auto it = cells_.find(key(cx + dx, cy + dy, cz + dz));
        if (it == cells_.end()) continue;
        if (emitter && it->second.emitter != emitter) continue;
        if (it->second.time > bestTime) {
          bestTime = it->second.time;
          out = Vec3{static_cast<float>(cx + dx), static_cast<float>(cy + dy),
                     static_cast<float>(cz + dz)};
          found = true;
        }
      }
    }
  }
  if (!found) return false;
  strength = std::max(0.0f, 1.0f - static_cast<float>((now_ - bestTime) / life_));
  return true;
}

bool ScentField::nextTrailCell(const Vec3& pos, const char* emitter, Vec3& out) const {
  const int cx = static_cast<int>(std::floor(pos.x));
  const int cy = static_cast<int>(std::floor(pos.y + 0.01f));
  const int cz = static_cast<int>(std::floor(pos.z));

  double bestTime = -1e30;
  auto here = cells_.find(key(cx, cy, cz));
  if (here != cells_.end() && (!emitter || here->second.emitter == emitter)) {
    bestTime = here->second.time;
  }
  bool found = false;
  for (int dx = -1; dx <= 1; ++dx) {
    for (int dz = -1; dz <= 1; ++dz) {
      if (dx == 0 && dz == 0) continue;
      for (int dy = -1; dy <= 1; ++dy) {
        auto it = cells_.find(key(cx + dx, cy + dy, cz + dz));
        if (it == cells_.end()) continue;
        if (emitter && it->second.emitter != emitter) continue;
        if (it->second.time > bestTime) {
          bestTime = it->second.time;
          out = Vec3{static_cast<float>(cx + dx), static_cast<float>(cy + dy),
                     static_cast<float>(cz + dz)};
          found = true;
        }
      }
    }
  }
  return found;
}

// ---------------------------------------------------------------------------

void Senses::perceive(const Entity& e, float dt, const EntityContext& ctx, Percept& p) const {
  if (p.nextEval < 0.0f) {
    p.nextEval = static_cast<float>(randomUnit()) / cfg_.hz;  // stagger mobs
  }
  if (p.haveLastKnown) {
    p.lastKnownAge += dt;
    if (p.lastKnownAge > cfg_.memorySec) p.haveLastKnown = false;
  }
  p.nextEval -= dt;
  if (p.nextEval > 0.0f) return;
  p.nextEval += 1.0f / cfg_.hz;

  const world::World* world = ctx.world;
  const Player* player = ctx.player;

  // ---- sight ----
  p.visible = false;
  if (world && player && player->health() > 0.0f) {
    const Vec3 eye{e.pos.x, e.pos.y + cfg_.eyeHeight, e.pos.z};
    const Vec3 target{player->pos().x, player->pos().y + 1.5f, player->pos().z};
    const float dx = target.x - eye.x, dy = target.y - eye.y, dz = target.z - eye.z;
    p.dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (p.dist < cfg_.viewRange) {
      bool inCone = true;
      if (cfg_.fovDeg < 360.0f) {
        // Facing, from the model convention: yaw = pi/2 - heading.
        const float h = kPi / 2.0f - e.yaw;
        const float flat = std::max(1e-6f, std::sqrt(dx * dx + dz * dz));
        const float cosTo = (std::cos(h) * dx + std::sin(h) * dz) / flat;
        inCone = cosTo > std::cos(cfg_.fovDeg * kPi / 360.0f);
      }
      if (inCone && lineOfSight(*world, eye, target, cfg_.viewRange)) p.visible = true;
    }
    if (p.visible) {
      p.haveLastKnown = true;
      p.lastKnown = player->pos();
      p.lastKnownAge = 0.0f;
    }
  }

  // ---- hearing ----
  p.heard = false;
  if (ctx.entities) {
    p.heard = sounds(*ctx.entities).loudestAt(e.pos, cfg_.hearMult, p.sound);
    if (p.heard && !p.visible && (!p.haveLastKnown || p.lastKnownAge > 1.0f)) {
      // A heard sound refreshes a vaguer memory: where the noise came from.
      p.haveLastKnown = true;
      p.lastKnown = Vec3{p.sound.x, p.sound.y, p.sound.z};
      p.lastKnownAge = 0.5f;
    }
  }

  // ---- smell ----
  p.haveScent = false;
  if (cfg_.smell && ctx.entities) {
    float strength = 0.0f;
    p.haveScent = scent(*ctx.entities).freshestNear(e.pos, 2, nullptr, p.scent, strength);
  }
}

}  // namespace hr::game
