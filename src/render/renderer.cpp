#include "render/renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/jsmath.h"
#include "core/log.h"

namespace hr::render {
namespace {

// Monotonic seconds since the first call. The entity walk cycles need a clock that
// is independent of the sky, which --time can pin.
double wallSeconds() {
  using clock = std::chrono::steady_clock;
  static const clock::time_point start = clock::now();
  return std::chrono::duration<double>(clock::now() - start).count();
}

constexpr float kPi = 3.14159265358979323846f;

// Distance in blocks at which lights stop being considered at all.
constexpr float kLightRange = 28.0f;

}  // namespace

bool Renderer::init(ShaderCache& shaders, const resource::Atlas* atlas) {
  atlas_ = atlas;
  fullscreen_.create();

  const ShaderDefines lightDefines = {{"MAX_LIGHTS", std::to_string(kMaxLights)}};

  skyProg_ = shaders.load({.name = "gbufferSky",
                           .vertAsset = "shaders/gbuffer_sky.vert",
                           .fragAsset = "shaders/gbuffer_sky.frag",
                           .defines = {},
                           .attribs = {"aPos"}});
  terrainProg_ = shaders.load({.name = "gbufferTerrain",
                               .vertAsset = "shaders/gbuffer_terrain.vert",
                               .fragAsset = "shaders/gbuffer_terrain.frag",
                               .defines = {},
                               .attribs = {"aPos", "aUV", "aPack", "aTint"}});
  shadowTerrainProg_ = shaders.load({.name = "shadowTerrain",
                                     .vertAsset = "shaders/shadow_terrain.vert",
                                     .fragAsset = "shaders/shadow_terrain.frag",
                                     .defines = {},
                                     .attribs = {"aPos", "aUV", "aPack", "aTint"}});
  if (!entityRenderer_.init(shaders, atlas, &itemMeshes_)) return false;
  compositeProg_ = shaders.load({.name = "composite",
                                 .vertAsset = "shaders/fullscreen.vert",
                                 .fragAsset = "shaders/composite.frag",
                                 .defines = lightDefines,
                                 .attribs = {"aPos"}});
  ssaoProg_ = shaders.load({.name = "ssao",
                            .vertAsset = "shaders/fullscreen.vert",
                            .fragAsset = "shaders/ssao.frag",
                            .defines = {},
                            .attribs = {"aPos"}});
  ssaoBlurProg_ = shaders.load({.name = "ssaoBlur",
                                .vertAsset = "shaders/fullscreen.vert",
                                .fragAsset = "shaders/ssao_blur.frag",
                                .defines = {},
                                .attribs = {"aPos"}});
  waterProg_ = shaders.load({.name = "water",
                             .vertAsset = "shaders/water.vert",
                             .fragAsset = "shaders/water.frag",
                             .defines = {},
                             .attribs = {"aPos", "aUV", "aPack", "aTint"}});
  godrayProg_ = shaders.load({.name = "godray",
                              .vertAsset = "shaders/fullscreen.vert",
                              .fragAsset = "shaders/godray.frag",
                              .defines = {},
                              .attribs = {"aPos"}});
  presentProg_ = shaders.load({.name = "present",
                               .vertAsset = "shaders/fullscreen.vert",
                               .fragAsset = "shaders/present.frag",
                               .defines = {},
                               .attribs = {"aPos"}});
  lineProg_ = shaders.load({.name = "line",
                            .vertAsset = "shaders/line.vert",
                            .fragAsset = "shaders/line.frag",
                            .defines = {},
                            .attribs = {"aPos"}});
  viewmodelProg_ = shaders.load({.name = "viewmodel",
                                 .vertAsset = "shaders/viewmodel.vert",
                                 .fragAsset = "shaders/viewmodel.frag",
                                 .defines = {},
                                 .attribs = {"aPos", "aUV", "aShade", "aBone", "aColor"}});

  itemMeshes_.setAtlas(atlas);

  if (!skyProg_ || !terrainProg_ || !shadowTerrainProg_ || !compositeProg_ || !ssaoProg_ ||
      !ssaoBlurProg_ || !waterProg_ || !godrayProg_ || !presentProg_ || !lineProg_ ||
      !viewmodelProg_) {
    log::error("renderer: one or more shader programs failed to build");
    return false;
  }

  // The "no occlusion" source, so the composite can sample uSSAO unconditionally.
  const unsigned char white[4] = {255, 255, 255, 255};
  glGenTextures(1, &whiteTex_);
  glBindTexture(GL_TEXTURE_2D, whiteTex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  // Dynamic buffer for the selection outline: 12 edges, 2 vertices, 3 floats.
  glGenVertexArrays(1, &lineVao_);
  glGenBuffers(1, &lineVbo_);
  glBindVertexArray(lineVao_);
  glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
  glBufferData(GL_ARRAY_BUFFER, 72 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
  glBindVertexArray(0);

  lightPos_.assign(kMaxLights * 3, 0.0f);
  lightColor_.assign(kMaxLights * 3, 0.0f);
  lightRad_.assign(kMaxLights, 0.0f);

  ensureShadowFbo(quality_.shadowSize);
  // No-op unless --perf set the flag before init.
  profiler_.init();
  return true;
}

void Renderer::dispose() {
  profiler_.dispose();
  entityRenderer_.dispose();
  gbuffer_.dispose();
  fullscreen_.destroy();
  itemMeshes_.clear();
  if (whiteTex_) glDeleteTextures(1, &whiteTex_);
  if (shadowTex_) glDeleteTextures(1, &shadowTex_);
  if (shadowFbo_) glDeleteFramebuffers(1, &shadowFbo_);
  if (ssrDepthTex_) glDeleteTextures(1, &ssrDepthTex_);
  if (ssrDepthFbo_) glDeleteFramebuffers(1, &ssrDepthFbo_);
  if (lineVbo_) glDeleteBuffers(1, &lineVbo_);
  if (lineVao_) glDeleteVertexArrays(1, &lineVao_);
  whiteTex_ = shadowTex_ = shadowFbo_ = ssrDepthTex_ = ssrDepthFbo_ = 0;
  lineVbo_ = lineVao_ = 0;
}

void Renderer::setQuality(const QualitySettings& q) {
  const int previousShadow = quality_.shadowSize;
  quality_ = q;
  if (quality_.shadowSize != previousShadow) ensureShadowFbo(quality_.shadowSize);
}

void Renderer::ensureShadowFbo(int size) {
  if (shadowTex_) {
    glDeleteTextures(1, &shadowTex_);
    shadowTex_ = 0;
  }
  if (shadowFbo_) {
    glDeleteFramebuffers(1, &shadowFbo_);
    shadowFbo_ = 0;
  }
  shadowSize_ = size;
  if (!size) return;

  glGenTextures(1, &shadowTex_);
  glBindTexture(GL_TEXTURE_2D, shadowTex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size, size, 0, GL_DEPTH_COMPONENT,
               GL_UNSIGNED_INT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  // Sampling outside the map is handled by the shader's uv range check, so plain
  // clamp is fine.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenFramebuffers(1, &shadowFbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowTex_, 0);
  const GLenum none = GL_NONE;
  glDrawBuffers(1, &none);
  glReadBuffer(GL_NONE);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::ensureSsrDepth(int w, int h) {
  if (ssrDepthW_ == w && ssrDepthH_ == h && ssrDepthTex_) return;
  if (ssrDepthTex_) glDeleteTextures(1, &ssrDepthTex_);
  if (ssrDepthFbo_) glDeleteFramebuffers(1, &ssrDepthFbo_);
  ssrDepthW_ = w;
  ssrDepthH_ = h;

  glGenTextures(1, &ssrDepthTex_);
  glBindTexture(GL_TEXTURE_2D, ssrDepthTex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT,
               GL_UNSIGNED_INT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenFramebuffers(1, &ssrDepthFbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, ssrDepthFbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, ssrDepthTex_, 0);
  const GLenum none = GL_NONE;
  glDrawBuffers(1, &none);
  glReadBuffer(GL_NONE);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::setCameraUniforms(Program& p, const Camera& camera, float aspect) const {
  const Vec3 pos = camera.pos();
  const Vec3 right = camera.right();
  const Vec3 up = camera.up();
  const Vec3 fwd = camera.forward();
  p.set("uCamPos", pos.x, pos.y, pos.z);
  p.set("uCamRight", right.x, right.y, right.z);
  p.set("uCamUp", up.x, up.y, up.z);
  p.set("uCamFwd", fwd.x, fwd.y, fwd.z);
  p.set("uTanHalf", camera.tanHalfFov());
  p.set("uAspect", aspect);
  p.set("uNear", camera.near());
  p.set("uFar", camera.far());
}

void Renderer::renderShadowMap(world::World& world, const Camera& camera, const Sky& sky,
                               double time) {
  shadowActive_ = shadowSize_ > 0 && sky.sunHeight() > 0.08f;
  if (!shadowActive_) return;

  const Celestial cel = sky.celestial();
  const Vec3 L = cel.dir;
  const float range = quality_.shadowRange;
  const float dist = range * 1.5f;
  const Vec3 up = std::abs(L.y) > 0.99f ? Vec3 {0, 0, 1} : Vec3 {0, 1, 0};

  // Quantise the centre to whole blocks first, so head bob — a few centimetres of
  // sub-texel wobble — cannot nudge the texel snap across a boundary and shimmer
  // every shadow edge every frame.
  // jsRoundF throughout the shadow snap: camera and light-space coordinates are
  // routinely negative, and std::round's away-from-zero half would shift the snap by a
  // whole texel on one side of the origin only.
  const Vec3 cp {jsmath::jsRoundF(camera.pos().x), jsmath::jsRoundF(camera.pos().y),
                 jsmath::jsRoundF(camera.pos().z)};

  // Centre the shadow box on the camera, but SNAP that centre to the map's texel
  // grid IN LIGHT SPACE, along the light's right and up axes — not the world axes.
  // World-axis snapping leaves the texel grid sliding under the world as the camera
  // moves, which makes shadow edges crawl when you strafe. Light-space snapping pins
  // each texel to a fixed world spot. The basis matches Mat4::lookAt's internal one.
  const Vec3 f {-L.x, -L.y, -L.z};
  Vec3 s {f.y * up.z - f.z * up.y, f.z * up.x - f.x * up.z, f.x * up.y - f.y * up.x};
  s = s.normalized();
  const Vec3 u {s.y * f.z - s.z * f.y, s.z * f.x - s.x * f.z, s.x * f.y - s.y * f.x};

  const float texel = (2.0f * range) / shadowSize_;
  const float lx = cp.x * s.x + cp.y * s.y + cp.z * s.z;
  const float ly = cp.x * u.x + cp.y * u.y + cp.z * u.z;
  const float dlx = jsmath::jsRoundF(lx / texel) * texel - lx;
  const float dly = jsmath::jsRoundF(ly / texel) * texel - ly;
  const Vec3 centre {cp.x + dlx * s.x + dly * u.x, cp.y + dlx * s.y + dly * u.y,
                     cp.z + dlx * s.z + dly * u.z};

  const Vec3 eye {centre.x + L.x * dist, centre.y + L.y * dist, centre.z + L.z * dist};
  const Mat4 lightView = Mat4::lookAt(eye, centre, up);
  // A tight near/far around the scene slab gives good depth precision, which is what
  // lets the bias stay small.
  const float nearP = std::max(0.5f, dist - range - 3.0f);
  const float farP = dist + range + 3.0f;
  shadowDepthSpan_ = farP - nearP;
  lightVP_ = Mat4::ortho(-range, range, -range, range, nearP, farP) * lightView;

  glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
  glViewport(0, 0, shadowSize_, shadowSize_);
  // depthMask MUST be true before the clear: glClear respects the depth write mask,
  // and the present pass leaves it false. With the clear masked out the map
  // accumulates the minimum depth ever rendered per texel, and because the light box
  // is camera-centred that stale content is glued to the camera — shadows then appear
  // to strafe along with the player.
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_TRUE);
  glClear(GL_DEPTH_BUFFER_BIT);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);

  if (quality_.shadowSlope > 0) {
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(quality_.shadowSlope, quality_.shadowUnits);
  } else {
    glDisable(GL_POLYGON_OFFSET_FILL);
  }

  shadowTerrainProg_->use();
  shadowTerrainProg_->setMat4("uLightVP", lightVP_.data());
  shadowTerrainProg_->set("uTime", static_cast<float>(time));
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, atlas_->texture());
  shadowTerrainProg_->set("uAtlas", 0);

  // Light-space extent cull: the ortho box only spans +/-range along the light's
  // right and up axes around the snapped centre, so a chunk whose AABB projection
  // lies wholly outside it cannot reach a single shadow texel. Depth along the light
  // direction is irrelevant to an ortho footprint. The pad is the chunk AABB's
  // projected half-extent — conservative, so this culls only chunks that provably
  // contribute nothing and the map output is unchanged.
  const float cull = range + world::CX * 2.0f;
  const float cull2 = cull * cull;
  // Half a SECTION, not half the world. This is what broke when WH went 128 -> 192:
  // the test modelled every chunk as a box centred at WH/2 with a half-height of
  // WH/2, so the pad grew by |s.y| * 32 on each light axis and, with the sun low,
  // stopped rejecting anything. The shadow pass was redrawing nearly every loaded
  // chunk's full column, twice the geometry of the g-buffer pass, at a cost that
  // does not care what resolution you run — which is exactly the symptom.
  const float hy = world::kSectionHeight * 0.5f;
  // One block of slack, because the shadow vertex shader sways leaves past the AABB.
  const float padS =
      range + 1 + std::abs(s.x) * 8 + std::abs(s.y) * hy + std::abs(s.z) * 8;
  const float padU =
      range + 1 + std::abs(u.x) * 8 + std::abs(u.y) * hy + std::abs(u.z) * 8;

  shadowTris_ = 0;
  for (const auto& [key, lc] : world.chunks()) {
    if (lc->opaqueMesh.empty()) continue;
    const float dx = lc->chunk.cx * world::CX + world::CX * 0.5f - camera.pos().x;
    const float dz = lc->chunk.cz * world::CZ + world::CZ * 0.5f - camera.pos().z;
    if (dx * dx + dz * dz > cull2) continue;
    const float mx = lc->chunk.cx * world::CX + 8 - centre.x;
    const float mz = lc->chunk.cz * world::CZ + 8 - centre.z;
    unsigned mask = 0;
    for (int i = 0; i < world::kSections; ++i) {
      if (lc->opaqueMesh.sectionCount(i) == 0) continue;
      // The sun cannot see a face with no skylight on it, so it cannot be
      // shadowed by one either. This is what stops a hundred blocks of cave
      // wall rasterising into an alpha-tested depth map every single frame.
      if (!lc->opaqueMesh.sectionSunlit(i)) continue;
      const float my = (i + 0.5f) * world::kSectionHeight - centre.y;
      if (std::abs(mx * s.x + my * s.y + mz * s.z) > padS) continue;
      if (std::abs(mx * u.x + my * u.y + mz * u.z) > padU) continue;
      shadowTris_ += lc->opaqueMesh.sectionCount(i) / 3;
      mask |= 1u << i;
    }
    lc->opaqueMesh.drawSectionMask(mask);
  }

  // Mobs, boats and drops cast shadows too. Drawn after the terrain so they share
  // the same polygon offset, and before it is cleared below.
  if (entities_) entityRenderer_.drawShadow(world, *entities_, lightVP_.data(), animClock_);

  // The shadow pass's polygon offset must not leak into the G-buffer pass: it would
  // skew every depth-based reconstruction — shadows, SSAO, fog — view-dependently.
  glDisable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(0, 0);
}

void Renderer::collectLights(const world::World& world, const Vec3& camPos,
                             world::BlockId heldBlock) {
  const int cap = std::min(kMaxLights, quality_.maxLights);
  lightScratch_.clear();
  const world::BlockRegistry& reg = world::blocks();

  // A held emitter glows from the camera.
  if (heldBlock != world::kAir) {
    const int e = reg.emit(heldBlock);
    if (e > 0) {
      const world::Rgb c = reg.def(heldBlock).lightColor;
      const float i = std::min(1.0f, e / 14.0f);
      lightScratch_.push_back(
          {-1.0f, camPos, {c.r * i, c.g * i, c.b * i}, e * 0.9f + 3.0f});
    }
  }

  const float r2 = kLightRange * kLightRange;
  for (const auto& [key, lc] : world.chunks()) {
    if (lc->chunk.emitters.empty()) continue;
    // Cheap chunk-level reject before touching individual emitters.
    const float mx = lc->chunk.cx * world::CX + world::CX * 0.5f;
    const float mz = lc->chunk.cz * world::CZ + world::CZ * 0.5f;
    const float cdx = mx - camPos.x, cdz = mz - camPos.z;
    const float reach = kLightRange + world::CX;
    if (cdx * cdx + cdz * cdz > reach * reach) continue;

    for (const world::Emitter& em : lc->chunk.emitters) {
      const float dx = em.x - camPos.x, dy = em.y - camPos.y, dz = em.z - camPos.z;
      const float d2 = dx * dx + dy * dy + dz * dz;
      if (d2 > r2) continue;
      const int e = reg.emit(em.id);
      const world::Rgb c = reg.def(em.id).lightColor;
      const float i = std::min(1.0f, e / 14.0f);
      lightScratch_.push_back(
          {d2, {em.x, em.y, em.z}, {c.r * i, c.g * i, c.b * i}, e * 0.9f + 2.0f});
    }
  }

  std::sort(lightScratch_.begin(), lightScratch_.end(),
            [](const LightCandidate& a, const LightCandidate& b) { return a.d2 < b.d2; });

  lightCount_ = std::min<int>(cap, static_cast<int>(lightScratch_.size()));
  for (int k = 0; k < lightCount_; ++k) {
    const LightCandidate& L = lightScratch_[k];
    lightPos_[k * 3 + 0] = L.pos.x;
    lightPos_[k * 3 + 1] = L.pos.y;
    lightPos_[k * 3 + 2] = L.pos.z;
    // The colour already carries intensity, so the shader just adds it.
    lightColor_[k * 3 + 0] = L.color.x;
    lightColor_[k * 3 + 1] = L.color.y;
    lightColor_[k * 3 + 2] = L.color.z;
    lightRad_[k] = L.radius;
  }
}

GBuffer::Aux* Renderer::renderSSAO(const Camera& camera, int screenW, int screenH) {
  if (quality_.ssaoSamples <= 0) return nullptr;

  GBuffer::Aux& raw = gbuffer_.auxTarget(1, 1.0f, GL_LINEAR);
  GBuffer::Aux& blur = gbuffer_.auxTarget(2, 1.0f, GL_LINEAR);
  const float aspect = static_cast<float>(screenW) / std::max(1, screenH);

  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_BLEND);

  glBindFramebuffer(GL_FRAMEBUFFER, raw.fbo);
  glViewport(0, 0, raw.w, raw.h);
  ssaoProg_->use();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gbuffer_.depth());
  ssaoProg_->set("uDepth", 0);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, gbuffer_.normal());
  ssaoProg_->set("uNormal", 1);
  ssaoProg_->setMat4("uViewProj", camera.viewProj().data());
  setCameraUniforms(*ssaoProg_, camera, aspect);
  ssaoProg_->set("uSamples", quality_.ssaoSamples);
  ssaoProg_->set("uRadius", quality_.ssaoRadius);
  ssaoProg_->set("uStrength", quality_.ssaoStrength);
  fullscreen_.draw();

  glBindFramebuffer(GL_FRAMEBUFFER, blur.fbo);
  glViewport(0, 0, blur.w, blur.h);
  ssaoBlurProg_->use();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, raw.tex);
  ssaoBlurProg_->set("uSSAO", 0);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, gbuffer_.depth());
  ssaoBlurProg_->set("uDepth", 1);
  ssaoBlurProg_->set("uTexel", 1.0f / blur.w, 1.0f / blur.h);
  ssaoBlurProg_->set("uNear", camera.near());
  ssaoBlurProg_->set("uFar", camera.far());
  fullscreen_.draw();

  return &blur;
}

