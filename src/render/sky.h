// Day/night cycle, ported from js/render/sky.js.
//
// Drives the sky gradient, a daylight factor that scales baked skylight, the fog
// colour (which is the horizon), and the primary directional light the deferred
// pass shades with.

#pragma once

#include <string>

#include "core/mat4.h"

namespace hr::render {

// The directional light for deferred shading: the sun while it is up, the moon
// while it is down. `dir` points *toward* the light, for N dot L.
struct Celestial {
  Vec3 dir;
  Vec3 color;
  float strength = 0.0f;
};

class Sky {
 public:
  // 0..1, where 0 is midnight and 0.5 is noon. The web build started here.
  float time = 0.32f;
  float dayLength = 600.0f;  // real seconds for a full cycle
  bool paused = false;

  void update(float dt);

  // Fast-forwards toward `target` (dawn by default) over `durationSeconds` of real
  // time. Beds call this; the sky handles the sweep.
  void startSleep(float target = 0.27f, float durationSeconds = 1.6f);
  bool isSleeping() const { return sleeping_; }
  bool isNight() const { return dayFactor() < 0.25f; }

  // In-game days the clock moved this frame, including any sleep fast-forward.
  float advanced() const { return advanced_; }

  float sunHeight() const;  // -1 at midnight, +1 at noon
  Vec3 sunDir() const;      // rises east, arcs overhead with a slight tilt, sets west
  float dayFactor() const;  // 0 at night .. 1 in full day
  // What the shader multiplies baked skylight by. Floored above zero so night is
  // dim rather than pitch black.
  float daylight() const;
  float duskFactor() const;  // warm tint strength near sunrise and sunset
  // Extra fog in the early morning: a narrow bump peaking just after dawn.
  float morningFog() const;

  Celestial celestial() const;
  Vec3 ambientColor() const;
  Vec3 horizon() const;
  Vec3 zenith() const;
  Vec3 fogColor() const { return horizon(); }

  std::string clockString() const;

 private:
  bool sleeping_ = false;
  float sleepRemaining_ = 0.0f;
  float sleepRate_ = 0.0f;
  float advanced_ = 0.0f;
};

}  // namespace hr::render
