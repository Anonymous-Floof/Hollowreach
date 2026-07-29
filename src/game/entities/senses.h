// Mob perception, ported from js/game/entities/ai/senses.js.
//
// Three senses, each answering "what do I know about the world right now?":
//
//   * SIGHT   — range, a field-of-view cone, and true voxel line-of-sight, so mobs
//               cannot see through hills and sneaking up from behind works.
//   * HEARING — the world SoundBus: recent sound events within earshot.
//   * SMELL   — the ScentField the player leaves behind, so a tracker can follow
//               where the player has *been*, not where they are.
//
// Only line-of-sight is wired into a mob today — the zombie's. The rest is the AI
// backend the plan calls for: built, tested, and dormant until a mob needs it.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/mat4.h"

namespace hr::world {
class World;
}

namespace hr::game {

struct Entity;
struct EntityContext;

// True when no solid block sits between a and b. Amanatides & Woo DDA, the same
// walk as game/raycast.cpp but stopping at solids only — liquids and foliage stay
// see-through — and answering yes/no rather than reporting a hit.
bool lineOfSight(const world::World& world, const Vec3& a, const Vec3& b, float maxDist = 64.0f);

// ---------------------------------------------------------------------------
// Sound events
// ---------------------------------------------------------------------------

struct SoundEvent {
  float x = 0, y = 0, z = 0;
  float loudness = 0;  // audible radius in blocks
  std::string type;
  float age = 0;
};

struct HeardSound {
  float x = 0, y = 0, z = 0;
  std::string type;
  float age = 0;
  float strength = 0;  // 0..1
};

class SoundBus {
 public:
  // Anything may announce a sound: emit(pos, 12, "break").
  void emit(const Vec3& pos, float loudness, const char* type = "generic");
  void tick(float dt);
  // Loudest audible event at `pos`; `hearMult` scales the listener's sensitivity.
  bool loudestAt(const Vec3& pos, float hearMult, HeardSound& out) const;
  void clear() { events_.clear(); }
  std::size_t size() const { return events_.size(); }

 private:
  std::vector<SoundEvent> events_;
};

// ---------------------------------------------------------------------------
// Scent trail
// ---------------------------------------------------------------------------

// A sparse, decaying grid of "someone was here" markers, dropped at the player's
// feet a few times a second. Each carries the time it was laid, so FRESHER means
// CLOSER TO THE PLAYER: a tracking mob standing on the trail follows the
// neighbouring cell with the freshest stamp and walks the player's actual route —
// around the lake, up the real slope — with no pathfinding at all.
class ScentField {
 public:
  void deposit(const Vec3& pos, const char* emitter = "player");
  void tick(float dt);

  double now() const { return now_; }

  // Strength 0..1 at a cell; 0 means none or expired.
  float sample(int x, int y, int z, const char* emitter = nullptr) const;
  // Freshest cell within Chebyshev radius r. Small r — this is a scan.
  bool freshestNear(const Vec3& pos, int r, const char* emitter, Vec3& out, float& strength) const;
  // From the mob's own cell, the neighbour whose scent is fresher than where it
  // stands: the direction the player walked. False when the trail is cold.
  bool nextTrailCell(const Vec3& pos, const char* emitter, Vec3& out) const;

  void clear() { cells_.clear(); }
  std::size_t size() const { return cells_.size(); }

 private:
  struct Cell {
    double time = 0;
    std::string emitter;
  };
  static std::uint64_t key(int x, int y, int z);

  std::unordered_map<std::uint64_t, Cell> cells_;
  double now_ = 0.0;
  double life_ = 45.0;  // seconds a scent cell lasts
  float sweepT_ = 0.0f;
};

// ---------------------------------------------------------------------------
// The three combined
// ---------------------------------------------------------------------------

struct SenseConfig {
  float viewRange = 20.0f;
  float fovDeg = 140.0f;  // vision cone centred on facing; 360 = eyes everywhere
  float eyeHeight = 1.6f;
  float hearMult = 1.0f;
  bool smell = false;
  float memorySec = 6.0f;
  float hz = 5.0f;  // perceive() re-evaluates this many times a second
};

struct Percept {
  bool visible = false;
  float dist = 1e30f;
  bool heard = false;
  HeardSound sound;
  bool haveScent = false;
  Vec3 scent;
  bool haveLastKnown = false;
  Vec3 lastKnown;
  float lastKnownAge = 0;
  float nextEval = -1;  // negative until the first call seeds a random phase
};

// Sight lines are the expensive part, so this throttles itself and staggers mobs
// by a random per-entity phase.
class Senses {
 public:
  explicit Senses(const SenseConfig& config = {}) : cfg_(config) {}
  void perceive(const Entity& e, float dt, const EntityContext& ctx, Percept& p) const;

 private:
  SenseConfig cfg_;
};

}  // namespace hr::game