GBuffer::Aux* Renderer::renderGodrays(const Camera& camera, const Sky& sky) {
  if (!quality_.godrays || quality_.godraySamples <= 0) return nullptr;
  if (sky.sunHeight() <= -0.02f) return nullptr;  // sun below the horizon

  // Project a far point along the sun direction through the camera view-projection.
  const Vec3 sun = sky.sunDir();
  const float x = camera.pos().x + sun.x * 1000.0f;
  const float y = camera.pos().y + sun.y * 1000.0f;
  const float z = camera.pos().z + sun.z * 1000.0f;
  const Mat4& vp = camera.viewProj();
  const float cw = vp[3] * x + vp[7] * y + vp[11] * z + vp[15];
  if (cw <= 0) return nullptr;  // sun behind the camera

  const float ndcX = (vp[0] * x + vp[4] * y + vp[8] * z + vp[12]) / cw;
  const float ndcY = (vp[1] * x + vp[5] * y + vp[9] * z + vp[13]) / cw;

  // Fade out as the sun moves off screen — allowing overscan so shafts still enter
  // from just past the edge — as it nears the horizon, and at night.
  const float edge = std::max(std::abs(ndcX), std::abs(ndcY));
  const float fade = 1.0f - std::clamp((edge - 0.95f) / (1.6f - 0.95f), 0.0f, 1.0f);
  const float hgt = std::clamp((sky.sunHeight() + 0.02f) / 0.17f, 0.0f, 1.0f);
  const float valid = fade * hgt * sky.dayFactor();
  if (valid <= 0.001f) return nullptr;

  GBuffer::Aux& a = gbuffer_.auxTarget(0, quality_.godrayScale, GL_LINEAR);
  glBindFramebuffer(GL_FRAMEBUFFER, a.fbo);
  glViewport(0, 0, a.w, a.h);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_BLEND);

  godrayProg_->use();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gbuffer_.depth());
  godrayProg_->set("uDepth", 0);
  godrayProg_->set("uSunScreen", ndcX * 0.5f + 0.5f, ndcY * 0.5f + 0.5f);
  const Celestial cel = sky.celestial();
  godrayProg_->set("uSunColor", cel.color.x, cel.color.y, cel.color.z);
  godrayProg_->set("uGodrayValid", valid);
  godrayProg_->set("uGodrayStrength", quality_.godrayStrength);
  godrayProg_->set("uGodraySamples", quality_.godraySamples);
  fullscreen_.draw();
  return &a;
}

