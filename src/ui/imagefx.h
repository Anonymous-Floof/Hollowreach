// The two CSS filters the interface cannot express as a shader primitive.
//
// `filter: drop-shadow(0 12px 22px rgba(0,0,0,0.6))` on the menu title is a real
// Gaussian blur of the alpha channel, and `background-clip: text` fills glyph coverage
// with a three-stop gradient. Both are applied once, at startup, to one fixed string,
// so they belong on the CPU rather than in the batcher.

#pragma once

#include <vector>

#include "resource/image.h"

namespace hr::ui {

// A CSS gradient stop: a position in 0..1 and a colour.
struct GradientStop {
  float position = 0;
  Rgba color {};
};

// Replaces every pixel's RGB with the gradient sampled at its vertical position,
// keeping alpha. This is exactly what `background-clip: text` does: the gradient is
// painted over the element box and clipped to the glyph coverage.
void applyVerticalGradient(Image& image, const std::vector<GradientStop>& stops);

// Separable Gaussian blur of the alpha channel only, with the RGB forced to `color`.
// `blurRadius` is the CSS blur radius, which is twice the Gaussian sigma.
Image dropShadowLayer(const Image& source, float blurRadius, Rgba color);

// Source-over composite of `src` onto `dst` at (dx, dy), in floating point with one
// rounding step — the same treatment the icon rasteriser needed, and for the same
// reason: truncating per layer accumulates a visible offset.
void compositeOver(Image& dst, const Image& src, int dx, int dy);

}  // namespace hr::ui
