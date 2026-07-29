#include "render/gbuffer.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"

namespace hr::render {
namespace {

GLuint makeColorTex(int w, int h, GLenum filter) {
  GLuint t = 0;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return t;
}

}  // namespace

int GBuffer::bufW() const { return std::max(1, static_cast<int>(std::lround(w_ * scale_))); }
int GBuffer::bufH() const { return std::max(1, static_cast<int>(std::lround(h_ * scale_))); }

bool GBuffer::check(const char* label) const {
  const GLenum s = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (s != GL_FRAMEBUFFER_COMPLETE) {
    log::error("gbuffer: %s framebuffer incomplete (0x%04X)", label, s);
    return false;
  }
  return true;
}

void GBuffer::resize(int width, int height, float scale) {
  if (width == w_ && height == h_ && scale == scale_ && sceneFbo_) return;
  w_ = width;
  h_ = height;
  scale_ = scale;
  disposeCore();

  const int bw = bufW(), bh = bufH();

  // --- scene: three colour targets plus a depth texture ---
  albedo_ = makeColorTex(bw, bh, GL_NEAREST);
  light_ = makeColorTex(bw, bh, GL_NEAREST);
  normal_ = makeColorTex(bw, bh, GL_NEAREST);

  glGenTextures(1, &depth_);
  glBindTexture(GL_TEXTURE_2D, depth_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, bw, bh, 0, GL_DEPTH_COMPONENT,
               GL_UNSIGNED_INT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenFramebuffers(1, &sceneFbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, albedo_, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 1, GL_TEXTURE_2D, light_, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + 2, GL_TEXTURE_2D, normal_, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_, 0);
  {
    const GLenum bufs[3] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT0 + 1,
                            GL_COLOR_ATTACHMENT0 + 2};
    glDrawBuffers(3, bufs);
  }
  check("scene");

  // --- lit colour. Two FBOs share the SAME colour texture: one carries the scene
  // depth, so forward water and the selection outline can depth-test against the
  // opaque scene; the other is depth-less, so the composite can sample the depth
  // texture without a read/write feedback loop on its own attachment. ---
  lit_ = makeColorTex(bw, bh, GL_LINEAR);

  glGenFramebuffers(1, &litFbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, litFbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lit_, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_, 0);
  {
    const GLenum bufs[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, bufs);
  }
  check("lit");

  glGenFramebuffers(1, &litColorFbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, litColorFbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lit_, 0);
  {
    const GLenum bufs[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, bufs);
  }
  check("litColor");

  // --- viewmodel: the lit colour again, but with a SCRATCH depth buffer we can
  // clear. That gives the held item a real depth test against itself while leaving
  // the scene depth — which the god-ray and underwater passes still read — intact.
  // Without any depth test a model draws in emission order and its back faces paint
  // over its front ones, which is what made a held block lose its top face. ---
  glGenRenderbuffers(1, &heldDepth_);
  glBindRenderbuffer(GL_RENDERBUFFER, heldDepth_);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, bw, bh);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);

  glGenFramebuffers(1, &heldFbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, heldFbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, lit_, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, heldDepth_);
  {
    const GLenum bufs[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, bufs);
  }
  check("held");

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  log::debug("gbuffer: %dx%d (scale %.2f)", bw, bh, scale_);
}

GBuffer::Aux& GBuffer::auxTarget(int slot, float frac, GLenum filter) {
  if (static_cast<int>(aux_.size()) <= slot) aux_.resize(slot + 1);
  Aux& a = aux_[slot];

  const int w = std::max(1, static_cast<int>(std::lround(bufW() * frac)));
  const int h = std::max(1, static_cast<int>(std::lround(bufH() * frac)));
  if (a.fbo && a.w == w && a.h == h) return a;

  if (a.fbo) {
    glDeleteFramebuffers(1, &a.fbo);
    glDeleteTextures(1, &a.tex);
  }
  a.tex = makeColorTex(w, h, filter);
  glGenFramebuffers(1, &a.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, a.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, a.tex, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  a.w = w;
  a.h = h;
  return a;
}

void GBuffer::bindScene() {
  glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo_);
  glViewport(0, 0, bufW(), bufH());
}

void GBuffer::bindLit() {
  glBindFramebuffer(GL_FRAMEBUFFER, litFbo_);
  glViewport(0, 0, bufW(), bufH());
}

void GBuffer::bindLitColor() {
  glBindFramebuffer(GL_FRAMEBUFFER, litColorFbo_);
  glViewport(0, 0, bufW(), bufH());
}

void GBuffer::bindHeld() {
  glBindFramebuffer(GL_FRAMEBUFFER, heldFbo_);
  glViewport(0, 0, bufW(), bufH());
  // glClear respects the depth write mask, so it has to be enabled first.
  glDepthMask(GL_TRUE);
  glClear(GL_DEPTH_BUFFER_BIT);
}

void GBuffer::disposeCore() {
  for (GLuint* t : {&albedo_, &light_, &normal_, &depth_, &lit_}) {
    if (*t) {
      glDeleteTextures(1, t);
      *t = 0;
    }
  }
  if (heldDepth_) {
    glDeleteRenderbuffers(1, &heldDepth_);
    heldDepth_ = 0;
  }
  for (GLuint* f : {&sceneFbo_, &litFbo_, &litColorFbo_, &heldFbo_}) {
    if (*f) {
      glDeleteFramebuffers(1, f);
      *f = 0;
    }
  }
}

void GBuffer::dispose() {
  disposeCore();
  for (Aux& a : aux_) {
    if (a.fbo) {
      glDeleteFramebuffers(1, &a.fbo);
      glDeleteTextures(1, &a.tex);
    }
  }
  aux_.clear();
}

}  // namespace hr::render
