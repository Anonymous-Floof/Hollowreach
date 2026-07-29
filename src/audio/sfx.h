// The sound library, ported from js/audio/sfx.js.
//
// Every game event is synthesised from the two primitives in audio/engine.h. The
// recipes are transcribed value for value, because they are the sound design — a
// stone knock, a wood tok and a glass shatter are recognisably *those sounds* only
// because of where their bandpass centres sit and how their debris is staggered.
//
// The originals' notes on why each recipe sounds right, kept because they are the
// only documentation of the intent:
//   impacts   = filtered noise transient + a low "body" thump; the bandpass centre
//               is the material's voice (stone ~900 Hz, wood ~320 Hz).
//   wood      = two narrow resonances, like a struck board's modes.
//   glass     = a cluster of inharmonic high sine partials with random decays.
//   creatures = a sawtooth larynx driven through 1-2 sweeping bandpass "formants"
//               (a throat), plus breath noise. Tremolo is a bleat.
//   water     = noise swept down + little rising sine "droplets".
//
// Every call no-ops until the engine is running, and every call passes through the
// 32-event cap, so a burst of events cannot stack into a wall of sound.

#pragma once

#include <cstdint>
#include <string>

#include "core/mat4.h"
#include "world/blocks.h"

namespace hr::audio::sfx {

// The acoustic family a block belongs to. Derived from the block definition, so a
// new block sounds right without being listed anywhere — the tool hints at the
// material, with the handful of overrides the JS carried.
enum class Material : std::uint8_t {
  Stone,
  Ore,
  Wood,
  Dirt,
  Sand,
  Gravel,
  Grass,
  Leaves,
  Cloth,
  Glass,
};

Material materialOf(const world::BlockDef& block);

// Which of a creature's three voices to use.
enum class MobCall : std::uint8_t { Say, Hurt, Death };

// ---- blocks ----
void blockHit(const world::BlockDef& block, const Vec3& pos);
void blockBreak(const world::BlockDef& block, const Vec3& pos);
void blockPlace(const world::BlockDef& block, const Vec3& pos);
void step(const world::BlockDef& block, bool sprint);
void wadeStep();

// ---- doors / containers / stations ----
void doorToggle(const world::BlockDef& block, bool open, const Vec3& pos);
void chestOpen(const Vec3& pos);
void chestClose(const Vec3& pos);
void craft();
void smeltDone(const Vec3& pos);

// ---- player ----
void hurt();
void died();
void land(float intensity);  // 0..1 with fall height
void eat();
void splash(bool big);
void bubbles();
void pickup();
void toss();
void swing();
void thwack(const Vec3& pos);
void shutter();
void crit();
void warp();

// ---- mob voices ----
void sheep(MobCall kind, const Vec3& pos, int seed);
void pig(MobCall kind, const Vec3& pos, int seed);
void cow(MobCall kind, const Vec3& pos, int seed);
void zombie(MobCall kind, const Vec3& pos, int seed);
void sizzle(const Vec3& pos);

// ---- network relay ----
// One positional one-shot, named by string. The multiplayer host tells a guest
// "play `cow_hurt` over there" rather than replicating the state that would have
// produced it locally, because the alternative is streaming every mob's damage
// events and deriving the sound twice. Unknown names are ignored — a peer chooses
// this string, so it is a lookup and never a code path.
void playNamed(const std::string& kind, const Vec3& pos);

// ---- UI ----
void uiClick();
void uiSlot();

}  // namespace hr::audio::sfx
