// Floating-point discipline for this translation unit.
//
// MSVC has no command-line switch to disable FMA contraction (there is /fp:fast
// and /fp:precise, but no /fp:contract-), so it is turned off here at the source
// level. Contraction would fold `sum + amp * noise(...)` in fbm into a single
// fused multiply-add with one rounding instead of two, changing the low bits of
// every terrain height. That would be invisible in a single-player world but not
// across a multiplayer session, where guests generate the terrain locally from
// the seed and must agree with the host.
#if defined(_MSC_VER)
#pragma float_control(precise, on, push)
#pragma fp_contract(off)
#elif defined(__clang__) || defined(__GNUC__)
#pragma STDC FP_CONTRACT OFF
#endif

#include "world/noise.h"

#include <cmath>

#include "core/jsmath.h"
#include "core/prng.h"

namespace hr {

Noise::Noise(std::uint32_t seed) {
  Mulberry32 rng(seed);

  std::uint8_t p[256];
  for (int i = 0; i < 256; ++i) p[i] = static_cast<std::uint8_t>(i);

  // Fisher-Yates, descending and inclusive of i, with the index truncated from a
  // double. This is the single highest-consequence loop in the port: iterating
  // the other direction, using a modulo, or computing rng() in float all permute
  // the table differently, and the result is a completely unrelated world rather
  // than a subtly different one.
  for (int i = 255; i > 0; --i) {
    const int j = jsmath::truncToInt(rng.next() * (i + 1));
    const std::uint8_t t = p[i];
    p[i] = p[j];
    p[j] = t;
  }

  for (int i = 0; i < 512; ++i) perm_[i] = p[i & 255];
}

double Noise::grad2(std::uint8_t hash, double x, double y) {
  switch (hash & 3) {
    case 0: return x + y;
    case 1: return -x + y;
    case 2: return x - y;
    default: return -x - y;
  }
}

double Noise::grad3(std::uint8_t hash, double x, double y, double z) {
  const int h = hash & 15;
  const double u = h < 8 ? x : y;
  const double v = h < 4 ? y : ((h == 12 || h == 14) ? x : z);
  return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}

double Noise::noise2(double x, double y) const {
  const auto& p = perm_;
  const std::int32_t X = jsmath::floorAnd255(x);
  const std::int32_t Y = jsmath::floorAnd255(y);
  x -= std::floor(x);
  y -= std::floor(y);
  const double u = fade(x);
  const double v = fade(y);

  const std::uint8_t aa = p[p[X] + Y];
  const std::uint8_t ab = p[p[X] + Y + 1];
  const std::uint8_t ba = p[p[X + 1] + Y];
  const std::uint8_t bb = p[p[X + 1] + Y + 1];

  const double x1 = lerp(grad2(aa, x, y), grad2(ba, x - 1.0, y), u);
  const double x2 = lerp(grad2(ab, x, y - 1.0), grad2(bb, x - 1.0, y - 1.0), u);
  return lerp(x1, x2, v);
}

double Noise::noise3(double x, double y, double z) const {
  const auto& p = perm_;
  const std::int32_t X = jsmath::floorAnd255(x);
  const std::int32_t Y = jsmath::floorAnd255(y);
  const std::int32_t Z = jsmath::floorAnd255(z);
  x -= std::floor(x);
  y -= std::floor(y);
  z -= std::floor(z);
  const double u = fade(x);
  const double v = fade(y);
  const double w = fade(z);

  const int A = p[X] + Y, AA = p[A] + Z, AB = p[A + 1] + Z;
  const int B = p[X + 1] + Y, BA = p[B] + Z, BB = p[B + 1] + Z;

  return lerp(
      lerp(lerp(grad3(p[AA], x, y, z), grad3(p[BA], x - 1.0, y, z), u),
           lerp(grad3(p[AB], x, y - 1.0, z), grad3(p[BB], x - 1.0, y - 1.0, z), u), v),
      lerp(lerp(grad3(p[AA + 1], x, y, z - 1.0), grad3(p[BA + 1], x - 1.0, y, z - 1.0), u),
           lerp(grad3(p[AB + 1], x, y - 1.0, z - 1.0), grad3(p[BB + 1], x - 1.0, y - 1.0, z - 1.0),
                u),
           v),
      w);
}

double Noise::fbm2(double x, double y, int octaves, double lacunarity, double gain) const {
  // The expression shape is load-bearing. `x * freq` must not be folded into a
  // precomputed constant per octave, and `freq *= lacunarity` must not become a
  // power of two by shifting: the accumulated rounding is part of the output the
  // worldgen thresholds were tuned against.
  double amp = 1.0, freq = 1.0, sum = 0.0, norm = 0.0;
  for (int i = 0; i < octaves; ++i) {
    sum += amp * noise2(x * freq, y * freq);
    norm += amp;
    amp *= gain;
    freq *= lacunarity;
  }
  return sum / norm;
}

double Noise::fbm3(double x, double y, double z, int octaves, double lacunarity,
                   double gain) const {
  double amp = 1.0, freq = 1.0, sum = 0.0, norm = 0.0;
  for (int i = 0; i < octaves; ++i) {
    sum += amp * noise3(x * freq, y * freq, z * freq);
    norm += amp;
    amp *= gain;
    freq *= lacunarity;
  }
  return sum / norm;
}

}  // namespace hr

#if defined(_MSC_VER)
#pragma float_control(pop)
#endif
