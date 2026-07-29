#include "ui/imagefx.h"

#include <algorithm>
#include <cmath>

namespace hr::ui {
namespace {

Rgba sampleStops(const std::vector<GradientStop>& stops, float t) {
  if (stops.empty()) return {};
  if (t <= stops.front().position) return stops.front().color;
  if (t >= stops.back().position) return stops.back().color;
  for (std::size_t i = 1; i < stops.size(); ++i) {
    if (t > stops[i].position) continue;
    const GradientStop& a = stops[i - 1];
    const GradientStop& b = stops[i];
    const float span = b.position - a.position;
    const float f = span > 1e-6f ? (t - a.position) / span : 0.0f;
    const auto mix = [f](std::uint8_t x, std::uint8_t y) {
      return static_cast<std::uint8_t>(
          std::lround(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * f));
    };
    return {mix(a.color.r, b.color.r), mix(a.color.g, b.color.g), mix(a.color.b, b.color.b),
            mix(a.color.a, b.color.a)};
  }
  return stops.back().color;
}

}  // namespace

void applyVerticalGradient(Image& image, const std::vector<GradientStop>& stops) {
  if (image.empty() || stops.empty()) return;
  const float h = static_cast<float>(image.height());
  for (int y = 0; y < image.height(); ++y) {
    // Sample at the pixel centre, which is where the browser evaluates a gradient for
    // that row.
    const Rgba c = sampleStops(stops, (static_cast<float>(y) + 0.5f) / h);
    for (int x = 0; x < image.width(); ++x) {
      Rgba p = image.get(x, y);
      if (p.a == 0) continue;
      p.r = c.r;
      p.g = c.g;
      p.b = c.b;
      image.set(x, y, p);
    }
  }
}

Image dropShadowLayer(const Image& source, float blurRadius, Rgba color) {
  if (source.empty()) return {};
  const float sigma = std::max(0.0001f, blurRadius * 0.5f);
  const int radius = static_cast<int>(std::ceil(sigma * 3.0f));
  const int pad = radius;
  const int w = source.width() + pad * 2;
  const int h = source.height() + pad * 2;

  std::vector<float> kernel(static_cast<std::size_t>(radius) * 2 + 1);
  float total = 0;
  for (int i = -radius; i <= radius; ++i) {
    const float v = std::exp(-(static_cast<float>(i) * i) / (2.0f * sigma * sigma));
    kernel[static_cast<std::size_t>(i + radius)] = v;
    total += v;
  }
  for (float& k : kernel) k /= total;

  std::vector<float> alpha(static_cast<std::size_t>(w) * h, 0.0f);
  for (int y = 0; y < source.height(); ++y) {
    for (int x = 0; x < source.width(); ++x) {
      alpha[static_cast<std::size_t>(y + pad) * w + (x + pad)] =
          static_cast<float>(source.get(x, y).a) / 255.0f;
    }
  }

  std::vector<float> temp(alpha.size(), 0.0f);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float sum = 0;
      for (int i = -radius; i <= radius; ++i) {
        const int sx = std::min(w - 1, std::max(0, x + i));
        sum += alpha[static_cast<std::size_t>(y) * w + sx] *
               kernel[static_cast<std::size_t>(i + radius)];
      }
      temp[static_cast<std::size_t>(y) * w + x] = sum;
    }
  }
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      float sum = 0;
      for (int i = -radius; i <= radius; ++i) {
        const int sy = std::min(h - 1, std::max(0, y + i));
        sum += temp[static_cast<std::size_t>(sy) * w + x] *
               kernel[static_cast<std::size_t>(i + radius)];
      }
      alpha[static_cast<std::size_t>(y) * w + x] = sum;
    }
  }

  Image out(w, h, Rgba {0, 0, 0, 0});
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float a = alpha[static_cast<std::size_t>(y) * w + x] *
                      (static_cast<float>(color.a) / 255.0f);
      if (a <= 0.002f) continue;
      out.set(x, y, Rgba {color.r, color.g, color.b,
                          static_cast<std::uint8_t>(std::lround(std::min(1.0f, a) * 255.0f))});
    }
  }
  return out;
}

void compositeOver(Image& dst, const Image& src, int dx, int dy) {
  for (int y = 0; y < src.height(); ++y) {
    for (int x = 0; x < src.width(); ++x) {
      const Rgba s = src.get(x, y);
      if (s.a == 0) continue;
      const int tx = dx + x;
      const int ty = dy + y;
      if (!dst.inBounds(tx, ty)) continue;
      const Rgba d = dst.get(tx, ty);
      const float sa = static_cast<float>(s.a) / 255.0f;
      const float da = static_cast<float>(d.a) / 255.0f;
      const float oa = sa + da * (1.0f - sa);
      if (oa <= 0.0001f) {
        dst.set(tx, ty, Rgba {0, 0, 0, 0});
        continue;
      }
      const auto channel = [&](std::uint8_t sc, std::uint8_t dc) {
        const float v = (static_cast<float>(sc) * sa + static_cast<float>(dc) * da * (1.0f - sa)) /
                        oa;
        return static_cast<std::uint8_t>(std::lround(std::min(255.0f, std::max(0.0f, v))));
      };
      dst.set(tx, ty,
              Rgba {channel(s.r, d.r), channel(s.g, d.g), channel(s.b, d.b),
                    static_cast<std::uint8_t>(std::lround(oa * 255.0f))});
    }
  }
}

}  // namespace hr::ui