void Renderer::drawSelection(const Camera& camera, const Selection& sel) {
  // A hair larger than the block, so the outline is not z-fighting its faces.
  const float e = 0.002f;
  const float x0 = sel.x - e, y0 = sel.y - e, z0 = sel.z - e;
  const float x1 = sel.x + 1 + e, y1 = sel.y + 1 + e, z1 = sel.z + 1 + e;

  float a[72];
  int i = 0;
  auto P = [&](float px, float py, float pz) {
    a[i++] = px;
    a[i++] = py;
    a[i++] = pz;
  };
  // bottom ring
  P(x0, y0, z0); P(x1, y0, z0);  P(x1, y0, z0); P(x1, y0, z1);
  P(x1, y0, z1); P(x0, y0, z1);  P(x0, y0, z1); P(x0, y0, z0);
  // top ring
  P(x0, y1, z0); P(x1, y1, z0);  P(x1, y1, z0); P(x1, y1, z1);
  P(x1, y1, z1); P(x0, y1, z1);  P(x0, y1, z1); P(x0, y1, z0);
  // verticals
  P(x0, y0, z0); P(x0, y1, z0);  P(x1, y0, z0); P(x1, y1, z0);
  P(x1, y0, z1); P(x1, y1, z1);  P(x0, y0, z1); P(x0, y1, z1);

  lineProg_->use();
  lineProg_->setMat4("uViewProj", camera.viewProj().data());
  lineProg_->set("uColor", 0.0f, 0.0f, 0.0f, 0.5f);
  glBindVertexArray(lineVao_);
  glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(a), a);
  glDrawArrays(GL_LINES, 0, 24);
  glBindVertexArray(0);
}

