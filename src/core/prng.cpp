#include "core/prng.h"

#include <cstdio>
#include <random>

namespace hr {

using jsmath::imul;
using jsmath::rotl;
using jsmath::toUnitDouble;
using jsmath::ushr;

std::uint32_t hashSeed(std::string_view str) {
  std::uint32_t h = 1779033703u ^ static_cast<std::uint32_t>(str.size());
  for (char ch : str) {
    // JS iterates UTF-16 code units via charCodeAt. Texture and block keys are
    // pure ASCII, so a byte-at-a-time walk is identical; the cast keeps a
    // high-bit byte from sign-extending if a non-ASCII name ever appears.
    h = imul(h ^ static_cast<std::uint8_t>(ch), 3432918353u);
    h = rotl(h, 13);
  }
  h = imul(h ^ ushr(h, 16), 2246822507u);
  h = imul(h ^ ushr(h, 13), 3266489909u);
  return h ^ ushr(h, 16);
}

std::uint32_t hashSeedNumber(double value) {
  // The JS did String(seed) before hashing, so a numeric seed has to be rendered
  // the same way to land on the same world. %.17g round-trips a double and
  // matches JS integer formatting for the integral seeds actually used.
  char buffer[40];
  if (value == std::floor(value) && std::abs(value) < 1e15) {
    std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  }
  return hashSeed(buffer);
}

double hash2i(std::uint32_t seed, std::int32_t x, std::int32_t y) {
  std::uint32_t h = seed ^ imul(static_cast<std::uint32_t>(x), 374761393u) ^
                    imul(static_cast<std::uint32_t>(y), 668265263u);
  h = imul(h ^ ushr(h, 13), 1274126177u);
  return toUnitDouble(h ^ ushr(h, 16));
}

double hash3i(std::uint32_t seed, std::int32_t x, std::int32_t y, std::int32_t z) {
  std::uint32_t h = seed ^ imul(static_cast<std::uint32_t>(x), 374761393u) ^
                    imul(static_cast<std::uint32_t>(y), 668265263u) ^
                    imul(static_cast<std::uint32_t>(z), 2147483647u);
  h = imul(h ^ ushr(h, 13), 1274126177u);
  h = imul(h ^ ushr(h, 9), 2246822519u);
  return toUnitDouble(h ^ ushr(h, 16));
}

namespace {
// Thread-local so a worker calling randomUnit() cannot race, and so a call from a
// job thread can never perturb the main thread's sequence — which would look exactly
// like a determinism bug while being nothing of the sort.
std::mt19937& entropy() {
  static thread_local std::mt19937 gen(std::random_device {}());
  return gen;
}
}  // namespace

double randomUnit() {
  return std::uniform_real_distribution<double>(0.0, 1.0)(entropy());
}

std::uint32_t randomSeed() {
  return static_cast<std::uint32_t>(entropy()());
}

}  // namespace hr
