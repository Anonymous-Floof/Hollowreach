#include "game/painting.h"

#include <algorithm>
#include <atomic>

#include "core/log.h"
#include "resource/image.h"

namespace hr::game {

std::uint64_t nextPaintingStamp() {
  // Starts at 1, so a default-constructed Painting's 0 can never be mistaken for
  // a real picture by anything comparing stamps.
  static std::atomic<std::uint64_t> counter {1};
  return counter.fetch_add(1);
}

bool paintingFromPng(const std::string& path, Painting& out) {
  Image src;
  std::string error;
  if (!Image::loadPng(path, src, &error) || src.empty()) {
    log::warn("painting: could not read %s (%s)", path.c_str(), error.c_str());
    return false;
  }

  // Centre-crop to a square.
  const int side = std::min(src.width(), src.height());
  const int ox = (src.width() - side) / 2;
  const int oy = (src.height() - side) / 2;

  // Box filter, not the nearest-neighbour scaler the atlas uses. That one exists to
  // bring 16px art UP without softening it; going the other way it would throw away
  // 99 pixels in every 100 and turn a photograph into noise.
  Painting made;
  made.rgb.resize(kPaintingBytes);
  for (int y = 0; y < kPaintingSize; ++y) {
    // Integer edges, so every source pixel lands in exactly one cell and none is
    // counted twice however awkward the source size is.
    const int y0 = oy + static_cast<int>(static_cast<std::int64_t>(y) * side / kPaintingSize);
    const int y1 =
        oy + static_cast<int>(static_cast<std::int64_t>(y + 1) * side / kPaintingSize);
    for (int x = 0; x < kPaintingSize; ++x) {
      const int x0 = ox + static_cast<int>(static_cast<std::int64_t>(x) * side / kPaintingSize);
      const int x1 =
          ox + static_cast<int>(static_cast<std::int64_t>(x + 1) * side / kPaintingSize);
      std::uint32_t r = 0, g = 0, b = 0, n = 0;
      for (int sy = y0; sy < std::max(y1, y0 + 1); ++sy) {
        for (int sx = x0; sx < std::max(x1, x0 + 1); ++sx) {
          const Rgba p = src.get(sx, sy);
          r += p.r;
          g += p.g;
          b += p.b;
          ++n;
        }
      }
      if (n == 0) n = 1;
      const std::size_t o = (static_cast<std::size_t>(y) * kPaintingSize + x) * 3;
      made.rgb[o + 0] = static_cast<std::uint8_t>(r / n);
      made.rgb[o + 1] = static_cast<std::uint8_t>(g / n);
      made.rgb[o + 2] = static_cast<std::uint8_t>(b / n);
    }
  }

  const std::size_t slash = path.find_last_of("/\\");
  made.source = slash == std::string::npos ? path : path.substr(slash + 1);
  out = std::move(made);
  return true;
}

}  // namespace hr::game
