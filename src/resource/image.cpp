#include "resource/image.h"

#include <algorithm>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
// Resource packs ship PNGs; nothing needs the other decoders. STBI_NO_STDIO is
// deliberately not defined — stb tests it with #ifdef, so even `#define ... 0`
// would strip the file-based loaders we use.
#define STBI_ONLY_PNG
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "core/log.h"

namespace hr {

void Image::resize(int width, int height, Rgba fill) {
  width_ = std::max(0, width);
  height_ = std::max(0, height);
  pixels_.assign(static_cast<std::size_t>(width_) * height_ * 4, 0);
  if (fill.a != 0 || fill.r != 0 || fill.g != 0 || fill.b != 0) clear(fill);
}

void Image::clear(Rgba fill) {
  for (std::size_t i = 0; i < pixels_.size(); i += 4) {
    pixels_[i] = fill.r;
    pixels_[i + 1] = fill.g;
    pixels_[i + 2] = fill.b;
    pixels_[i + 3] = fill.a;
  }
}

void Image::blend(int x, int y, Rgba c) {
  if (!inBounds(x, y) || c.a == 0) return;
  if (c.a == 255) {
    set(x, y, c);
    return;
  }
  std::uint8_t* p = &pixels_[(static_cast<std::size_t>(y) * width_ + x) * 4];
  const int sa = c.a;
  const int ia = 255 - sa;
  // Straight (non-premultiplied) source-over, rounded.
  const int outA = sa + p[3] * ia / 255;
  if (outA == 0) {
    p[0] = p[1] = p[2] = p[3] = 0;
    return;
  }
  p[0] = static_cast<std::uint8_t>((c.r * sa + p[0] * p[3] * ia / 255) / outA);
  p[1] = static_cast<std::uint8_t>((c.g * sa + p[1] * p[3] * ia / 255) / outA);
  p[2] = static_cast<std::uint8_t>((c.b * sa + p[2] * p[3] * ia / 255) / outA);
  p[3] = static_cast<std::uint8_t>(outA);
}

void Image::fillRect(int x, int y, int w, int h, Rgba c) {
  const int x1 = std::min(width_, x + w);
  const int y1 = std::min(height_, y + h);
  for (int yy = std::max(0, y); yy < y1; ++yy) {
    for (int xx = std::max(0, x); xx < x1; ++xx) set(xx, yy, c);
  }
}

void Image::blit(const Image& src, int dx, int dy, bool skipTransparent) {
  blitRegion(src, 0, 0, src.width(), src.height(), dx, dy, skipTransparent);
}

void Image::blitRegion(const Image& src, int sx, int sy, int sw, int sh, int dx, int dy,
                       bool skipTransparent) {
  for (int y = 0; y < sh; ++y) {
    for (int x = 0; x < sw; ++x) {
      Rgba c = src.get(sx + x, sy + y);
      if (skipTransparent && c.a == 0) continue;
      set(dx + x, dy + y, c);
    }
  }
}

Image Image::scaledNearest(int newWidth, int newHeight) const {
  Image out(newWidth, newHeight);
  if (empty() || newWidth <= 0 || newHeight <= 0) return out;
  for (int y = 0; y < newHeight; ++y) {
    const int sy = y * height_ / newHeight;
    for (int x = 0; x < newWidth; ++x) {
      out.set(x, y, get(x * width_ / newWidth, sy));
    }
  }
  return out;
}

Image Image::downsampleHalfAlphaWeighted() const {
  const int w = std::max(1, width_ / 2);
  const int h = std::max(1, height_ / 2);
  Image out(w, h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int aSum = 0, rSum = 0, gSum = 0, bSum = 0;
      for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
          Rgba c = get(std::min(x * 2 + dx, width_ - 1), std::min(y * 2 + dy, height_ - 1));
          aSum += c.a;
          rSum += c.r * c.a;
          gSum += c.g * c.a;
          bSum += c.b * c.a;
        }
      }
      if (aSum == 0) {
        out.set(x, y, {0, 0, 0, 0});
      } else {
        out.set(x, y,
                {static_cast<std::uint8_t>(rSum / aSum), static_cast<std::uint8_t>(gSum / aSum),
                 static_cast<std::uint8_t>(bSum / aSum),
                 static_cast<std::uint8_t>(aSum / 4)});
      }
    }
  }
  return out;
}

Image Image::subImage(int x, int y, int w, int h) const {
  Image out(w, h);
  out.blitRegion(*this, x, y, w, h, 0, 0, /*skipTransparent=*/false);
  return out;
}

bool Image::writePng(const std::string& path) const {
  if (empty()) return false;
  stbi_write_png_compression_level = 6;
  const int ok = stbi_write_png(path.c_str(), width_, height_, 4, pixels_.data(), width_ * 4);
  if (!ok) log::error("image: failed to write %s", path.c_str());
  return ok != 0;
}

bool Image::loadPng(const std::string& path, Image& out, std::string* errorOut) {
  int w = 0, h = 0, channels = 0;
  stbi_uc* data = stbi_load(path.c_str(), &w, &h, &channels, 4);
  if (!data) {
    if (errorOut) *errorOut = stbi_failure_reason() ? stbi_failure_reason() : "unknown error";
    return false;
  }
  out.resize(w, h);
  std::memcpy(out.data(), data, static_cast<std::size_t>(w) * h * 4);
  stbi_image_free(data);
  return true;
}

bool Image::loadPngFromMemory(const std::uint8_t* bytes, std::size_t size, Image& out,
                              std::string* errorOut) {
  int w = 0, h = 0, channels = 0;
  stbi_uc* data = stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &channels, 4);
  if (!data) {
    if (errorOut) *errorOut = stbi_failure_reason() ? stbi_failure_reason() : "unknown error";
    return false;
  }
  out.resize(w, h);
  std::memcpy(out.data(), data, static_cast<std::size_t>(w) * h * 4);
  stbi_image_free(data);
  return true;
}

}  // namespace hr