// The first-person hand, drawn last into the lit buffer through its own close-up
// projection so a held item never clips into the world.
//
// It gets a scratch depth buffer of its own (GBuffer::bindHeld): the scene's depth
// is in a different projection and is still needed by later passes, but without
// SOME depth test a model just draws in emission order and its back faces paint
// over its front ones — the old "held block has no top face".
void Renderer::drawHeld(const world::World& world, const Camera& camera, const Sky& sky,
                        const std::string& itemKey, float aspect) {
  const ItemMesh* mesh = itemMeshes_.get(itemKey);
  if (!mesh) return;

  const ItemModel model = itemModelFor(itemKey);
  const float fovRadians = camera.fov() * kPi / 180.0f;
  const Mat4 held = viewmodel_.modelMatrix(model.hold, fovRadians, aspect);
  const Mat4 mvp = Mat4::perspective(fovRadians, aspect, 0.01f, 10.0f) * held;

  // Light the item by where the player is standing, so it dims at night and goes
  // near-black in an unlit cave. Skylight keeps a moonlight floor, the same idea as
  // the terrain's night ambient — a hand outdoors at midnight is dim, not invisible.
  const int cx = static_cast<int>(std::floor(camera.pos().x));
  const int cy = static_cast<int>(std::floor(camera.pos().y));
  const int cz = static_cast<int>(std::floor(camera.pos().z));
  const float light =
      std::max(world.getBlockLight(cx, cy, cz) / 15.0f,
               (world.getSky(cx, cy, cz) / 15.0f) * std::max(0.10f, sky.dayFactor()));

  gbuffer_.bindHeld();
  viewmodelProg_->use();
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glDisable(GL_CULL_FACE);  // mirrored hold styles reverse the winding
  glDisable(GL_BLEND);
  viewmodelProg_->setMat4("uMVP", mvp.data());
  viewmodelProg_->set("uLight", light);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, atlas_->texture());
  viewmodelProg_->set("uAtlas", 0);
  glBindVertexArray(mesh->vao);
  glDrawArrays(GL_TRIANGLES, 0, mesh->count);
  glBindVertexArray(0);
  gbuffer_.bindLit();  // leave the scene target bound as found
}

