#include "audio/ambience.h"

#include <algorithm>
#include <cmath>

#include "audio/engine.h"
#include "audio/sfx.h"
#include "core/prng.h"
#include "game/blockentities.h"
#include "game/player.h"
#include "world/world.h"

namespace hr::audio {
namespace {

constexpr float kPi = 3.14159265358979f;

inline float R(float a, float b) { return a + static_cast<float>(randomUnit()) * (b - a); }
inline int randInt(int n) { return static_cast<int>(randomUnit() * n); }

}  // namespace

Ambience::Ambience() {
  birdT_ = R(2.0f, 6.0f);
  dripT_ = R(3.0f, 8.0f);
  cricketT_ = R(1.0f, 3.0f);
}

Ambience& ambience() {
  static Ambience instance;
  return instance;
}

void Ambience::update(float dt, const AmbienceContext& ctx) {
  if (!engine().ready() || !ctx.world || !ctx.player) return;

  const Vec3 head = ctx.player->eye();
  const Vec3 pos = ctx.player->pos();
  const float exposure = ctx.world->getSky(static_cast<int>(std::floor(head.x)),
                                           static_cast<int>(std::floor(head.y)),
                                           static_cast<int>(std::floor(head.z))) /
                         15.0f;
  const float day = ctx.dayFactor;
  const float uw = ctx.underwater;
  const float act = ctx.active ? 1.0f : 0.4f;  // paused: beds fade low, not out
  const bool inCave = exposure < 0.2f && head.y < 55.0f;

  // ---- bed targets ----
  const float windT = act * (1.0f - uw) *
                      (inCave ? 0.02f
                              : (0.05f + exposure * 0.10f +
                                 std::max(0.0f, std::min(0.15f, (head.y - 66.0f) / 90.0f)))) *
                      (0.7f + 0.3f * day);
  const float caveT = act * (1.0f - uw) * (inCave ? 0.11f : 0.0f);
  engine().setBedGain(Bed::Wind, windT, 0.8f);
  engine().setBedGain(Bed::Cave, caveT, 1.5f);

  // wind gusts: every few seconds pick a new filter centre plus a swell
  gustT_ -= dt;
  if (gustT_ <= 0.0f) {
    gustT_ = R(2.5f, 6.0f);
    engine().setWindFilter(R(240.0f, 720.0f), 1.8f);
    engine().setBedGain(Bed::Wind, windT * R(0.7f, 1.5f), 1.2f);
  }

  // cave drone swells
  caveSwellT_ -= dt;
  if (caveSwellT_ <= 0.0f && inCave) {
    caveSwellT_ = R(5.0f, 12.0f);
    engine().setBedGain(Bed::Cave, caveT * R(0.6f, 1.6f), 2.5f);
  }

  if (!ctx.active) return;  // no one-shots while paused

  // ---- birds (day, on the surface) ----
  birdT_ -= dt;
  if (birdT_ <= 0.0f) {
    birdT_ = R(5.0f, 16.0f);
    if (day > 0.5f && exposure > 0.55f && uw < 0.5f) birdChirp(pos);
  }

  // ---- crickets (night, on the surface): a sparse chirp chorus. Each fire is one
  // nearby cricket; the quick interval and random directions make it read as many
  // crickets around you, not one buzzing in your ear. ----
  cricketT_ -= dt;
  if (cricketT_ <= 0.0f) {
    const bool night = !inCave && exposure > 0.4f && day < 0.2f && uw < 0.5f;
    cricketT_ = night ? R(0.26f, 0.85f) : R(1.5f, 3.0f);
    if (night) cricketChirp(pos);
  }

  // ---- drips (underground) ----
  dripT_ -= dt;
  if (dripT_ <= 0.0f) {
    dripT_ = R(3.0f, 9.0f);
    if (inCave) drip(pos);
  }

  // ---- fire crackle: scan for nearby torches and burning forges ----
  scanT_ -= dt;
  if (scanT_ <= 0.0f) {
    scanT_ = 0.8f;
    scanFires(*ctx.world, pos);
  }
  popT_ -= dt;
  if (popT_ <= 0.0f && fireCount_ > 0) {
    popT_ = R(0.1f, 0.35f) / std::min(3, fireCount_);
    pop(fires_[randInt(fireCount_)]);
  }

  // ---- bubbles while submerged ----
  bubbleT_ -= dt;
  if (bubbleT_ <= 0.0f) {
    bubbleT_ = R(0.7f, 1.8f);
    if (uw > 0.5f) sfx::bubbles();
  }
}

void Ambience::scanFires(const world::World& world, const Vec3& p) {
  fireCount_ = 0;
  const int px = static_cast<int>(std::floor(p.x));
  const int py = static_cast<int>(std::floor(p.y + 1.0f));
  const int pz = static_cast<int>(std::floor(p.z));
  const world::BlockId ember = world::wk().emberlight;
  constexpr int kRadius = 7;

  for (int y = py - 4; y <= py + 4 && fireCount_ < 6; ++y) {
    for (int z = pz - kRadius; z <= pz + kRadius && fireCount_ < 6; ++z) {
      for (int x = px - kRadius; x <= px + kRadius && fireCount_ < 6; ++x) {
        if (world.getBlock(x, y, z) == ember) {
          fires_[fireCount_++] = {x + 0.5f, y + 0.5f, z + 0.5f, 0.6f};
        }
      }
    }
  }

  // Burning forges crackle harder than torches.
  for (const auto& [key, be] : world.blockEntities()) {
    if (fireCount_ >= static_cast<int>(sizeof(fires_) / sizeof(fires_[0]))) break;
    if (be.kind != game::BlockEntityKind::Forge || !(be.fuelLeft > 0.0f)) continue;
    int x = 0, y = 0, z = 0;
    game::unpackBlockEntityKey(key, x, y, z);
    if (std::abs(x - px) <= 10 && std::abs(y - py) <= 5 && std::abs(z - pz) <= 10) {
      fires_[fireCount_++] = {x + 0.5f, y + 0.5f, z + 0.5f, 1.4f};
    }
  }
}

void Ambience::pop(const Fire& fire) {
  if (!engine().tryVoice(0.3f)) return;
  const Dest d = engine().out(Bus::Amb, Vec3{fire.x, fire.y, fire.z});
  engine().burst(d, {.dur = R(0.02f, 0.06f), .gain = 0.5f * fire.strength,
                     .noise = NoiseKind::Crackle, .filters = hp(R(900.0f, 1600.0f), 0.7f)});
  if (randomUnit() < 0.25) {
    engine().burst(d, {.dur = R(0.15f, 0.3f), .gain = 0.12f * fire.strength, .attack = 0.05f,
                       .curve = Curve::Lin, .filters = bp(R(160.0f, 260.0f), 1.0f)});
  }
}

void Ambience::cricketChirp(const Vec3& p) {
  // One cricket's chirp: a short run of clean tonal pulses (~30/s pulse rate) at
  // this cricket's own pitch, then silence. A stridulation is a burst of pulses, so
  // a bandpassed sine blip per pulse — NOT a sustained tone.
  if (!engine().tryVoice(0.6f)) return;
  const float ang = static_cast<float>(randomUnit()) * kPi * 2.0f;
  const float dist = R(3.0f, 14.0f);
  const Vec3 pos{p.x + std::cos(ang) * dist, p.y + R(-1.5f, 2.0f), p.z + std::sin(ang) * dist};
  const Dest d = engine().out(Bus::Amb, pos);

  const float f = R(4200.0f, 5000.0f) * (randomUnit() < 0.5 ? 1.0f : 0.985f);  // this cricket's note
  const int pulses = 2 + randInt(4);                                           // 2-5 per chirp
  const float gap = R(0.028f, 0.04f);                                          // ~25-36 Hz
  const float gain = R(0.05f, 0.085f);
  for (int i = 0; i < pulses; ++i) {
    engine().tone(d, {.delay = i * gap, .freq = f, .dur = 0.014f, .gain = gain, .attack = 0.002f,
                      .filter = bp(f, 12.0f)});
  }
}

void Ambience::birdChirp(const Vec3& p) {
  if (!engine().tryVoice(1.2f)) return;
  const float ang = static_cast<float>(randomUnit()) * kPi * 2.0f;
  const float dist = R(9.0f, 20.0f);
  const Dest d = engine().out(
      Bus::Amb, Vec3{p.x + std::cos(ang) * dist, p.y + R(4.0f, 9.0f), p.z + std::sin(ang) * dist});

  const int notes = 2 + randInt(4);
  const float base = R(2400.0f, 4100.0f);
  float at = 0.0f;
  for (int i = 0; i < notes; ++i) {
    const float f = base * R(0.85f, 1.2f);
    const bool up = randomUnit() < 0.6;
    engine().tone(d, {.delay = at, .freq = f,
                      .sweepTo = f * (up ? R(1.15f, 1.5f) : R(0.65f, 0.85f)),
                      .dur = R(0.05f, 0.1f), .gain = R(0.1f, 0.16f), .attack = 0.008f});
    if (randomUnit() < 0.35) {
      engine().tone(d, {.delay = at + 0.02f, .freq = f * 1.01f, .sweepTo = f * 1.3f, .dur = 0.05f,
                        .gain = 0.06f});
    }
    at += R(0.07f, 0.17f);
  }
}

void Ambience::drip(const Vec3& p) {
  if (!engine().tryVoice(1.5f)) return;
  const float ang = static_cast<float>(randomUnit()) * kPi * 2.0f;
  const float dist = R(4.0f, 12.0f);
  const Dest d = engine().out(
      Bus::Amb, Vec3{p.x + std::cos(ang) * dist, p.y + R(1.0f, 4.0f), p.z + std::sin(ang) * dist});

  const float f = R(900.0f, 1400.0f);
  // The drip, then two fading echoes off the cave walls.
  for (int i = 0; i < 3; ++i) {
    engine().tone(d, {.delay = i * R(0.21f, 0.26f), .freq = f, .sweepTo = f * 0.5f, .dur = 0.06f,
                      .gain = 0.16f / (i + 1), .attack = 0.003f});
  }
  if (randomUnit() < 0.4) {  // plop resonance
    engine().tone(d, {.delay = 0.05f, .freq = f * 0.5f, .sweepTo = f * 0.9f, .dur = 0.1f,
                      .gain = 0.05f});
  }
}

void Ambience::quiet() {
  engine().quietBeds();
  fireCount_ = 0;
}

}  // namespace hr::audio
