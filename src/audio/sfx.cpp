#include "audio/sfx.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

#include "audio/engine.h"
#include "audio/soundbank.h"
#include "core/prng.h"

namespace hr::audio::sfx {
namespace {

// `const R = (a, b) => a + Math.random() * (b - a)`.
inline float R(float a, float b) { return a + static_cast<float>(randomUnit()) * (b - a); }

// `go(dur, bus, pos)`: the voice-cap gate every entry point starts with.
Dest go(float dur, Bus bus) {
  if (!engine().ready() || !engine().tryVoice(dur)) return Dest{};
  return engine().out(bus);
}

Dest go(float dur, Bus bus, const Vec3& pos) {
  if (!engine().ready() || !engine().tryVoice(dur)) return Dest{};
  return engine().out(bus, pos);
}

// ---------------------------------------------------------------------------
// Resource packs
//
// Every entry point below now asks the pack stack first, by name, and falls
// through to its synthesised recipe when no pack supplies that name. That
// fall-through is the whole design: a pack replacing four sounds replaces four
// sounds, and the other ninety are still the recipes in this file.
//
// `reference` is what a clip at "volume": 1.0 plays back at, chosen per call site
// to sit where the recipe it replaces sat. Without it a full-scale wav would be
// two to five times louder than everything around it, and a pack that changed only
// the footsteps would make walking the loudest thing in the game. A pack author
// who disagrees has sounds.json's own `volume` to say so with.
// ---------------------------------------------------------------------------

// Wide enough that repeated hits do not sound like a loop, narrow enough that the
// material still reads. Minecraft varies its own block sounds by about this much.
constexpr float kBlockJitter = 0.08f;
constexpr float kVoiceJitter = 0.06f;

bool playPack(std::string_view event, Bus bus, const Vec3* pos, float reference, float jitter,
              float roll) {
  if (!engine().ready()) return false;
  const SoundPick picked = sounds().pick(event, roll);
  if (!picked.valid()) return false;

  float pitch = picked.pitch;
  if (jitter > 0.0f) pitch *= 1.0f + R(-jitter, jitter);
  pitch = std::clamp(pitch, 0.125f, 4.0f);
  const float seconds = static_cast<float>(picked.clip->seconds() / pitch);

  const Dest d = pos ? go(seconds, bus, *pos) : go(seconds, bus);
  // Refused by the 32-event cap. Still "handled": falling through to the
  // synthesised recipe here would push a second event at a mixer that has just
  // said it has no room, which is the one thing the cap exists to prevent.
  if (!d.valid) return true;

  engine().sample(d, {.samples = picked.clip->mono.data(),
                      .frameCount = static_cast<int>(picked.clip->mono.size()),
                      .sampleRate = picked.clip->sampleRate,
                      .gain = reference * picked.volume,
                      .pitch = pitch});
  return true;
}

bool playPack(std::string_view event, Bus bus, const Vec3* pos, float reference,
              float jitter = 0.0f) {
  return playPack(event, bus, pos, reference, jitter, static_cast<float>(randomUnit()));
}

// A mob's variant is chosen from its seed rather than at random, so one animal
// keeps one voice across its calls the way the synthesised path already does by
// feeding the seed into its formants.
float seedRoll(int seed) {
  const unsigned int h = static_cast<unsigned int>(seed) * 2654435761u + 0x9E3779B9u;
  return static_cast<float>(h >> 8) / static_cast<float>(1u << 24);
}

std::string_view materialName(Material material) {
  switch (material) {
    case Material::Stone: return "stone";
    case Material::Ore: return "ore";
    case Material::Wood: return "wood";
    case Material::Dirt: return "dirt";
    case Material::Sand: return "sand";
    case Material::Gravel: return "gravel";
    case Material::Grass: return "grass";
    case Material::Leaves: return "leaves";
    case Material::Cloth: return "wool";  // Minecraft's spelling, so its packs fit
    case Material::Glass: return "glass";
  }
  return "stone";
}

std::string blockEvent(Material material, std::string_view action) {
  std::string out = "block.";
  out += materialName(material);
  out += '.';
  out += action;
  return out;
}

// "ambient" rather than "say": Minecraft's name for an idle call, and therefore
// the key already sitting in any pack made for it.
std::string_view callName(MobCall kind) {
  switch (kind) {
    case MobCall::Say: return "ambient";
    case MobCall::Hurt: return "hurt";
    case MobCall::Death: return "death";
  }
  return "ambient";
}

bool playMobPack(std::string_view species, MobCall kind, const Vec3& pos, int seed) {
  std::string event = "entity.";
  event += species;
  event += '.';
  event += callName(kind);
  return playPack(event, Bus::Sfx, &pos, 0.6f, kVoiceJitter, seedRoll(seed));
}

// ---------------------------------------------------------------------------
// Per-material impact designers. `s` scales loudness and length: dig ticks use
// ~0.5, steps ~0.3, breaks 1.0. `at` offsets the whole recipe in time, which the
// JS did with a setTimeout in the one place it needed it.
// ---------------------------------------------------------------------------

void impact(Material material, const Dest& d, float s, float at = 0.0f);

void impactStone(const Dest& d, float s, float at) {
  const float f = R(580.0f, 820.0f);
  engine().burst(d, {.delay = at, .dur = 0.055f * s + 0.03f, .gain = 0.5f * s, .filters = bp(f, 1.0f)});
  engine().burst(d, {.delay = at, .dur = 0.07f, .gain = 0.45f * s, .filters = lp(200.0f, 0.8f)});
}

void impactOre(const Dest& d, float s, float at) {
  impactStone(d, s, at);
  // the metallic glint
  engine().tone(d, {.delay = at, .freq = R(1900.0f, 2600.0f), .dur = 0.09f, .gain = 0.06f * s});
}

void impactWood(const Dest& d, float s, float at) {
  const float f0 = R(150.0f, 195.0f);
  engine().burst(d, {.delay = at, .dur = 0.05f * s + 0.025f, .gain = 0.55f * s, .filters = bp(f0, 5.0f)});
  engine().burst(d, {.delay = at, .dur = 0.04f * s + 0.02f, .gain = 0.4f * s, .filters = bp(f0 * 2.3f, 6.0f)});
  engine().burst(d, {.delay = at, .dur = 0.012f, .gain = 0.22f * s, .filters = hp(1800.0f, 0.7f)});
}

void impactDirt(const Dest& d, float s, float at) {
  engine().burst(d, {.delay = at, .dur = 0.08f * s + 0.04f, .gain = 0.5f * s, .attack = 0.006f,
                     .filters = lp(R(280.0f, 380.0f), 0.8f)});
  engine().burst(d, {.delay = at, .dur = 0.05f, .gain = 0.14f * s, .filters = bp(900.0f, 0.6f)});
}

void impactSand(const Dest& d, float s, float at) {
  for (int i = 0; i < 3; ++i) {
    engine().burst(d, {.delay = at + i * R(0.014f, 0.03f), .dur = R(0.03f, 0.06f), .gain = 0.2f * s,
                       .filters = bp(R(2300.0f, 3600.0f), 0.8f)});
  }
  engine().burst(d, {.delay = at, .dur = 0.09f * s, .gain = 0.2f * s, .filters = lp(500.0f, 0.6f)});
}

void impactGravel(const Dest& d, float s, float at) {
  for (int i = 0; i < 4; ++i) {
    engine().burst(d, {.delay = at + i * R(0.015f, 0.035f), .dur = R(0.025f, 0.05f),
                       .gain = 0.28f * s, .filters = bp(R(900.0f, 1600.0f), 1.4f)});
  }
}

void impactGrass(const Dest& d, float s, float at) {
  engine().burst(d, {.delay = at, .dur = 0.1f * s + 0.05f, .gain = 0.3f * s, .attack = 0.02f,
                     .curve = Curve::Lin, .filters = hp(1500.0f, 0.6f)});
}

void impactLeaves(const Dest& d, float s, float at) {
  engine().burst(d, {.delay = at, .dur = 0.12f * s + 0.06f, .gain = 0.32f * s, .attack = 0.025f,
                     .curve = Curve::Lin, .filters = hp(1200.0f, 0.6f)});
  engine().burst(d, {.delay = at + 0.04f, .dur = 0.08f, .gain = 0.16f * s, .filters = hp(2500.0f, 0.7f)});
}

void impactCloth(const Dest& d, float s, float at) {
  engine().burst(d, {.delay = at, .dur = 0.1f * s + 0.04f, .gain = 0.34f * s, .attack = 0.012f,
                     .filters = lp(420.0f, 0.6f)});
}

void impactGlass(const Dest& d, float s, float at) {
  engine().tone(d, {.delay = at, .freq = R(2200.0f, 2800.0f), .dur = 0.07f, .gain = 0.2f * s});
  engine().burst(d, {.delay = at, .dur = 0.03f, .gain = 0.14f * s, .filters = hp(3000.0f, 0.7f)});
}

void impact(Material material, const Dest& d, float s, float at) {
  switch (material) {
    case Material::Stone: impactStone(d, s, at); break;
    case Material::Ore: impactOre(d, s, at); break;
    case Material::Wood: impactWood(d, s, at); break;
    case Material::Dirt: impactDirt(d, s, at); break;
    case Material::Sand: impactSand(d, s, at); break;
    case Material::Gravel: impactGravel(d, s, at); break;
    case Material::Grass: impactGrass(d, s, at); break;
    case Material::Leaves: impactLeaves(d, s, at); break;
    case Material::Cloth: impactCloth(d, s, at); break;
    case Material::Glass: impactGlass(d, s, at); break;
  }
}

// ---------------------------------------------------------------------------
// Breaking: the impact plus material-specific debris.
// ---------------------------------------------------------------------------

void breakGlass(const Dest& d) {
  // The shatter: a cluster of bright inharmonic partials, each with its own random
  // decay, over a high splash of noise.
  for (int i = 0; i < 7; ++i) {
    engine().tone(d, {.delay = R(0.0f, 0.05f), .freq = R(1600.0f, 6800.0f),
                      .dur = R(0.06f, 0.3f), .gain = R(0.05f, 0.13f)});
  }
  engine().burst(d, {.dur = 0.22f, .gain = 0.3f, .filters = hp(2800.0f, 0.6f)});
  engine().burst(d, {.delay = 0.05f, .dur = 0.14f, .gain = 0.12f, .filters = hp(4000.0f, 0.6f)});
}

void breakWood(const Dest& d) {
  engine().burst(d, {.dur = 0.018f, .gain = 0.5f, .filters = hp(1300.0f, 0.7f)});  // the crack
  impactWood(d, 1.0f, 0.0f);
  engine().burst(d, {.delay = 0.06f, .dur = 0.12f, .gain = 0.2f, .filters = bp(350.0f, 2.0f)});
}

void breakStone(const Dest& d) {
  // Rock gives way: a sharp crack, a heavy low boom, then chunks tumbling.
  engine().burst(d, {.dur = 0.02f, .gain = 0.45f, .filters = hp(900.0f, 0.7f)});
  engine().burst(d, {.dur = 0.1f, .gain = 0.55f, .filters = bp(R(420.0f, 560.0f), 1.0f)});
  engine().burst(d, {.dur = 0.18f, .gain = 0.6f, .attack = 0.006f, .filters = lp(170.0f, 0.9f)});
  engine().tone(d, {.freq = 95.0f, .sweepTo = 52.0f, .dur = 0.16f, .gain = 0.32f});
  for (int i = 0; i < 5; ++i) {  // rubble settling, falling in pitch
    const float f = R(280.0f, 620.0f);
    engine().burst(d, {.delay = 0.06f + i * R(0.035f, 0.06f), .dur = R(0.04f, 0.09f),
                       .gain = R(0.14f, 0.24f), .filters = bp(f, 1.3f, f * 0.6f, 0.08f)});
  }
}

void breakOre(const Dest& d) {
  breakStone(d);
  engine().tone(d, {.delay = 0.02f, .freq = R(1900.0f, 2600.0f), .dur = 0.12f, .gain = 0.07f});
}

void breakGeneric(const Dest& d, Material material) {
  impact(material, d, 1.0f);
  // rubble/debris tail, staggered and falling in pitch
  const float base = material == Material::Dirt ? 350.0f : 600.0f;
  for (int i = 0; i < 3; ++i) {
    engine().burst(d, {.delay = 0.04f + i * R(0.03f, 0.05f), .dur = R(0.04f, 0.08f),
                       .gain = R(0.12f, 0.2f), .filters = bp(base * R(0.6f, 1.1f), 1.2f)});
  }
}

}  // namespace

// ---------------------------------------------------------------------------

Material materialOf(const world::BlockDef& block) {
  const std::string_view k = block.key;
  if (block.isPlant) return Material::Grass;
  if (block.isLeaf) return Material::Leaves;
  if (k == "glass") return Material::Glass;
  if (k == "wool" || block.render == world::RenderKind::Bed) return Material::Cloth;
  if (k == "sand") return Material::Sand;
  if (k == "shingle") return Material::Gravel;
  if (k.rfind("ore_", 0) == 0) return Material::Ore;
  if (k == "emberlight" || k == "ladder") return Material::Wood;
  if (block.tool == world::ToolType::Axe || block.isPlank || block.isLog) return Material::Wood;
  if (block.tool == world::ToolType::Shovel) return Material::Dirt;
  return Material::Stone;  // picks and everything else
}

// ---- blocks ---------------------------------------------------------------

void blockHit(const world::BlockDef& block, const Vec3& pos) {
  const Material material = materialOf(block);
  if (playPack(blockEvent(material, "hit"), Bus::Sfx, &pos, 0.4f, kBlockJitter)) return;
  const Dest d = go(0.15f, Bus::Sfx, pos);  // rhythmic dig tick while mining
  if (d.valid) impact(material, d, 0.45f);
}

void blockBreak(const world::BlockDef& block, const Vec3& pos) {
  const Material material = materialOf(block);
  if (playPack(blockEvent(material, "break"), Bus::Sfx, &pos, 0.75f, kBlockJitter)) return;
  const Dest d = go(0.5f, Bus::Sfx, pos);
  if (!d.valid) return;
  switch (material) {
    case Material::Glass: breakGlass(d); break;
    case Material::Wood: breakWood(d); break;
    case Material::Stone: breakStone(d); break;
    case Material::Ore: breakOre(d); break;
    default: breakGeneric(d, material); break;
  }
}

void blockPlace(const world::BlockDef& block, const Vec3& pos) {
  const Material material = materialOf(block);
  if (playPack(blockEvent(material, "place"), Bus::Sfx, &pos, 0.55f, kBlockJitter)) return;
  const Dest d = go(0.2f, Bus::Sfx, pos);
  if (d.valid) impact(material, d, 0.7f);
}

void step(const world::BlockDef& block, bool sprint) {
  const Material material = materialOf(block);
  // Steps live at the feet: no panner, just quiet short impacts.
  if (playPack(blockEvent(material, "step"), Bus::Sfx, nullptr, sprint ? 0.38f : 0.3f,
               kBlockJitter)) {
    return;
  }
  const Dest d = go(0.12f, Bus::Sfx);
  if (d.valid) impact(material, d, sprint ? 0.34f : 0.26f);
}

void wadeStep() {
  if (playPack("entity.player.swim", Bus::Sfx, nullptr, 0.35f, kBlockJitter)) return;
  const Dest d = go(0.25f, Bus::Sfx);
  if (!d.valid) return;
  engine().burst(d, {.dur = 0.16f, .gain = 0.22f, .attack = 0.02f,
                     .filters = bp(R(700.0f, 1100.0f), 0.7f, 400.0f, 0.16f)});
  engine().tone(d, {.delay = R(0.03f, 0.08f), .freq = R(700.0f, 1100.0f),
                    .sweepTo = R(1300.0f, 1900.0f), .dur = 0.05f, .gain = 0.05f});
}

// ---- doors / containers / stations ----------------------------------------

void doorToggle(const world::BlockDef& block, bool open, const Vec3& pos) {
  const bool trapdoor = block.render == world::RenderKind::Trapdoor;
  const std::string event = std::string(trapdoor ? "block.wooden_trapdoor." : "block.wooden_door.") +
                            (open ? "open" : "close");
  if (playPack(event, Bus::Sfx, &pos, 0.5f, kVoiceJitter)) return;
  const Dest d = go(0.55f, Bus::Sfx, pos);
  if (!d.valid) return;
  // Hinge creak = stick-slip: a bending tone juddered by a fast deep tremolo
  // (~15 Hz, near-full depth) so it grinds rather than whines; the squeak partial
  // rides an octave-and-a-bit up, then the latch clacks at the end.
  const float up = open ? 1.25f : 0.8f;  // opening creaks up, closing down
  const float f0 = (block.render == world::RenderKind::Trapdoor ? 130.0f : 105.0f) * R(0.94f, 1.06f);
  const float tremRate = R(14.0f, 17.0f);
  engine().tone(d, {.wave = Wave::Sawtooth, .freq = f0, .sweepTo = f0 * up, .dur = 0.38f,
                    .gain = 0.34f, .attack = 0.04f, .tremRate = tremRate, .tremDepth = 0.9f,
                    .filter = bp(620.0f, 1.2f, 620.0f * up, 0.38f)});
  engine().tone(d, {.wave = Wave::Sawtooth, .freq = f0 * 2.7f, .sweepTo = f0 * 2.7f * up,  // the squeak
                    .dur = 0.34f, .gain = 0.12f, .attack = 0.05f, .tremRate = tremRate,
                    .tremDepth = 0.9f, .filter = bp(1300.0f, 2.0f)});
  engine().burst(d, {.dur = 0.34f, .gain = 0.16f, .attack = 0.04f, .curve = Curve::Lin,
                     .filters = bp(480.0f, 1.2f)});
  // the latch (kept under the creak)
  engine().burst(d, {.delay = 0.34f, .dur = 0.02f, .gain = 0.22f, .filters = bp(1500.0f, 2.0f)});
  engine().burst(d, {.delay = 0.365f, .dur = 0.03f, .gain = 0.16f, .filters = bp(700.0f, 2.0f)});
}

void chestOpen(const Vec3& pos) {
  if (playPack("block.chest.open", Bus::Sfx, &pos, 0.5f, kVoiceJitter)) return;
  const Dest d = go(0.65f, Bus::Sfx, pos);
  if (!d.valid) return;
  // A heavier, slower hinge than a door: low grind rising as the lid lifts.
  const float tremRate = R(12.0f, 14.0f);
  engine().tone(d, {.wave = Wave::Sawtooth, .freq = 78.0f, .sweepTo = 118.0f, .dur = 0.52f,
                    .gain = 0.32f, .attack = 0.06f, .tremRate = tremRate, .tremDepth = 0.85f,
                    .filter = bp(460.0f, 1.1f, 700.0f, 0.52f)});
  engine().tone(d, {.wave = Wave::Sawtooth, .freq = 205.0f, .sweepTo = 310.0f, .dur = 0.48f,
                    .gain = 0.11f, .attack = 0.07f, .tremRate = tremRate, .tremDepth = 0.85f,
                    .filter = bp(1100.0f, 2.0f)});
  engine().burst(d, {.dur = 0.45f, .gain = 0.14f, .attack = 0.06f, .curve = Curve::Lin,
                     .filters = bp(420.0f, 1.1f)});
}

void chestClose(const Vec3& pos) {
  if (playPack("block.chest.close", Bus::Sfx, &pos, 0.5f, kVoiceJitter)) return;
  const Dest d = go(0.5f, Bus::Sfx, pos);
  if (!d.valid) return;
  engine().tone(d, {.wave = Wave::Sawtooth, .freq = 112.0f, .sweepTo = 72.0f, .dur = 0.26f,
                    .gain = 0.28f, .attack = 0.02f, .tremRate = 13.0f, .tremDepth = 0.85f,
                    .filter = bp(620.0f, 1.2f, 380.0f, 0.26f)});
  engine().burst(d, {.dur = 0.24f, .gain = 0.12f, .attack = 0.03f, .curve = Curve::Lin,
                     .filters = bp(480.0f, 1.2f)});
  engine().burst(d, {.delay = 0.24f, .dur = 0.08f, .gain = 0.5f, .filters = lp(260.0f, 1.0f)});  // lid thud
  engine().tone(d, {.delay = 0.24f, .freq = 130.0f, .sweepTo = 70.0f, .dur = 0.08f, .gain = 0.2f});
  engine().burst(d, {.delay = 0.255f, .dur = 0.02f, .gain = 0.16f, .filters = bp(1200.0f, 2.0f)});
}

void craft() {
  if (playPack("entity.player.craft", Bus::Sfx, nullptr, 0.45f)) return;
  const Dest d = go(0.4f, Bus::Sfx);
  if (!d.valid) return;
  impactWood(d, 0.5f, 0.0f);
  // the "zip" of assembly
  engine().burst(d, {.delay = 0.1f, .dur = 0.14f, .gain = 0.16f,
                     .filters = bp(500.0f, 2.0f, 1900.0f, 0.14f)});
  // The JS fired the second knock from a setTimeout, which meant a second trip
  // through the voice cap 190 ms later. Scheduling it as a delay instead keeps the
  // rhythm exact and costs only that: the tail can no longer be dropped on its own.
  impactWood(d, 0.4f, 0.19f);
}

void smeltDone(const Vec3& pos) {
  if (playPack("block.furnace.smelt", Bus::Sfx, &pos, 0.4f)) return;
  const Dest d = go(0.3f, Bus::Sfx, pos);
  if (!d.valid) return;
  engine().tone(d, {.freq = 1320.0f, .dur = 0.18f, .gain = 0.07f});
  engine().tone(d, {.delay = 0.02f, .freq = 1980.0f, .dur = 0.24f, .gain = 0.05f});
}

// ---- player ---------------------------------------------------------------

void hurt() {
  if (playPack("entity.player.hurt", Bus::Sfx, nullptr, 0.5f, kVoiceJitter)) return;
  const Dest d = go(0.3f, Bus::Sfx);
  if (!d.valid) return;
  // A short "uhh": saw larynx dropping through a closing formant, plus breath.
  const float f0 = R(120.0f, 145.0f);
  engine().tone(d, {.wave = Wave::Sawtooth, .freq = f0, .sweepTo = f0 * 0.62f, .dur = 0.17f,
                    .gain = 0.2f, .attack = 0.012f,
                    .filter = bp(640.0f, 2.2f, 380.0f, 0.17f)});
  engine().burst(d, {.dur = 0.1f, .gain = 0.1f, .filters = bp(1400.0f, 0.8f)});
}

void died() {
  if (playPack("entity.player.death", Bus::Sfx, nullptr, 0.55f)) return;
  const Dest d = go(0.8f, Bus::Sfx);
  if (!d.valid) return;
  const float f0 = 130.0f;
  engine().tone(d, {.wave = Wave::Sawtooth, .freq = f0, .sweepTo = 48.0f, .dur = 0.7f,
                    .gain = 0.18f, .attack = 0.02f, .vibRate = 5.0f, .vibDepth = 8.0f,
                    .filter = bp(560.0f, 2.0f, 220.0f, 0.7f)});
  engine().burst(d, {.dur = 0.5f, .gain = 0.08f, .attack = 0.05f, .curve = Curve::Lin,
                     .filters = lp(500.0f, 0.7f)});
}

void land(float intensity) {
  const float s = 0.35f + intensity * 0.65f;
  // Minecraft splits the landing thump in two at roughly this height, and its
  // packs supply both, so the split is worth honouring rather than scaling one
  // clip's gain across the whole range.
  if (playPack(intensity > 0.45f ? "entity.player.big_fall" : "entity.player.small_fall",
               Bus::Sfx, nullptr, 0.6f * s, kBlockJitter)) {
    return;
  }
  const Dest d = go(0.25f, Bus::Sfx);
  if (!d.valid) return;
  engine().burst(d, {.dur = 0.1f, .gain = 0.5f * s, .filters = lp(180.0f, 0.8f)});
  engine().tone(d, {.freq = 74.0f, .sweepTo = 46.0f, .dur = 0.12f, .gain = 0.3f * s});
  if (intensity > 0.45f) {
    engine().burst(d, {.delay = 0.03f, .dur = 0.08f, .gain = 0.2f * s, .filters = bp(700.0f, 1.0f)});
  }
}

void eat() {
  if (playPack("entity.generic.eat", Bus::Sfx, nullptr, 0.5f, kVoiceJitter)) return;
  const Dest d = go(1.0f, Bus::Sfx);
  if (!d.valid) return;
  for (int i = 0; i < 3; ++i) {  // munch, munch, munch...
    engine().burst(d, {.delay = i * 0.22f + R(0.0f, 0.03f), .dur = 0.07f, .gain = 0.32f,
                       .attack = 0.008f, .filters = lp(R(650.0f, 950.0f) - i * 120.0f, 1.2f)});
    engine().burst(d, {.delay = i * 0.22f + 0.03f, .dur = 0.05f, .gain = 0.14f,
                       .filters = bp(R(1400.0f, 2100.0f), 0.9f)});
  }
  engine().tone(d, {.delay = 0.72f, .freq = 260.0f, .sweepTo = 88.0f, .dur = 0.13f,  // gulp
                    .gain = 0.14f, .filter = lp(700.0f, 1.0f)});
}

void splash(bool big) {
  const float s = big ? 1.0f : 0.55f;
  if (playPack("entity.player.splash", Bus::Sfx, nullptr, 0.6f * s, kBlockJitter)) return;
  const Dest d = go(0.6f, Bus::Sfx);
  if (!d.valid) return;
  engine().burst(d, {.dur = 0.3f, .gain = 0.4f * s, .attack = 0.012f,
                     .filters = bp(2400.0f, 0.6f, 420.0f, 0.3f)});
  for (int i = 0; i < 3; ++i) {  // droplets plink back down
    engine().tone(d, {.delay = 0.1f + i * R(0.05f, 0.09f), .freq = R(650.0f, 1250.0f),
                      .sweepTo = R(1500.0f, 2400.0f), .dur = 0.045f, .gain = 0.06f * s});
  }
}

void bubbles() {
  if (playPack("entity.player.bubbles", Bus::Sfx, nullptr, 0.25f, kVoiceJitter)) return;
  const Dest d = go(0.3f, Bus::Sfx);  // sporadic underwater blips
  if (!d.valid) return;
  for (int i = 0; i < 2; ++i) {
    engine().tone(d, {.delay = i * R(0.08f, 0.16f), .freq = R(180.0f, 340.0f),
                      .sweepTo = R(600.0f, 1100.0f), .dur = R(0.05f, 0.1f), .gain = 0.05f,
                      .filter = lp(900.0f, 1.0f)});
  }
}

void pickup() {
  if (playPack("entity.item.pickup", Bus::Sfx, nullptr, 0.35f, kBlockJitter)) return;
  const Dest d = go(0.15f, Bus::Sfx);
  if (!d.valid) return;
  const float f = R(480.0f, 620.0f);
  engine().tone(d, {.freq = f, .sweepTo = f * 1.9f, .dur = 0.09f, .gain = 0.14f, .attack = 0.004f});
  engine().burst(d, {.dur = 0.012f, .gain = 0.08f, .filters = hp(2000.0f, 0.7f)});
}

void toss() {
  if (playPack("entity.item.throw", Bus::Sfx, nullptr, 0.35f, kBlockJitter)) return;
  const Dest d = go(0.15f, Bus::Sfx);
  if (d.valid) {
    engine().burst(d, {.dur = 0.12f, .gain = 0.16f, .attack = 0.03f, .curve = Curve::Lin,
                       .filters = bp(600.0f, 1.0f, 1500.0f, 0.12f)});
  }
}

void swing() {
  if (playPack("entity.player.attack.sweep", Bus::Sfx, nullptr, 0.3f, kBlockJitter)) return;
  const Dest d = go(0.15f, Bus::Sfx);  // melee whoosh
  if (d.valid) {
    engine().burst(d, {.dur = 0.13f, .gain = 0.13f, .attack = 0.04f, .curve = Curve::Lin,
                       .filters = bp(380.0f, 1.4f, 950.0f, 0.13f)});
  }
}

void thwack(const Vec3& pos) {
  if (playPack("entity.player.attack.strong", Bus::Sfx, &pos, 0.6f, kBlockJitter)) return;
  const Dest d = go(0.15f, Bus::Sfx, pos);  // a hit landing on a mob
  if (!d.valid) return;
  engine().burst(d, {.dur = 0.07f, .gain = 0.42f, .filters = lp(500.0f, 1.0f)});
  engine().tone(d, {.freq = 200.0f, .sweepTo = 90.0f, .dur = 0.07f, .gain = 0.2f});
}

void shutter() {
  if (playPack("ui.screenshot", Bus::Ui, nullptr, 0.5f)) return;
  const Dest d = go(0.15f, Bus::Ui);  // F2 camera
  if (!d.valid) return;
  engine().burst(d, {.dur = 0.02f, .gain = 0.3f, .filters = bp(2600.0f, 2.0f)});
  engine().burst(d, {.delay = 0.07f, .dur = 0.025f, .gain = 0.24f, .filters = bp(1700.0f, 2.0f)});
}

void crit() {
  if (playPack("entity.player.attack.crit", Bus::Sfx, nullptr, 0.4f, kBlockJitter)) return;
  const Dest d = go(0.3f, Bus::Sfx);  // a bright metallic snap over the thwack
  if (!d.valid) return;
  engine().tone(d, {.wave = Wave::Sine, .freq = 1900.0f, .sweepTo = 900.0f, .dur = 0.12f,
                    .gain = 0.14f, .attack = 0.004f});
  engine().tone(d, {.delay = 0.04f, .wave = Wave::Sine, .freq = 2600.0f, .sweepTo = 1300.0f,
                    .dur = 0.1f, .gain = 0.08f});
  engine().burst(d, {.dur = 0.05f, .gain = 0.12f, .filters = hp(2400.0f, 1.0f)});
}

void warp() {
  if (playPack("entity.enderman.teleport", Bus::Sfx, nullptr, 0.55f)) return;
  const Dest d = go(0.9f, Bus::Sfx);  // a rising two-voice shimmer with an airy whoosh
  if (!d.valid) return;
  engine().tone(d, {.wave = Wave::Sine, .freq = 320.0f, .sweepTo = 1500.0f, .dur = 0.5f,
                    .gain = 0.16f, .attack = 0.02f});
  engine().tone(d, {.wave = Wave::Sine, .freq = 480.0f, .sweepTo = 2200.0f, .dur = 0.55f,
                    .gain = 0.09f, .attack = 0.06f});
  engine().burst(d, {.dur = 0.5f, .gain = 0.12f, .attack = 0.05f, .filters = hp(900.0f, 0.7f)});
  engine().tone(d, {.delay = 0.45f, .wave = Wave::Sine, .freq = 1800.0f, .sweepTo = 2600.0f,
                    .dur = 0.2f, .gain = 0.05f});
}

// ---- mob voices -----------------------------------------------------------

void sheep(MobCall kind, const Vec3& pos, int seed) {
  if (playMobPack("sheep", kind, pos, seed)) return;
  const Dest d = go(1.0f, Bus::Sfx, pos);
  if (!d.valid) return;
  // The bleat: a LOW larynx (200-260 Hz — higher reads as an insect) chopped hard by
  // ~9.5 Hz amplitude tremolo (the "eh-eh-eh"), voiced through open /a/ formants at
  // ~700/1100 Hz. No pitch vibrato — that is what buzzes.
  const float f0 = R(205.0f, 255.0f) * (0.92f + (seed % 7) * 0.025f) *
                   (kind == MobCall::Hurt ? 1.2f : kind == MobCall::Death ? 0.85f : 1.0f);
  const float dur = kind == MobCall::Hurt ? 0.35f : kind == MobCall::Death ? 0.9f : R(0.6f, 0.75f);
  const float droop = kind == MobCall::Death ? 0.6f : 0.82f;
  const float tremRate = R(9.0f, 10.5f);
  engine().tone(d, {.wave = Wave::Sawtooth, .freq = f0, .sweepTo = f0 * droop,  // chest/body
                    .dur = dur, .gain = 0.3f, .attack = 0.07f, .tremRate = tremRate,
                    .tremDepth = 0.85f, .filter = lp(1300.0f, 0.8f)});
  engine().tone(d, {.wave = Wave::Sawtooth, .freq = f0 * 1.008f, .sweepTo = f0 * droop,  // /a/ formant
                    .dur = dur, .gain = 0.16f, .attack = 0.07f, .tremRate = tremRate,
                    .tremDepth = 0.85f, .filter = bp(700.0f, 1.6f)});
  engine().tone(d, {.wave = Wave::Sawtooth, .freq = f0 * 0.994f, .sweepTo = f0 * droop,  // upper colour
                    .dur = dur, .gain = 0.07f, .attack = 0.09f, .tremRate = tremRate,
                    .tremDepth = 0.85f, .filter = bp(1150.0f, 2.5f)});
  engine().burst(d, {.dur = dur, .gain = 0.05f, .attack = 0.08f, .curve = Curve::Lin,  // breath
                     .filters = bp(1100.0f, 0.8f)});
}

void pig(MobCall kind, const Vec3& pos, int seed) {
  if (playMobPack("pig", kind, pos, seed)) return;
  const Dest d = go(0.4f, Bus::Sfx, pos);
  if (!d.valid) return;
  // The oink: a snorty grunt — the nasal formant swings up then down fast.
  const float f0 = R(95.0f, 125.0f) * (0.92f + (seed % 5) * 0.04f) *
                   (kind == MobCall::Hurt ? 1.35f : kind == MobCall::Death ? 0.8f : 1.0f);
  const float dur = kind == MobCall::Hurt ? 0.18f : kind == MobCall::Death ? 0.42f : 0.26f;
  engine().burst(d, {.dur = 0.05f, .gain = 0.14f, .filters = hp(2000.0f, 0.7f)});  // nostril snort
  engine().tone(d, {.wave = Wave::Sawtooth, .freq = f0, .sweepTo = f0 * 0.8f, .dur = dur,
                    .gain = 0.2f, .attack = 0.02f, .vibRate = 26.0f, .vibDepth = f0 * 0.15f,
                    .filter = bp(800.0f, 2.6f, 1350.0f, dur * 0.45f)});
  engine().tone(d, {.delay = dur * 0.1f, .wave = Wave::Sawtooth, .freq = f0 * 0.996f,
                    .dur = dur * 0.9f, .gain = 0.08f,
                    .filter = bp(1400.0f, 3.0f, 700.0f, dur * 0.9f)});
}

void cow(MobCall kind, const Vec3& pos, int seed) {
  if (playMobPack("cow", kind, pos, seed)) return;
  const float dur = kind == MobCall::Hurt ? 0.4f : kind == MobCall::Death ? 1.1f : R(0.8f, 1.15f);
  const Dest d = go(dur + 0.2f, Bus::Sfx, pos);
  if (!d.valid) return;
  // The moo: a deep chesty larynx swelling up a shade as the mouth opens (a bandpass
  // "throat" sweeping from a closed m-hum up to a round oo), then sagging. Slow, mild
  // vibrato — anything faster reads as a bleat.
  const float f0 = R(88.0f, 112.0f) * (0.94f + (seed % 6) * 0.03f) *
                   (kind == MobCall::Hurt ? 1.3f : kind == MobCall::Death ? 0.78f : 1.0f);
  engine().tone(d, {.wave = Wave::Sawtooth, .freq = f0 * 0.85f,
                    .sweepTo = f0 * (kind == MobCall::Death ? 0.6f : 0.96f), .dur = dur,
                    .gain = 0.26f, .attack = 0.12f, .vibRate = 3.4f, .vibDepth = f0 * 0.04f,
                    .filter = lp(900.0f, 0.8f)});
  engine().tone(d, {.wave = Wave::Sawtooth, .freq = f0 * 0.852f, .sweepTo = f0 * 0.95f,  // opening mouth
                    .dur = dur, .gain = 0.15f, .attack = 0.14f,
                    .filter = bp(300.0f, 1.7f, 560.0f, dur * 0.6f)});
  engine().burst(d, {.dur = dur * 0.7f, .gain = 0.04f, .attack = 0.15f, .curve = Curve::Lin,
                     .filters = bp(700.0f, 0.8f)});  // breath
}

void zombie(MobCall kind, const Vec3& pos, int seed) {
  if (playMobPack("zombie", kind, pos, seed)) return;
  const float dur = kind == MobCall::Death ? 1.3f : kind == MobCall::Hurt ? 0.32f : R(0.85f, 1.2f);
  const Dest d = go(dur + 0.2f, Bus::Sfx, pos);
  if (!d.valid) return;
  // The groan: a low ragged saw sagging in pitch through two dark formants.
  const float f0 = (kind == MobCall::Hurt ? R(110.0f, 135.0f) : R(72.0f, 95.0f)) *
                   (0.94f + (seed % 6) * 0.025f);
  const float drop = kind == MobCall::Death ? 0.45f : 0.72f;
  engine().tone(d, {.wave = Wave::Sawtooth, .freq = f0, .sweepTo = f0 * drop, .dur = dur,
                    .gain = 0.16f, .attack = kind == MobCall::Hurt ? 0.015f : 0.09f,
                    .vibRate = 4.2f, .vibDepth = f0 * 0.08f, .tremRate = 6.5f, .tremDepth = 0.3f,
                    .filter = bp(520.0f, 1.9f, 330.0f, dur)});
  engine().tone(d, {.wave = Wave::Sawtooth, .freq = f0 * 1.007f, .sweepTo = f0 * drop, .dur = dur,
                    .gain = 0.07f, .attack = 0.09f, .filter = bp(1150.0f, 3.0f, 760.0f, dur)});
  engine().burst(d, {.dur = dur, .gain = 0.05f, .attack = 0.1f, .curve = Curve::Lin,
                     .filters = bp(900.0f, 0.7f, 500.0f, dur)});
}

void sizzle(const Vec3& pos) {
  if (playPack("entity.generic.burn", Bus::Sfx, &pos, 0.45f, kVoiceJitter)) return;
  const Dest d = go(0.5f, Bus::Sfx, pos);  // zombie burning in the sun
  if (!d.valid) return;
  engine().burst(d, {.dur = 0.4f, .gain = 0.2f, .attack = 0.02f, .curve = Curve::Lin,
                     .noise = NoiseKind::Crackle, .filters = hp(1300.0f, 0.7f)});
  engine().burst(d, {.dur = 0.3f, .gain = 0.08f, .filters = bp(3200.0f, 0.8f)});
}

// ---- UI -------------------------------------------------------------------

void uiClick() {
  // No jitter on the interface: a button that answers at a slightly different
  // pitch each press reads as a fault rather than as variety.
  if (playPack("ui.button.click", Bus::Ui, nullptr, 0.45f)) return;
  const Dest d = go(0.08f, Bus::Ui);
  if (!d.valid) return;
  engine().burst(d, {.dur = 0.018f, .gain = 0.2f, .filters = bp(1350.0f, 2.2f)});
  engine().tone(d, {.freq = 1900.0f, .dur = 0.03f, .gain = 0.05f});
}

void uiSlot() {
  if (playPack("ui.slot.click", Bus::Ui, nullptr, 0.35f)) return;
  const Dest d = go(0.06f, Bus::Ui);  // inventory slot tick (very quiet)
  if (d.valid) engine().burst(d, {.dur = 0.014f, .gain = 0.13f, .filters = bp(900.0f, 2.0f)});
}

// ---- network relay ----------------------------------------------------------

void playNamed(const std::string& kind, const Vec3& pos) {
  // A table, deliberately: the string comes from a peer, so it selects between
  // sounds and can never select between behaviours. Anything unrecognised is
  // silently ignored rather than logged, because logging it would let a peer fill
  // the log file.
  struct MobEntry {
    const char* prefix;
    void (*play)(MobCall, const Vec3&, int);
  };
  static constexpr MobEntry kMobs[] = {
      {"sheep", &sheep}, {"pig", &pig}, {"cow", &cow}, {"zombie", &zombie},
  };

  if (kind == "thwack") {
    thwack(pos);
    return;
  }
  if (kind == "sizzle") {
    sizzle(pos);
    return;
  }
  for (const MobEntry& entry : kMobs) {
    const std::string prefix = entry.prefix;
    if (kind.rfind(prefix, 0) != 0) continue;
    const std::string suffix = kind.substr(prefix.size());
    // The seed picks the individual's voice; a relayed call has no entity id to
    // hand, so it comes off the position and stays stable for a stationary mob.
    const int seed = static_cast<int>(pos.x * 7.0f) ^ static_cast<int>(pos.z * 13.0f);
    if (suffix == "_hurt") entry.play(MobCall::Hurt, pos, seed);
    else if (suffix == "_death") entry.play(MobCall::Death, pos, seed);
    else if (suffix.empty() || suffix == "_say") entry.play(MobCall::Say, pos, seed);
    return;
  }
}

}  // namespace hr::audio::sfx
