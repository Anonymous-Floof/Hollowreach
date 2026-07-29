// Seeded pseudo-random generation, ported line for line from js/core/prng.js.
//
// Two distinct uses, and the distinction matters:
//
//  * `Mulberry32` is a *sequence* generator. Only the noise permutation tables
//    and the texture painters use it, and both consume it in a fixed order.
//  * `hash2i` / `hash3i` are *stateless* — "does a tree grow at (x, z)?" with no
//    ordering dependency, which is what lets worldgen produce identical chunks
//    on any thread in any order.
//
// A warning carried over from js/world/worldgen.js:386-389: hash2i barely
// avalanches a single flipped seed bit, so two rolls at the same coordinates with
// different seed salts come out correlated. Secondary rolls must offset their
// coordinates, not just the seed. The ported call sites keep those offsets.

#pragma once

#include <cstdint>
#include <string_view>

#include "core/jsmath.h"

namespace hr {

// xmur3: hashes a string into a 32-bit seed. Used for texture names, so every
// painter gets a stable per-texture seed.
std::uint32_t hashSeed(std::string_view str);

// The same, for a numeric seed entered by the player.
std::uint32_t hashSeedNumber(double value);

// mulberry32. Deliberately a small struct rather than a std::function closure:
// the painters and the permutation shuffle call it in tight loops.
class Mulberry32 {
 public:
  explicit Mulberry32(std::uint32_t seed) : a_(seed) {}

  // Returns a double in [0, 1). Must stay double — see jsmath::toUnitDouble.
  double next() {
    a_ = a_ + 0x6d2b79f5u;
    std::uint32_t t = jsmath::imul(a_ ^ jsmath::ushr(a_, 15), 1u | a_);
    t = (t + jsmath::imul(t ^ jsmath::ushr(t, 7), 61u | t)) ^ t;
    return jsmath::toUnitDouble(t ^ jsmath::ushr(t, 14));
  }

  // Convenience for the painters, which mostly want "an int in [0, n)".
  int nextInt(int n) { return static_cast<int>(next() * n); }
  // ...and "does this happen?".
  bool chance(double probability) { return next() < probability; }

 private:
  std::uint32_t a_;
};

// Stateless coordinate hashes returning [0, 1).
double hash2i(std::uint32_t seed, std::int32_t x, std::int32_t y);
double hash3i(std::uint32_t seed, std::int32_t x, std::int32_t y, std::int32_t z);

// Math.random(): unseeded and deliberately *not* reproducible. This is a third use,
// distinct from both above, and the distinction is the point — it must never appear
// in worldgen, the noise tables or anything a multiplayer guest recomputes locally.
// Its call sites are the ones the web build used it for: a random world seed, and the
// coin flip when eating rotten flesh.
double randomUnit();
std::uint32_t randomSeed();

}  // namespace hr
