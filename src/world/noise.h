// Perlin gradient noise (2D + 3D) plus fbm, ported from js/world/noise.js.
//
// Everything is `double`, deliberately. A single float in this chain produces a
// different world: the permutation shuffle truncates `rng() * (i + 1)` to an
// index, so one boundary case landing differently permutes the entire table, and
// the fbm accumulation is normalised by a running sum whose rounding is part of
// the output. The thresholds in worldgen are tuned against the measured
// distribution of these exact values (roughly ±0.26 at the 10th/90th percentile
// for a 3-octave field, not ±1), so drift changes which biomes appear.
//
// See noise.cpp for the floating-point pragmas that keep the compiler from
// reassociating or contracting any of it.

#pragma once

#include <array>
#include <cstdint>

namespace hr {

class Noise {
 public:
  explicit Noise(std::uint32_t seed);

  double noise2(double x, double y) const;
  double noise3(double x, double y, double z) const;

  // Fractal Brownian motion. Defaults match the JS signature so ported call
  // sites can pass exactly the arguments they did before.
  double fbm2(double x, double y, int octaves = 4, double lacunarity = 2.0,
              double gain = 0.5) const;
  double fbm3(double x, double y, double z, int octaves = 4, double lacunarity = 2.0,
              double gain = 0.5) const;

  // Exposed so tools/gen_golden.mjs can be diffed against the C++ table: if the
  // shuffle is wrong, every downstream value is wrong, and comparing the table
  // directly is what makes that failure obvious instead of mysterious.
  const std::array<std::uint8_t, 512>& permutation() const { return perm_; }

 private:
  static double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
  static double lerp(double a, double b, double t) { return a + t * (b - a); }
  static double grad2(std::uint8_t hash, double x, double y);
  static double grad3(std::uint8_t hash, double x, double y, double z);

  std::array<std::uint8_t, 512> perm_ {};
};

}  // namespace hr
