// backdrop-filter, for the one element that uses it.
//
// `.menu-glass { backdrop-filter: blur(10px) saturate(118%) }` sits over a slowly rotating
// panorama, so the plan's judgement was that a downsample plus a two-pass blur refreshed
// once per frame is plenty rather than anything adaptive. That is exactly this: copy the
// finished frame to a quarter-size target, blur it there (which multiplies the effective
// radius by four for free), and hand back a texture the card samples through its own
// rounded-rect clip.

#pragma once

#include "core/gl.h"
#include "core/shader.h"
#include "render/screenquad.h"
#include "ui/ui2d.h"

namespace hr::ui {

class Backdrop {
 public:
  bool init(ShaderCache& shaders);
  void destroy();

  // Captures the default framebuffer and blurs it. Must be called while the finished
  // frame is still in the back buffer and before anything is drawn over it.
  // Returns false when the target could not be sized, in which case the caller should
  // fall back to drawing the card's plain translucent gradient.
  bool capture(int pixelWidth, int pixelHeight, float saturate = 1.18f);

  GLuint texture() const { return targets_[0]; }
  bool ready() const { return ready_; }

 private:
  bool resize(int pixelWidth, int pixelHeight);

  Program* program_ = nullptr;
  render::ScreenQuad quad_;
  // Two ping-pong targets at quarter resolution; [0] holds the final result.
  GLuint targets_[2] = {0, 0};
  GLuint fbos_[2] = {0, 0};
  GLuint copy_ = 0;
  GLuint copyFbo_ = 0;
  int width_ = 0, height_ = 0;
  bool ready_ = false;
};

}  // namespace hr::ui