void Renderer::render(world::World& world, const Camera& camera, const Sky& sky,
                      int screenWidth, int screenHeight, const Selection* selection,
                      const std::string& heldItem, float underwater, double time) {
  const float cloudBase =
      static_cast<float>(world::seaLevel(world.genVersion())) + 72.0f;

  const float t = static_cast<float>(time);
  const float aspect = static_cast<float>(screenWidth) / std::max(1, screenHeight);
  gbuffer_.resize(screenWidth, screenHeight, quality_.scale);

  // Rotates the query ring and harvests results from four frames ago. Must come
  // before any pass opens a query.
  profiler_.beginFrame();

  // Entity walk cycles integrate against a real-time clock rather than the sky
  // clock, because the sky can be paused (--time) or run at any rate and a frozen
  // sky must not freeze a sheep mid-stride.
  animClock_ = wallSeconds();

  const Vec3 fog = sky.fogColor();
  const Vec3 hz = sky.horizon();
  const Vec3 zn = sky.zenith();
  const Vec3 sun = sky.sunDir();
  const Celestial cel = sky.celestial();

  // Fog reaches full strength just inside the loaded area, so chunks resolve out of
  // haze rather than popping in. Dawn thickens it markedly.
  const float far = (world.renderDistance() - 1.2f) * world::CX;
  const float mf = sky.morningFog();
  const float fogNear = std::max(16.0f, far * 0.55f) * (1.0f - 0.62f * mf);
  const float fogFar = std::max(40.0f, far) * (1.0f - 0.42f * mf);

  // ===================== Pass 0: sun shadow map =====================
  {
    GpuScope gs(profiler_, GpuPass::Shadow);
    renderShadowMap(world, camera, sky, time);
  }

  // ===================== Pass 1: scene G-buffer =====================
  gbuffer_.bindScene();
  glClearColor(0, 0, 0, 1);
  glDepthMask(GL_TRUE);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  // Sky fills the background with no depth, so any geometry overwrites it.
  profiler_.begin(GpuPass::Sky);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  skyProg_->use();
  skyProg_->set("uHorizon", hz.x, hz.y, hz.z);
  skyProg_->set("uZenith", zn.x, zn.y, zn.z);
  const Vec3 camRight = camera.right(), camUp = camera.up(), camFwd = camera.forward();
  skyProg_->set("uRight", camRight.x, camRight.y, camRight.z);
  skyProg_->set("uUp", camUp.x, camUp.y, camUp.z);
  skyProg_->set("uFwd", camFwd.x, camFwd.y, camFwd.z);
  skyProg_->set("uTanHalf", camera.tanHalfFov());
  skyProg_->set("uAspect", aspect);
  skyProg_->set("uSunDir", sun.x, sun.y, sun.z);
  skyProg_->set("uDayFactor", sky.dayFactor());
  skyProg_->set("uTime", t);
  fullscreen_.draw();
  profiler_.end();

  // Opaque terrain.
  profiler_.begin(GpuPass::Terrain);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  // Culling stays off: cross billboards need both faces.
  glDisable(GL_CULL_FACE);

  terrainProg_->use();
  terrainProg_->setMat4("uViewProj", camera.viewProj().data());
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, atlas_->texture());
  terrainProg_->set("uAtlas", 0);
  terrainProg_->set("uTime", t);

  visible_.clear();
  drawList_.clear();
  bool anyWater = false;
  drawnTris_ = 0;
  loadedTris_ = 0;
  const Frustum& frustum = camera.frustum();
  // Culling and submission together: this is the CPU cost of terrain, and the
  // question it answers is whether the frame is submission bound or shading bound.
  {
  CpuScope csVisible(profiler_, CpuPhase::Visibility);
  for (const auto& [key, lc] : world.chunks()) {
    loadedTris_ += lc->opaqueMesh.count() / 3;
    if (lc->opaqueMesh.empty() && lc->waterMesh.empty()) continue;
    const float minx = static_cast<float>(lc->chunk.cx * world::CX);
    const float minz = static_cast<float>(lc->chunk.cz * world::CZ);
    // Column test first: one cheap reject for the whole 192 blocks, so a chunk
    // behind the camera never pays for six section tests.
    if (!frustum.testAabb(minx, 0.0f, minz, minx + world::CX,
                          static_cast<float>(world::WH), minz + world::CZ)) {
      continue;
    }
    visible_.push_back(lc.get());
    if (!lc->waterMesh.empty()) anyWater = true;
    // Then per section. This is where the underground stops being drawn: standing
    // on the surface, the sections below are outside the frustum for every chunk
    // but the few directly underfoot.
    unsigned mask = 0;
    for (int i = 0; i < world::kSections; ++i) {
      const GLsizei n = lc->opaqueMesh.sectionCount(i);
      if (n == 0) continue;
      const float y0 = static_cast<float>(i * world::kSectionHeight);
      if (!frustum.testAabb(minx, y0, minz, minx + world::CX,
                            y0 + world::kSectionHeight, minz + world::CZ)) {
        continue;
      }
      drawnTris_ += n / 3;
      mask |= 1u << i;
    }
    if (mask == 0) continue;
    const float dcx = minx + world::CX * 0.5f - camera.pos().x;
    const float dcz = minz + world::CZ * 0.5f - camera.pos().z;
    drawList_.push_back({lc.get(), mask, dcx * dcx + dcz * dcz});
  }

  // Front to back, so the depth test rejects hidden fragments against depth that
  // is already there rather than against whatever hash order happened to put down
  // first. Measured honestly: while the chunk buffers were still DYNAMIC_DRAW this
  // was worth exactly nothing (36.51 -> 36.35 ms, i.e. noise), because the pass
  // was starved on PCIe vertex fetch and no amount of fragment rejection helps
  // with that. With that fixed it is worth about 0.1 ms of a 1.1 ms pass. Kept at
  // that size because it costs 0.06 ms of CPU and one sort, and because it is the
  // half of the win that grows if the `discard` ever leaves the terrain shader.
  std::sort(drawList_.begin(), drawList_.end(),
            [](const DrawItem& a, const DrawItem& b) { return a.d2 < b.d2; });
  for (const DrawItem& it : drawList_) it.chunk->opaqueMesh.drawSectionMask(it.mask);
  }
  profiler_.end();
  drawnChunks_ = static_cast<int>(visible_.size());

  // Entities share the opaque pass: they write the same three targets, so the
  // composite lights a sheep on exactly the curve it lights the grass under it.
  {
    GpuScope gs(profiler_, GpuPass::Entities);
    if (entities_) entityRenderer_.drawGBuffer(world, *entities_, camera, animClock_);
  }

  // ================ Pass 1b: screen-space AO, blurred ================
  GBuffer::Aux* ssaoTarget = nullptr;
  {
    GpuScope gs(profiler_, GpuPass::Ssao);
    ssaoTarget = renderSSAO(camera, screenWidth, screenHeight);
  }

  // ================= Pass 2: composite (lighting) ====================
  profiler_.begin(GpuPass::Composite);
  gbuffer_.bindLitColor();
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_BLEND);

  compositeProg_->use();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gbuffer_.albedo());
  compositeProg_->set("uAlbedo", 0);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, gbuffer_.light());
  compositeProg_->set("uLight", 1);
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, gbuffer_.normal());
  compositeProg_->set("uNormal", 2);
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_2D, gbuffer_.depth());
  compositeProg_->set("uDepth", 3);
  glActiveTexture(GL_TEXTURE4);
  glBindTexture(GL_TEXTURE_2D, shadowTex_ ? shadowTex_ : whiteTex_);
  compositeProg_->set("uShadowMap", 4);
  glActiveTexture(GL_TEXTURE5);
  glBindTexture(GL_TEXTURE_2D, ssaoTarget ? ssaoTarget->tex : whiteTex_);
  compositeProg_->set("uSSAO", 5);

  compositeProg_->setMat4("uViewProj", camera.viewProj().data());
  setCameraUniforms(*compositeProg_, camera, aspect);
  compositeProg_->set("uFogColor", fog.x, fog.y, fog.z);
  compositeProg_->set("uFogNear", fogNear);
  compositeProg_->set("uFogFar", fogFar);
  compositeProg_->set("uSunDir", cel.dir.x, cel.dir.y, cel.dir.z);
  compositeProg_->set("uSunColor", cel.color.x, cel.color.y, cel.color.z);
  compositeProg_->set("uSunStrength", cel.strength);
  compositeProg_->set("uDaylight", sky.daylight());

  compositeProg_->setMat4("uLightVP", lightVP_.data());
  compositeProg_->set("uShadowEnable", shadowActive_ ? 1.0f : 0.0f);
  compositeProg_->set("uShadowTexel", shadowSize_ ? 1.0f / shadowSize_ : 0.0f);
  compositeProg_->set("uShadowTexelWorld",
                      shadowSize_ ? (2.0f * quality_.shadowRange) / shadowSize_ : 0.0f);
  // A world-space bias converted into the current ortho span, so the setting is
  // independent of the shadow range.
  compositeProg_->set("uShadowBias",
                      shadowDepthSpan_ > 0 ? quality_.shadowBias / shadowDepthSpan_ : 0.001f);
  compositeProg_->set("uShadowSteps", quality_.shadowSteps);
  compositeProg_->set("uShadowDist", quality_.shadowDist);
  compositeProg_->set("uLightShadow", quality_.lightShadow);
  compositeProg_->set("uDebug", static_cast<float>(debug_));

  // Clouds march in the sky branch; their ground shadows use the TRUE sun direction,
  // since cel.dir flips to the moon at night.
  const Vec3 cloudAmb {(hz.x + zn.x) * 0.55f, (hz.y + zn.y) * 0.55f, (hz.z + zn.z) * 0.55f};
  compositeProg_->set("uTime", t);
  compositeProg_->set("uCloudCover", quality_.cloudCover);
  compositeProg_->set("uCloudBase", cloudBase);
  compositeProg_->set("uCloudSteps", quality_.cloudSteps);
  compositeProg_->set("uCloudShadow", quality_.cloudSteps > 0 ? quality_.cloudShadows : 0.0f);
  compositeProg_->set("uCloudSunDir", sun.x, sun.y, sun.z);
  compositeProg_->set("uCloudAmb", cloudAmb.x, cloudAmb.y, cloudAmb.z);
  compositeProg_->set("uCloudDay", sky.dayFactor());

  // An emitter block in hand glows from the camera.
  world::BlockId heldBlock = world::kAir;
  if (!heldItem.empty()) {
    const game::ItemDef* it = game::getItem(heldItem);
    if (it && it->type == game::ItemType::Block) heldBlock = it->blockId;
  }
  {
    CpuScope cs(profiler_, CpuPhase::CollectLights);
    collectLights(world, camera.pos(), heldBlock);
  }
  compositeProg_->set("uLightCount", lightCount_);
  if (lightCount_ > 0) {
    compositeProg_->setVec3Array("uLightPos", lightPos_.data(), lightCount_);
    compositeProg_->setVec3Array("uLightColor", lightColor_.data(), lightCount_);
    compositeProg_->setFloatArray("uLightRad", lightRad_.data(), lightCount_);
  }
  fullscreen_.draw();
  profiler_.end();

  // ========== Pass 2b: forward water with SSR, then selection ==========
  profiler_.begin(GpuPass::Water);
  // The reflection blit, depth copy and water pass only run when a water mesh is
  // actually in view: skipping them draws nothing different and saves two full-res
  // blits on dry scenes.
  if (anyWater) {
    const int bw = gbuffer_.bufW(), bh = gbuffer_.bufH();
    GBuffer::Aux& reflect = gbuffer_.auxTarget(3, 1.0f, GL_LINEAR);
    ensureSsrDepth(bw, bh);

    // Copy the lit opaque scene so water can sample reflections WITHOUT reading the
    // buffer it draws into, which would be a feedback loop.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gbuffer_.litColorFbo());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, reflect.fbo);
    glBlitFramebuffer(0, 0, bw, bh, 0, 0, reflect.w, reflect.h, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gbuffer_.sceneFbo());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ssrDepthFbo_);
    glBlitFramebuffer(0, 0, bw, bh, 0, 0, bw, bh, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    gbuffer_.bindLit();  // lit colour plus the scene depth

    waterProg_->use();
    waterProg_->setMat4("uViewProj", camera.viewProj().data());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas_->texture());
    waterProg_->set("uAtlas", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, reflect.tex);
    waterProg_->set("uReflect", 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ssrDepthTex_);
    waterProg_->set("uDepth", 2);
    setCameraUniforms(*waterProg_, camera, aspect);
    waterProg_->set("uDaylight", sky.daylight());
    waterProg_->set("uTime", t);
    waterProg_->set("uFogColor", fog.x, fog.y, fog.z);
    waterProg_->set("uFogNear", fogNear);
    waterProg_->set("uFogFar", fogFar);
    waterProg_->set("uHorizon", hz.x, hz.y, hz.z);
    waterProg_->set("uZenith", zn.x, zn.y, zn.z);
    waterProg_->set("uSunDir", sun.x, sun.y, sun.z);
    waterProg_->set("uSunColor", cel.color.x, cel.color.y, cel.color.z);
    waterProg_->set("uDayFactor", sky.dayFactor());
    waterProg_->set("uSSRSteps", quality_.ssrSteps);
    waterProg_->set("uReflStrength", quality_.ssrStrength);
    // Half the sky pass's cloud steps in the reflection: cheaper, and still reads.
    waterProg_->set("uCloudSteps", quality_.cloudSteps / 2);
    waterProg_->set("uCloudCover", quality_.cloudCover);
    waterProg_->set("uCloudBase", cloudBase);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    for (const world::LoadedChunk* lc : visible_) lc->waterMesh.draw();
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
  } else {
    // No water in view, but the selection draw and next frame's shadow pass still
    // expect the state the water branch would have left.
    gbuffer_.bindLit();
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
  }

  if (selection) drawSelection(camera, *selection);
  if (!heldItem.empty()) drawHeld(world, camera, sky, heldItem, aspect);
  profiler_.end();

  // ============ Pass 2c: god rays, half resolution =============
  GBuffer::Aux* godray = nullptr;
  {
    GpuScope gs(profiler_, GpuPass::Godray);
    godray = renderGodrays(camera, sky);
  }

  // ================= Pass 3: present to the screen =================
  GpuScope gsPresent(profiler_, GpuPass::Present);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, screenWidth, screenHeight);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glDisable(GL_BLEND);

  presentProg_->use();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gbuffer_.lit());
  presentProg_->set("uTex", 0);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, godray ? godray->tex : gbuffer_.lit());
  presentProg_->set("uGodray", 1);
  presentProg_->set("uGodrayEnable", godray ? 1.0f : 0.0f);
  // Safe to sample the scene depth now: we are bound to the default framebuffer,
  // not the FBO that owns the texture.
  glActiveTexture(GL_TEXTURE2);
  glBindTexture(GL_TEXTURE_2D, gbuffer_.depth());
  presentProg_->set("uDepth", 2);
  presentProg_->set("uUnderwater", underwater);
  presentProg_->set("uTime", t);
  presentProg_->set("uNear", camera.near());
  presentProg_->set("uFar", camera.far());
  presentProg_->set("uWaterTint", 0.10f, 0.26f, 0.36f);
  fullscreen_.draw();

  glBindVertexArray(0);
  glActiveTexture(GL_TEXTURE0);
}

}  // namespace hr::render
