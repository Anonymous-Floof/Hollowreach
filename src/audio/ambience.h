// The ambient soundscape, ported from js/audio/ambience.js.
//
// Two continuous beds — wind and a cave drone — crossfade with where you are and
// what time it is, and a handful of scheduled one-shots sit on top: bird chirps by
// day, cricket chirps at night, echoing drips underground, fire crackle near
// torches and burning forges, bubbles underwater. All synthesised.
//
// The beds live inside the engine because they are persistent node chains rather
// than voices; this class only eases their targets. The one-shots are fired from
// randomised countdowns so nothing loops audibly.

#pragma once

#include "core/mat4.h"

namespace hr::game {
class Player;
}
namespace hr::world {
class World;
}

namespace hr::audio {

struct AmbienceContext {
  const world::World* world = nullptr;
  const game::Player* player = nullptr;
  float dayFactor = 1.0f;
  float underwater = 0.0f;
  bool active = true;  // playing, not paused
};

class Ambience {
 public:
  Ambience();

  void update(float dt, const AmbienceContext& ctx);

  // Quitting to the menu: settle every bed, forget transient state.
  void quiet();

 private:
  struct Fire {
    float x, y, z, strength;
  };

  void scanFires(const world::World& world, const Vec3& player);
  void pop(const Fire& fire);
  void cricketChirp(const Vec3& player);
  void birdChirp(const Vec3& player);
  void drip(const Vec3& player);

  float birdT_ = 0.0f;
  float dripT_ = 0.0f;
  float popT_ = 0.2f;
  float bubbleT_ = 1.0f;
  float scanT_ = 0.0f;
  float cricketT_ = 0.0f;
  float gustT_ = 0.0f;
  float caveSwellT_ = 0.0f;

  Fire fires_[8];
  int fireCount_ = 0;
};

Ambience& ambience();

}  // namespace hr::audio
