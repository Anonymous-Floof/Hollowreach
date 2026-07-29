#include "core/bytes.h"

#include <array>

namespace hr {
namespace {

// The standard reflected table, built once on first use rather than written out.
std::array<std::uint32_t, 256> makeTable() {
  std::array<std::uint32_t, 256> table {};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    table[i] = c;
  }
  return table;
}

}  // namespace

std::uint32_t crc32(const void* data, std::size_t size, std::uint32_t seed) {
  static const std::array<std::uint32_t, 256> table = makeTable();
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::uint32_t c = seed ^ 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

}  // namespace hr
