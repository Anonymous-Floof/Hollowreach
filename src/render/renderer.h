// The deferred renderer, ported from js/render/renderer.js.
//
// Pass order, and why:
//   0  sun shadow map    depth-only from the sun, cutout-accurate
//   1  scene G-buffer    sky triangle, then opaque terrain (and later entities)
//   1b SSAO              its own pass so the result can be blurred
//   2  composite         fullscreen lighting; clouds march in the sky branch only
//   2b water             forward, translucent, with screen-space reflections
//   2c selection + held  drawn into the lit buffer
//   2d god rays          half resolution, into an aux target
//   3  present           upscale to the screen, add shafts, underwater post

#pragma once

#include <string>
#include <vector>

#include "core/camera.h"
#include "core/shader.h"
#include "render/entityrenderer.h"
#include "render/gbuffer.h"
#include "render/itemmesh.h"
#include "render/screenquad.h"
#include "render/sky.h"
#include "render/viewmodel.h"
#include "resource/atlas.h"
#include "world/world.h"

namespace hr::render {

// The knobs the graphics-quality preset and the individual effect toggles drive.
// Defaults match the web build's "High" preset.
struct QualitySettings {
  float scale = 1.0f;      // internal render scale
  int shadowSize = 2048;   // sun shadow map resolution, 0 = off
  float shadowRange = 72;  // world half-extent the shadow map covers
  float shadowBias = 0.12f;   // in WORLD units; small, because back-face depth avoids acne
  float shadowSlope = 2.5f;   // polygonOffset slope factor
  float shadowUnits = 4.0f;
  int shadowSteps = 0;     // point-light contact-shadow steps, 0 = off
  float shadowDist = 6.0f;
  float lightShadow = 0.0f;
  int maxLights = 16;
  int ssaoSamples = 16;
  float ssaoRadius = 1.0f;   // roughly one voxel
  float ssaoStrength = 1.15f;
  bool godrays = true;
  int godraySamples = 48;
  float godrayStrength = 0.55f;
  float godrayScale = 0.5f;
  int ssrSteps = 24;  // 0 = sky-only reflection, the cheapest
  float ssrStrength = 1.0f;
  int cloudSteps = 24;  // 0 = clouds off
  float cloudShadows = 1.0f;
  float cloudCover = 0.5f;
};

// Cap on dynamic coloured point lights, injected into the composite shader.
inline constexpr int kMaxLights = 16;

class Renderer {
 public:
  bool init(ShaderCache& shaders, const resource::Atlas* atlas);
  void dispose();

  void setQuality(const QualitySettings& q);
  const QualitySettings& quality() const { return quality_; }

  // 0 = off, 1 = show the AO term, 2 = show the sun shadow term.
  void setDebugView(int mode) { debug_ = mode; }
  int debugView() const { return debug_; }

  // `selection` is the block the crosshair is on, or null. `underwater` is the
  // 0..1 strength of the submerged post.
  struct Selection {
    int x = 0, y = 0, z = 0;
  };
  // `heldItem` is the selected hotbar item's key, or empty for an empty hand. It
  // drives both the first-person hand and, when it is an emitter block, the held
  // light in collectLights. The hand's pose and animation clocks live on
  // viewmodel(), which the game ticks.
  void render(world::World& world, const Camera& camera, const Sky& sky, int screenWidth,
              int screenHeight, const Selection* selection, const std::string& heldItem,
              float underwater, double time);

  // The live entities to draw, or null in a world without them. Set once by App;
  // the renderer only reads it, and only inside the two entity passes.
  void setEntities(const game::EntityManager* entities) { entities_ = entities; }
  EntityRenderer& entityRenderer() { return entityRenderer_; }

  // The animation state the game advances each frame.
  Viewmodel& viewmodel() { return viewmodel_; }

  // Shared with the entity renderer when it lands: dropped items use exactly the
  // same meshes as the held one.
  ItemMeshCache& itemMeshes() { return itemMeshes_; }

  // Chunks that passed the frustum test in the last frame, for the debug overlay.
  int drawnChunks() const { return drawnChunks_; }

 private:
  void drawHeld(const world::World& world, const Camera& camera, const Sky& sky,
                const std::string& itemKey, float aspect);
  void renderShadowMap(world::World& world, const Camera& camera, const Sky& sky, double time);
  // Wall-clock seconds since the renderer started, for the entity walk cycles.
  double animClock_ = 0.0;
  void collectLights(const world::World& world, const Vec3& camPos, world::BlockId heldBlock);
  GBuffer::Aux* renderSSAO(const Camera& camera, int screenW, int screenH);
  GBuffer::Aux* renderGodrays(const Camera& camera, const Sky& sky);
  void drawSelection(const Camera& camera, const Selection& sel);
  void ensureShadowFbo(int size);
  void ensureSsrDepth(int w, int h);
  void setCameraUniforms(Program& p, const Camera& camera, float aspect) const;

  const resource::Atlas* atlas_ = nullptr;
  GBuffer gbuffer_;
  ScreenQuad fullscreen_;

  Program* skyProg_ = nullptr;
  Program* terrainProg_ = nullptr;
  Program* shadowTerrainProg_ = nullptr;
  Program* compositeProg_ = nullptr;
  Program* ssaoProg_ = nullptr;
  Program* ssaoBlurProg_ = nullptr;
  Program* waterProg_ = nullptr;
  Program* godrayProg_ = nullptr;
  Program* presentProg_ = nullptr;
  Program* lineProg_ = nullptr;
  Program* viewmodelProg_ = nullptr;

  EntityRenderer entityRenderer_;
  const game::EntityManager* entities_ = nullptr;
  Viewmodel viewmodel_;
  ItemMeshCache itemMeshes_;

  QualitySettings quality_;
  int debug_ = 0;

  GLuint shadowTex_ = 0, shadowFbo_ = 0;
  int shadowSize_ = 0;
  bool shadowActive_ = false;
  float shadowDepthSpan_ = 0.0f;
  Mat4 lightVP_ = Mat4::identity();

  GLuint ssrDepthTex_ = 0, ssrDepthFbo_ = 0;
  int ssrDepthW_ = 0, ssrDepthH_ = 0;

  // A 1x1 white texture, bound as the AO source when SSAO is off so the composite
  // needs no branch.
  GLuint whiteTex_ = 0;

  GLuint lineVao_ = 0, lineVbo_ = 0;

  // Persistent point-light arrays, refilled per frame.
  std::vector<float> lightPos_, lightColor_, lightRad_;
  int lightCount_ = 0;
  struct LightCandidate {
    float d2;
    Vec3 pos;
    Vec3 color;
    float radius;
  };
  std::vector<LightCandidate> lightScratch_;

  std::vector<const world::LoadedChunk*> visible_;
  int drawnChunks_ = 0;
};

}  // namespace hr::render
