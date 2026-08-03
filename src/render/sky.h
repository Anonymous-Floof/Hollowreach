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

  // Fast-forwards to `target` on the 0..1 clock. The sweep's real duration is
  // derived from how far it has to go, so a nap does not take as long to watch as
  // a full night; pass `durationSeconds` to override it.
  void startSleep(float target = kDawn, float durationSeconds = -1.0f);
  bool isSleeping() const { return sleeping_; }
  bool isNight() const { return dayFactor() < 0.25f; }
  // Where the current sweep is headed, for anything that has to describe it.
  float sleepTarget() const { return sleepTarget_; }

  // --- tiredness -------------------------------------------------------------
  //
  // Game hours since the last sleep ended. A bed refuses until this reaches
  // kRestedHours, so sleeping is something you do once a day rather than a button
  // that deletes any night you did not fancy. It lives on the clock rather than on
  // the player because it is measured in clock time and nothing else, and because
  // the clock is the one thing every mode of play — single player, host and guest —
  // already agrees about.
  static constexpr float kRestedHours = 8.0f;
  static constexpr float kHoursPerDay = 24.0f;

  float hoursAwake() const { return hoursAwake_; }
  bool tired() const { return hoursAwake_ >= kRestedHours; }
  float hoursUntilTired() const {
    const float left = kRestedHours - hoursAwake_;
    return left > 0.0f ? left : 0.0f;
  }
  // Save/load, and the guest side of a sleep the host carried out.
  void setHoursAwake(float hours);
  void markRested() { hoursAwake_ = 0.0f; }

  // Dawn on the 0..1 clock, which is where a bed points by default.
  static constexpr float kDawn = 0.27f;

  // In-game days the clock moved this frame, including any sleep fast-forward.
  float advanced() const { return advanced_; }

  float sunHeight() const;  // -1 at midnight, +1 at noon
  Vec3 sunDir() const;      // rises east, arcs overhead with a slight tilt, sets west
  float dayFactor() const;  // 0 at night .. 1 in full day

  // The same two, for a clock reading that is not the current one. The Time Wheel
  // paints a whole day's worth of them around its rim, so they cannot be members
  // that read `time`.
  static float sunHeightAt(float t);
  static float dayFactorAt(float t);
  static std::string clockStringAt(float t);
  // "9h 20m" for a span of the 0..1 clock. Shared so the wheel and the notices
  // phrase a night the same way.
  static std::string spanString(float days);
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
  float sleepTarget_ = kDawn;
  float advanced_ = 0.0f;
  float hoursAwake_ = 0.0f;
};

}  // namespace hr::render
