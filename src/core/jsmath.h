// JavaScript numeric semantics, for the code paths where a plain C++ reading
// silently produces different results.
//
// The world generator is a direct transcription of js/world/worldgen.js and
// js/world/noise.js, and its output is validated against the JS by
// tools/gen_golden.mjs. Four things in that code are not what a C++ programmer
// would naturally write:
//
//  1. `Math.imul(a, b)` is a 32-bit wrapping multiply. Plain `a * b` on 64-bit
//     ints does not wrap, so the hash functions diverge immediately.
//  2. `x >>> n` is a *logical* shift on the ToUint32 value. Every bit-mixing
//     step in js/core/prng.js uses it, never `>>`.
//  3. `Math.floor(a / b)` on integers rounds toward negative infinity, whereas
//     C++ integer division truncates toward zero. js/world/worldgen.js:398 uses
//     `Math.floor(wx / 12)`, so getting this wrong shifts the flower-dominance
//     grid across the whole negative-coordinate half of every world.
//  4. `Math.min`/`Math.max` propagate NaN where std::min/max do not.
//
// Everything here operates on uint32_t: XOR, logical shift, left shift and
// wrapping multiply all produce identical bit patterns whether the value is read
// as signed or unsigned, so the JS's implicit ToInt32/ToUint32 round trips need
// no explicit conversion.

#pragma once

#include <cmath>
#include <cstdint>

namespace hr::jsmath {

// Math.imul: the low 32 bits of the product, i.e. multiplication mod 2^32.
constexpr std::uint32_t imul(std::uint32_t a, std::uint32_t b) { return a * b; }

// `x >>> n`. Written out so call sites read like the JS they came from.
constexpr std::uint32_t ushr(std::uint32_t x, int n) { return x >> n; }

// `x << n` on a 32-bit value.
constexpr std::uint32_t shl(std::uint32_t x, int n) { return x << n; }

// rotl, which is how `(h << 13) | (h >>> 19)` reads in js/core/prng.js:10.
constexpr std::uint32_t rotl(std::uint32_t x, int n) {
  return (x << n) | (x >> (32 - n));
}

// `(x >>> 0) / 4294967296` — a uint32 mapped into [0, 1). The divide must be
// double: doing it in float loses the low mantissa bits, and the permutation
// shuffle in noise.js truncates the result, so a float here permutes the whole
// table differently and yields an unrelated world.
constexpr double toUnitDouble(std::uint32_t x) { return static_cast<double>(x) / 4294967296.0; }

// `value | 0` on a double: truncate toward zero. Used where the JS turns a
// double into an array index, as in `(rng() * (i + 1)) | 0`.
inline std::int32_t truncToInt(double value) { return static_cast<std::int32_t>(value); }

// `Math.floor(value) & 255`. Negative coordinates matter here: Math.floor(-3.2)
// is -4, and -4 & 255 is 252, which two's-complement C++ reproduces exactly.
inline std::int32_t floorAnd255(double value) {
  return static_cast<std::int32_t>(std::floor(value)) & 255;
}

// Math.floor(a / b) for integers, with b > 0. C++ `/` truncates toward zero, so
// -1 / 12 is 0 where JavaScript gives -1.
constexpr std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
  const std::int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// Math.round, which rounds a half UP (toward +infinity). std::round rounds a half
// AWAY FROM ZERO, so the two disagree for every negative half: Math.round(-4.5) is -4,
// std::round(-4.5) is -5.
//
// This is not pedantry. The Atlas positions each 16x16 chunk tile at
// `round(tx * 16 - playerX + halfSize)`, and consecutive tiles differ by exactly 16 —
// so with std::round the spacing comes out as 16 everywhere except across zero, where
// it becomes 17 and opens a one-pixel seam straight across the map.
inline double jsRound(double value) { return std::floor(value + 0.5); }
inline float jsRoundF(float value) { return std::floor(value + 0.5f); }

// Math.min / Math.max, NaN-propagating like the JS versions. std::min and
// std::max return the first argument for any NaN comparison instead.
inline double jsMin(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) return NAN;
  return a < b ? a : b;
}
inline double jsMax(double a, double b) {
  if (std::isnan(a) || std::isnan(b)) return NAN;
  return a > b ? a : b;
}

}  // namespace hr::jsmath
