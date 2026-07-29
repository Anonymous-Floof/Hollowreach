// Per-frame audio orchestration, ported from js/audio/director.js.
//
// Owns the things that need continuous game state rather than a discrete event: the
// 3D listener pose, the underwater muffle, footsteps (a stride-distance accumulator
// over the block underfoot), entering-water splashes, ambient mob calls, and the
// ambience beds. Discrete events — mining, doors, hits — call the sfx facade
// directly from where they happen.

#pragma once

namespace hr::game {
class Player;
class EntityManager;
}
namespace hr::world {
class World;
}

namespace hr::audio {

struct DirectorContext {
  const world::World* world = nullptr;
  const game::Player* player = nullptr;
  float dayFactor = 1.0f;
  float underwater = 0.0f;
  bool active = true;  // state == playing
  game::EntityManager* entities = nullptr;
};

class Director {
 public:
  void update(float dt, const DirectorContext& ctx);

  // Quitting to the menu: settle every bed, forget transient state.
  void stop();

 private:
  float stride_ = 0.0f;
  bool wasInWater_ = false;
  float mobCallCooldown_ = 0.0f;
};

Director& director();

}  // namespace hr::audio
