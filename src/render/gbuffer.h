// Off-screen render targets for the deferred pipeline, ported from js/render/gbuffer.js.
//
// The scene renders into a G-buffer rather than straight to the screen:
//   RT0 albedo : rgb albedo,          a = baked face shade times vertex AO
//   RT1 light  : r skylight, g blocklight, b emissive, a = material id
//   RT2 normal : rgb world-space normal, 0..1 encoded
//   depth      : DEPTH_COMPONENT24 texture, used to reconstruct world position
//
// A fullscreen composite then reads these and writes a lit colour target which
// SHARES the scene depth, so forward-blended water can still depth-test against
// the opaque scene. Everything is RGBA8 — no float render targets, matching the
// web build's constraint and keeping the whole chain LDR.

#pragma once

#include <vector>

#include "core/gl.h"

namespace hr::render {

// Material ids stored in light.a as id/255.
namespace mat {
inline constexpr int kOpaque = 0;
inline constexpr int kFoliage = 1;
inline constexpr int kEmissive = 2;
inline constexpr int kEntity = 3;
inline constexpr int kWater = 4;
inline constexpr int kSky = 255;
}  // namespace mat

class GBuffer {
 public:
  ~GBuffer() { dispose(); }

  // Internal buffers are the window size times the quality scale.
  void resize(int width, int height, float scale);
  void dispose();

  int bufW() const;
  int bufH() const;

  GLuint albedo() const { return albedo_; }
  GLuint light() const { return light_; }
  GLuint normal() const { return normal_; }
  GLuint depth() const { return depth_; }
  GLuint lit() const { return lit_; }
  GLuint sceneFbo() const { return sceneFbo_; }
  GLuint litColorFbo() const { return litColorFbo_; }

  void bindScene();
  void bindLit();       // lit colour plus the scene depth
  void bindLitColor();  // lit colour only, so the composite can sample the depth
  // The viewmodel target: the lit colour with a scratch depth buffer, cleared and
  // ready. The held item is drawn through its own close-up projection, so its depths
  // mean nothing against the scene's — but it still has to hide its own back faces.
  void bindHeld();

  // A scratch single-target colour FBO at (full * frac), created on demand and
  // cached by slot. Used by the god-ray, SSAO and reflection passes.
  struct Aux {
    GLuint fbo = 0;
    GLuint tex = 0;
    int w = 0, h = 0;
  };
  Aux& auxTarget(int slot, float frac, GLenum filter = GL_LINEAR);

 private:
  bool check(const char* label) const;
  void disposeCore();

  int w_ = 0, h_ = 0;
  float scale_ = 1.0f;

  GLuint albedo_ = 0, light_ = 0, normal_ = 0, depth_ = 0, lit_ = 0;
  GLuint heldDepth_ = 0;
  GLuint sceneFbo_ = 0, litFbo_ = 0, litColorFbo_ = 0, heldFbo_ = 0;
  std::vector<Aux> aux_;
};

}  // namespace hr::render
