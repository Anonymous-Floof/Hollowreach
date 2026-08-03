#include "render/sky.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hr::render {
namespace {

constexpr float kPi = 3.14159265358979323846f;

Vec3 lerp3(const Vec3& a, const Vec3& b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

const Vec3 kDayHorizon {0.62f, 0.76f, 0.95f};
const Vec3 kDayZenith {0.27f, 0.52f, 0.86f};
const Vec3 kNightHorizon {0.06f, 0.08f, 0.14f};
const Vec3 kNightZenith {0.01f, 0.02f, 0.05f};
const Vec3 kDusk {0.85f, 0.45f, 0.28f};

}  // namespace

void Sky::update(float dt) {
  advanced_ = 0.0f;
  if (paused) return;

  // Sleeping fast-forwards toward morning. This is the hook for a richer time
  // scale later — slowing the rate for a realistic sweep, or tying entity ticks to
  // it — so beds only have to call startSleep().
  if (sleeping_) {
    const float step = std::min(sleepRemaining_, sleepRate_ * dt);
    time = std::fmod(time + step, 1.0f);
    advanced_ = step;
    sleepRemaining_ -= step;
    if (sleepRemaining_ <= 1e-4f) {
      sleeping_ = false;
      // You wake rested. Hours slept deliberately do not count toward the next
      // sleep — otherwise a twelve-hour night would leave you tired again on
      // waking, and a bed would be usable twice in a row.
      hoursAwake_ = 0.0f;
    }
    return;
  }

  const float step = dt / dayLength;
  time = std::fmod(time + step, 1.0f);
  advanced_ = step;
  hoursAwake_ += step * kHoursPerDay;
}

void Sky::startSleep(float target, float durationSeconds) {
  float remaining = std::fmod(target - time + 1.0f, 1.0f);
  if (remaining < 1e-4f) remaining = 1.0f;  // asked for the hour it already is: go round
  // Scaled by how far there is to go, floored so a short nap is still a visible
  // sweep rather than a cut. A full day round comes out at the 1.6 s the fixed
  // duration used to be, which is what the fade was tuned against.
  if (durationSeconds <= 0.0f) durationSeconds = 0.55f + 1.05f * remaining;
  sleeping_ = true;
  sleepTarget_ = target;
  sleepRemaining_ = remaining;
  sleepRate_ = remaining / durationSeconds;
}

void Sky::setHoursAwake(float hours) {
  // Clamped rather than trusted: this arrives from a save file and from the
  // network, and a nonsense value here would either lock a bed forever or unlock
  // it permanently. A day's worth is as tired as it is meaningful to be.
  if (!(hours > 0.0f)) hours = 0.0f;  // also catches NaN
  hoursAwake_ = hours < kHoursPerDay ? hours : kHoursPerDay;
}

float Sky::sunHeightAt(float t) { return -std::cos(t * kPi * 2.0f); }
float Sky::dayFactorAt(float t) { return clamp01((sunHeightAt(t) + 0.2f) / 1.2f); }

std::string Sky::clockStringAt(float t) {
  t = t - std::floor(t);
  const int mins = static_cast<int>(t * 24.0f * 60.0f);
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", mins / 60, mins % 60);
  return buf;
}

std::string Sky::spanString(float days) {
  if (days < 0.0f) days = 0.0f;
  // Rounded to the minute, then carried, so 59.7 minutes reads "1h 0m" rather than
  // "0h 60m".
  int total = static_cast<int>(std::lround(days * 24.0f * 60.0f));
  const int hours = total / 60;
  const int mins = total % 60;
  char buf[24];
  if (hours > 0) {
    std::snprintf(buf, sizeof(buf), "%dh %dm", hours, mins);
  } else {
    std::snprintf(buf, sizeof(buf), "%dm", mins);
  }
  return buf;
}

float Sky::sunHeight() const { return sunHeightAt(time); }

Vec3 Sky::sunDir() const {
  const float a = time * kPi * 2.0f;
  Vec3 d {std::sin(a), -std::cos(a), 0.18f};
  const float l = d.length();
  return l > 0 ? d * (1.0f / l) : Vec3 {0, 1, 0};
}

float Sky::dayFactor() const { return dayFactorAt(time); }

float Sky::daylight() const { return 0.12f + 0.88f * dayFactor(); }

float Sky::duskFactor() const {
  const float h = sunHeight();
  return clamp01(1.0f - std::abs(h) * 4.0f) * clamp01(dayFactor() * 3.0f);
}

float Sky::morningFog() const {
  // A narrow bump peaking just after dawn (~0.26 of the cycle), zero the rest of
  // the day.
  const float d = std::abs(std::fmod(time - 0.26f + 1.5f, 1.0f) - 0.5f);
  return clamp01(1.0f - d / 0.08f);
}

Celestial Sky::celestial() const {
  const float day = dayFactor();
  if (sunHeight() > -0.05f) {  // sun up, plus a little twilight
    const float dusk = duskFactor();
    Celestial c;
    c.dir = sunDir();
    c.color = lerp3({1.0f, 0.97f, 0.90f}, {1.0f, 0.58f, 0.32f}, clamp01(dusk * 0.85f));
    c.strength = 1.15f * day;
    return c;
  }
  const Vec3 s = sunDir();
  Celestial c;
  c.dir = {-s.x, -s.y, -s.z};
  c.color = {0.52f, 0.62f, 0.85f};
  c.strength = 0.16f;
  return c;
}

Vec3 Sky::ambientColor() const {
  // Kept moderate, so the directional sun still reads as contrast rather than
  // washing the scene out to a flat bright.
  return lerp3({0.05f, 0.06f, 0.10f}, {0.40f, 0.46f, 0.56f}, dayFactor());
}

Vec3 Sky::horizon() const {
  const Vec3 c = lerp3(kNightHorizon, kDayHorizon, dayFactor());
  return lerp3(c, kDusk, duskFactor() * 0.7f);
}

Vec3 Sky::zenith() const { return lerp3(kNightZenith, kDayZenith, dayFactor()); }

std::string Sky::clockString() const { return clockStringAt(time); }

}  // namespace hr::render
