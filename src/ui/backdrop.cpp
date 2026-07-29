#include "ui/backdrop.h"

#include <algorithm>

#include "core/log.h"

namespace hr::ui {
namespace {

// Quarter resolution: the blur radius the CSS asks for is 10px, and at 1/4 scale a
// sigma-2.2 kernel covers it.
constexpr int kDownscale = 4;

GLuint makeTarget(int w, int h) {
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  return texture;
}

}  // namespace

bool Backdrop::init(ShaderCache& shaders) {
  program_ = shaders.load({
      .name = "uiBackdrop",
      .vertAsset = "shaders/fullscreen.vert",
      .fragAsset = "shaders/ui_backdrop.frag",
      .defines = {},
      .attribs = {"aPos"},
  });
  if (!program_) return false;
  quad_.create();
  return true;
}

void Backdrop::destroy() {
  glDeleteTextures(2, targets_);
  glDeleteFramebuffers(2, fbos_);
  if (copy_) glDeleteTextures(1, &copy_);
  if (copyFbo_) glDeleteFramebuffers(1, &copyFbo_);
  targets_[0] = targets_[1] = fbos_[0] = fbos_[1] = copy_ = copyFbo_ = 0;
  quad_.destroy();
  width_ = height_ = 0;
  ready_ = false;
}

bool Backdrop::resize(int pixelWidth, int pixelHeight) {
  const int w = std::max(1, pixelWidth / kDownscale);
  const int h = std::max(1, pixelHeight / kDownscale);
  if (w == width_ && h == height_ && targets_[0] != 0) return true;

  glDeleteTextures(2, targets_);
  glDeleteFramebuffers(2, fbos_);
  if (copy_) glDeleteTextures(1, &copy_);
  if (copyFbo_) glDeleteFramebuffers(1, &copyFbo_);

  width_ = w;
  height_ = h;
  for (int i = 0; i < 2; ++i) {
    targets_[i] = makeTarget(w, h);
    glGenFramebuffers(1, &fbos_[i]);
    glBindFramebuffer(GL_FRAMEBUFFER, fbos_[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targets_[i], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      log::warn("backdrop target %d incomplete; the glass card will not blur", i);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      return false;
    }
  }
  // A full-resolution copy the downscale samples from, because glReadPixels into a
  // texture is not a thing and glCopyTexSubImage2D cannot scale.
  copy_ = makeTarget(pixelWidth, pixelHeight);
  glGenFramebuffers(1, &copyFbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, copyFbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, copy_, 0);
  const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return complete;
}

bool Backdrop::capture(int pixelWidth, int pixelHeight, float saturate) {
  ready_ = false;
  if (pixelWidth <= 0 || pixelHeight <= 0) return false;
  if (!program_ || !program_->valid()) return false;
  if (!resize(pixelWidth, pixelHeight)) return false;

  // The finished frame is in the back buffer; copy it into a texture so it can be
  // sampled with filtering.
  glBindTexture(GL_TEXTURE_2D, copy_);
  glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, pixelWidth, pixelHeight);
  glBindTexture(GL_TEXTURE_2D, 0);

  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glViewport(0, 0, width_, height_);

  program_->use();
  program_->set("uSource", 0);
  glActiveTexture(GL_TEXTURE0);

  // Horizontal, from the full-resolution copy straight into the quarter-size target —
  // the bilinear minification is the downsample.
  glBindFramebuffer(GL_FRAMEBUFFER, fbos_[1]);
  glBindTexture(GL_TEXTURE_2D, copy_);
  program_->set("uTexelSize", 1.0f / static_cast<float>(pixelWidth),
                1.0f / static_cast<float>(pixelHeight));
  program_->set("uDirection", 1.0f, 0.0f);
  program_->set("uSaturate", 1.0f);
  quad_.draw();

  // Vertical, plus the saturation, into the target the card will sample.
  glBindFramebuffer(GL_FRAMEBUFFER, fbos_[0]);
  glBindTexture(GL_TEXTURE_2D, targets_[1]);
  program_->set("uTexelSize", 1.0f / static_cast<float>(width_),
                1.0f / static_cast<float>(height_));
  program_->set("uDirection", 0.0f, 1.0f);
  program_->set("uSaturate", saturate);
  quad_.draw();

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, pixelWidth, pixelHeight);
  glBindTexture(GL_TEXTURE_2D, 0);
  ready_ = true;
  return true;
}

}  // namespace hr::ui
