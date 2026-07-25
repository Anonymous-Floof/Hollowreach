// Deferred renderer (the "Illumination" rework).
//
// Pass 1  G-buffer : sky + opaque terrain + entities render their surface data
//                    (albedo, baked light, normal, depth) into the G-buffer.
// Pass 2  composite: a fullscreen lighting shader reads the G-buffer and writes
//                    the lit colour. (Phase 1 reproduces the old forward look;
//                    later phases add sun/point lights, contact shadows, SSAO,
//                    god-rays, water reflections.)
// Pass 2b forward  : translucent water and the selection outline draw forward
//                    into the lit buffer, depth-testing the scene. The held
//                    viewmodel follows, into the same colour but with its own
//                    scratch depth (it lives in a separate near-field
//                    projection) — see drawHeld and render/viewmodel.js.
// Pass 3  present  : the lit buffer is upscaled/copied to the screen.

import { createProgram } from "../core/gl.js";
import { TERRAIN_VS, TERRAIN_FS, LINE_VS, LINE_FS, VIEWMODEL_VS, VIEWMODEL_FS } from "../core/shaders.js";
import {
  GBUF_SKY_VS, GBUF_SKY_FS, GBUF_TERRAIN_VS, GBUF_TERRAIN_FS,
  FULLSCREEN_VS, COMPOSITE_FS, PRESENT_FS, GODRAY_FS, SSAO_FS, SSAO_BLUR_FS,
  WATER_VS, WATER_FS,
  SHADOW_TERRAIN_VS, SHADOW_TERRAIN_FS, SHADOW_ENTITY_VS, SHADOW_ENTITY_FS,
} from "../core/shaders_deferred.js";
import { mat4, aabbInFrustum } from "../core/mat4.js";
import { Camera } from "../core/camera.js";
import { CX, CZ, WH } from "../world/chunk.js";
import { emitOf, lightColorOf } from "../world/blocks.js";
import { getItem } from "../game/items.js";
import { EntityRenderer } from "./entityrenderer.js";
import { GBuffer } from "./gbuffer.js";
import { Viewmodel } from "./viewmodel.js";
import { PANO_DIRS, PANO_UPS } from "./panorama.js";

export class Renderer {
  constructor(gl, atlas) {
    this.gl = gl;
    this.atlas = atlas;

    // deferred programs
    this.skyG = createProgram(gl, GBUF_SKY_VS, GBUF_SKY_FS, ["aPos"]);
    this.terrainG = createProgram(gl, GBUF_TERRAIN_VS, GBUF_TERRAIN_FS, ["aPos", "aUV", "aShade", "aSky", "aBlock", "aWave"]);
    this.composite = createProgram(gl, FULLSCREEN_VS, COMPOSITE_FS, ["aPos"]);
    this.present = createProgram(gl, FULLSCREEN_VS, PRESENT_FS, ["aPos"]);
    this.godray = createProgram(gl, FULLSCREEN_VS, GODRAY_FS, ["aPos"]);
    this.ssao = createProgram(gl, FULLSCREEN_VS, SSAO_FS, ["aPos"]);
    this.ssaoBlur = createProgram(gl, FULLSCREEN_VS, SSAO_BLUR_FS, ["aPos"]);
    this.water = createProgram(gl, WATER_VS, WATER_FS, ["aPos", "aUV", "aShade", "aSky", "aBlock", "aWave"]);
    // SSR samples the scene depth, but the water pass has the real depth bound as
    // the FBO's depth attachment (for depth-testing) — sampling that would be a
    // feedback loop. So we blit depth into this standalone copy and sample it.
    this.ssrDepthTex = null; this.ssrDepthFBO = null; this.ssrDW = 0; this.ssrDH = 0;
    // 1x1 white texture = "no occlusion", bound to the composite's uSSAO when AO is off
    this.whiteTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.whiteTex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([255, 255, 255, 255]));
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    // sun shadow map (depth-only). Attribute locations come from the shaders'
    // explicit layout() qualifiers, so no name bindings are needed.
    this.shadowTerrain = createProgram(gl, SHADOW_TERRAIN_VS, SHADOW_TERRAIN_FS, []);
    this.shadowEntity = createProgram(gl, SHADOW_ENTITY_VS, SHADOW_ENTITY_FS, []);
    // forward program (still used for translucent water)
    this.forward = createProgram(gl, TERRAIN_VS, TERRAIN_FS, ["aPos", "aUV", "aShade", "aSky", "aBlock", "aWave"]);
    this.line = createProgram(gl, LINE_VS, LINE_FS, ["aPos"]);
    // the first-person hand: its own program (pose baked into uMVP) and its own
    // pose/animation state, see render/viewmodel.js
    this.viewmodelProg = createProgram(gl, VIEWMODEL_VS, VIEWMODEL_FS, []);
    this.viewmodel = new Viewmodel();
    this._heldModel = mat4.create();

    this.gbuffer = new GBuffer(gl);
    // Quality knobs (overridden via setQuality from the graphics setting).
    this.quality = {
      scale: 1,            // internal render scale
      shadowSize: 2048,    // sun shadow-map resolution (0 = off). Driven by the quality
                           // preset + the Cast Shadows toggle via applyGraphicsSettings.
      shadowRange: 72,     // world half-extent the shadow map covers around the camera
      shadowBias: 0.12,    // shadow bias in WORLD units (small: back-face depth avoids acne)
      shadowSlope: 2.5,    // polygonOffset slope factor (hardware acne bias)
      shadowSteps: 0,      // point-light contact-shadow steps (0 = off; sun uses the map)
      shadowDist: 6,       // point-light contact-shadow march length
      lightShadow: 0,      // 1 = contact-shadow dynamic point lights too
      maxLights: 16,       // cap on dynamic coloured point lights
      ssaoSamples: 16,     // screen-space AO samples (0 = off)
      ssaoRadius: 1.0,     // SSAO world-space sample radius (~1 voxel)
      ssaoStrength: 1.15,  // SSAO darkening amount
      godrays: 1,          // 1 = volumetric sun shafts on
      godraySamples: 48,   // god-ray march steps (half-res pass)
      godrayStrength: 0.55,// god-ray brightness
      godrayScale: 0.5,    // god-ray pass resolution fraction
      ssrSteps: 24,        // water SSR raymarch steps (0 = sky-only reflection, cheapest)
      ssrStrength: 1.0,    // water reflectivity multiplier
      cloudSteps: 24,      // volumetric cloud raymarch steps (0 = clouds off)
      cloudShadows: 1,     // 1 = clouds cast moving shadows on terrain
      cloudCover: 0.5,     // cloud coverage 0..1 (bigger = more/denser clouds)
    };
    // sun shadow-map render target (depth texture only), (re)built on size change
    this.shadowSize = 0;
    this.shadowTex = null;
    this.shadowFBO = null;
    this._lightView = mat4.create();
    this._lightProj = mat4.create();
    this._lightVP = mat4.create();
    this._shadowActive = false;
    this._ensureShadowFBO(this.quality.shadowSize);

    // Persistent buffers for the dynamic point-light list (held + emitters).
    this.MAX_LIGHTS = 16;
    this._lightPos = new Float32Array(this.MAX_LIGHTS * 3);
    this._lightColor = new Float32Array(this.MAX_LIGHTS * 3);
    this._lightRad = new Float32Array(this.MAX_LIGHTS);
    this._lightCount = 0;
    this._lightScratch = [];   // reused candidate array

    // fullscreen triangle, shared by the sky / composite / present passes
    this.fsVAO = gl.createVertexArray();
    gl.bindVertexArray(this.fsVAO);
    const sb = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, sb);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);

    // dynamic buffer for the selection outline
    this.lineVAO = gl.createVertexArray();
    gl.bindVertexArray(this.lineVAO);
    this.lineVBO = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.lineVBO);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.bindVertexArray(null);

    this.heldProj = mat4.create();
    this.heldMVP = mat4.create();
    this._invVP = mat4.create();
    this._visible = [];                    // reused per-frame visible-chunk list
    this._selArr = new Float32Array(72);   // reused selection-outline vertex data
    this.entityRenderer = new EntityRenderer(gl, atlas);
  }

  resize(w, h) { this.w = w; this.h = h; this.gl.viewport(0, 0, w, h); }

  // ---- panorama / screenshot capture ----
  // Render the scene as six 90° cube faces from `eye` and return them as JPEG
  // data URLs in the order [+X,-X,+Y,-Y,+Z,-Z] (matching render/panorama.js). The
  // canvas is briefly resized square; render() self-sizes to it. Callers do this
  // between visible frames (or behind an overlay) so nothing flashes on screen.
  capturePanorama(world, sky, eye, faceSize = 1024) {
    const gl = this.gl, canvas = gl.canvas;
    const ow = canvas.width, oh = canvas.height;
    canvas.width = faceSize; canvas.height = faceSize;
    if (!this._capCam) this._capCam = new Camera();
    const cam = this._capCam;
    const faces = [];
    for (let i = 0; i < 6; i++) {
      cam.setLook(eye, PANO_DIRS[i], PANO_UPS[i], 90, 1);
      this.render(world, cam, sky, null, null, 0);   // no selection, no held item
      faces.push(this._grab(faceSize, faceSize, 0.86));
    }
    canvas.width = ow; canvas.height = oh;
    return faces;
  }

  // Grab the current default-framebuffer contents as a flipped JPEG data URL.
  // For a flat screenshot: call right after a normal render in the SAME JS turn —
  // the drawing buffer is only guaranteed valid until the browser next composites.
  captureFlat(quality = 0.92) {
    const gl = this.gl;
    return { data: this._grab(gl.canvas.width, gl.canvas.height, quality), w: gl.canvas.width, h: gl.canvas.height };
  }

  _grab(w, h, quality) {
    const gl = this.gl;
    const px = new Uint8Array(w * h * 4);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
    if (!this._grabCanvas) this._grabCanvas = document.createElement("canvas");
    const c = this._grabCanvas; c.width = w; c.height = h;
    const ctx = c.getContext("2d");
    const img = ctx.createImageData(w, h);
    const row = w * 4;
    for (let y = 0; y < h; y++) {                          // GL is bottom-up; flip to top-down
      const s = (h - 1 - y) * row;
      img.data.set(px.subarray(s, s + row), y * row);
    }
    ctx.putImageData(img, 0, 0);
    return c.toDataURL("image/jpeg", quality);
  }

  setQuality(q) {
    this.quality = Object.assign(this.quality, q);
    if (this.quality.shadowSize !== this.shadowSize) this._ensureShadowFBO(this.quality.shadowSize);
  }

  // (Re)create the depth-only sun shadow map at the requested size (0 = none).
  _ensureShadowFBO(size) {
    const gl = this.gl;
    if (this.shadowTex) { gl.deleteTexture(this.shadowTex); this.shadowTex = null; }
    if (this.shadowFBO) { gl.deleteFramebuffer(this.shadowFBO); this.shadowFBO = null; }
    this.shadowSize = size;
    if (!size) return;
    this.shadowTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.shadowTex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.DEPTH_COMPONENT24, size, size, 0, gl.DEPTH_COMPONENT, gl.UNSIGNED_INT, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    // clamp + a white border feel: sampling outside returns "far" (lit), handled
    // in-shader by the uv range check, so plain clamp is fine.
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    this.shadowFBO = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.shadowFBO);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.TEXTURE_2D, this.shadowTex, 0);
    gl.drawBuffers([gl.NONE]);
    gl.readBuffer(gl.NONE);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  }

  // Render the opaque terrain + entities depth-only from the sun's point of view
  // into the shadow map, and build the light view-proj the composite samples.
  renderShadowMap(world, camera, sky) {
    this._shadowActive = this.shadowSize > 0 && sky.sunHeight() > 0.08;
    if (!this._shadowActive) return;
    const gl = this.gl;
    const cel = sky.celestial();
    const L = cel.dir;
    const range = this.quality.shadowRange;
    const dist = range * 1.5;
    const up = Math.abs(L[1]) > 0.99 ? [0, 0, 1] : [0, 1, 0];
    // Quantise the centre to whole blocks first, so the head-bob (a few-cm,
    // sub-texel camera wobble) can't nudge the texel snap across a boundary and
    // shimmer every shadow edge. The light-space snap below still pins edges to
    // the texel grid as you actually walk block-to-block.
    const cp = [Math.round(camera.pos[0]), Math.round(camera.pos[1]), Math.round(camera.pos[2])];

    // Centre the shadow box on the camera, but SNAP that centre to the shadow
    // map's texel grid IN LIGHT SPACE (along the light's right/up axes) — not the
    // world axes. World-axis snapping leaves the texel grid sliding under the
    // world as the camera moves, which makes shadow edges crawl/"swim" when you
    // strafe. Snapping in light space pins each texel to a fixed world spot so
    // shadows stay put. (Forward = -L; right = norm(fwd×up); u = right×fwd —
    // matching mat4.lookAt's internal basis.)
    const fx = -L[0], fy = -L[1], fz = -L[2];
    let sx = fy * up[2] - fz * up[1], sy = fz * up[0] - fx * up[2], sz = fx * up[1] - fy * up[0];
    const sl = Math.hypot(sx, sy, sz) || 1; sx /= sl; sy /= sl; sz /= sl;
    const ux = sy * fz - sz * fy, uy = sz * fx - sx * fz, uz = sx * fy - sy * fx;
    const texel = (2 * range) / this.shadowSize;
    const lx = cp[0] * sx + cp[1] * sy + cp[2] * sz;   // camera in light right/up coords
    const ly = cp[0] * ux + cp[1] * uy + cp[2] * uz;
    const dlx = Math.round(lx / texel) * texel - lx;
    const dly = Math.round(ly / texel) * texel - ly;
    const cx2 = cp[0] + dlx * sx + dly * ux, cy2 = cp[1] + dlx * sy + dly * uy, cz2 = cp[2] + dlx * sz + dly * uz;

    const eye = [cx2 + L[0] * dist, cy2 + L[1] * dist, cz2 + L[2] * dist];
    mat4.lookAt(this._lightView, eye, [cx2, cy2, cz2], up);
    // tight near/far around the scene slab → good depth precision (small bias).
    const nearP = Math.max(0.5, dist - range - 3), farP = dist + range + 3;
    this._shadowDepthSpan = farP - nearP;
    mat4.ortho(this._lightProj, -range, range, -range, range, nearP, farP);
    mat4.multiply(this._lightVP, this._lightProj, this._lightView);

    gl.bindFramebuffer(gl.FRAMEBUFFER, this.shadowFBO);
    gl.viewport(0, 0, this.shadowSize, this.shadowSize);
    // CRITICAL: depthMask must be true BEFORE the clear — gl.clear respects the
    // depth write mask, and the previous frame's present pass leaves it false.
    // With the clear masked out, the map accumulated the min depth ever rendered
    // per texel; because the light box is camera-centred, that stale content is
    // glued to the camera and shadows appeared to strafe along with the player.
    gl.enable(gl.DEPTH_TEST);
    gl.depthMask(true);
    gl.clear(gl.DEPTH_BUFFER_BIT);
    gl.disable(gl.BLEND);
    gl.disable(gl.CULL_FACE);
    const slopeF = this.quality.shadowSlope;
    if (slopeF > 0) { gl.enable(gl.POLYGON_OFFSET_FILL); gl.polygonOffset(slopeF, this.quality.shadowUnits || 4.0); }
    else gl.disable(gl.POLYGON_OFFSET_FILL);

    const sp = this.shadowTerrain;
    gl.useProgram(sp);
    gl.uniformMatrix4fv(sp.uniform("uLightVP"), false, this._lightVP);
    gl.uniform1f(sp.uniform("uTime"), performance.now() * 0.001);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.atlas.texture);
    gl.uniform1i(sp.uniform("uAtlas"), 0);
    const cull = range + CX * 2, cull2 = cull * cull;
    // Light-space extent cull: the ortho box only spans ±range along the light's
    // right (s) / up (u) axes around the snapped centre, so a chunk whose AABB
    // projection lies wholly outside it can't reach a single shadow texel.
    // (Depth along the light dir is irrelevant to an ortho footprint.) The pad is
    // the chunk AABB's projected half-extent — conservative, so this culls only
    // chunks that provably contribute nothing: the map output is bit-identical.
    const hy = WH * 0.5;
    // +1 block slack: the shadow VS sways leaves/water slightly past the AABB
    const padS = range + 1 + Math.abs(sx) * 8 + Math.abs(sy) * hy + Math.abs(sz) * 8;
    const padU = range + 1 + Math.abs(ux) * 8 + Math.abs(uy) * hy + Math.abs(uz) * 8;
    for (const c of world.chunks.values()) {
      if (!c.meshOpaque) continue;
      const dx = c.cx * CX + CX * 0.5 - camera.pos[0], dz = c.cz * CZ + CZ * 0.5 - camera.pos[2];
      if (dx * dx + dz * dz > cull2) continue;
      const mx = c.cx * CX + 8 - cx2, my = hy - cy2, mz = c.cz * CZ + 8 - cz2;
      if (Math.abs(mx * sx + my * sy + mz * sz) > padS) continue;
      if (Math.abs(mx * ux + my * uy + mz * uz) > padU) continue;
      gl.bindVertexArray(c.meshOpaque.vao);
      gl.drawArrays(gl.TRIANGLES, 0, c.meshOpaque.count);
    }
    this.entityRenderer.drawShadow(this.shadowEntity, this._lightVP, world);
    gl.cullFace(gl.BACK);
    gl.disable(gl.CULL_FACE);
    // don't leak the shadow pass's polygon offset into the G-buffer pass — it
    // would skew every depth-based reconstruction (shadows, SSAO, fog) view-
    // dependently.
    gl.disable(gl.POLYGON_OFFSET_FILL);
    gl.polygonOffset(0, 0);
  }

  // Gather the nearest coloured point lights into the persistent buffers: every
  // emitter cell (from chunk.emitters) within range, plus the held light if the
  // player is carrying an emitter block. Nearest `maxLights` are kept.
  collectLights(world, camPos, heldBlockId) {
    const cap = Math.min(this.MAX_LIGHTS, this.quality.maxLights);
    const cands = this._lightScratch;
    cands.length = 0;
    const cx = camPos[0], cy = camPos[1], cz = camPos[2];
    const RANGE = 28, R2 = RANGE * RANGE;

    // held light: an emitter block in hand glows from the camera
    if (heldBlockId) {
      const e = emitOf(heldBlockId);
      if (e > 0) {
        const col = lightColorOf(heldBlockId);
        cands.push({ d2: -1, x: cx, y: cy, z: cz, r: e * 0.9 + 3, c: col, i: Math.min(1, e / 14) });
      }
    }

    for (const c of world.chunks.values()) {
      if (!c.emitters || !c.emitters.length) continue;
      // cheap chunk-level reject
      const mx = c.cx * CX + CX * 0.5, mz = c.cz * CZ + CZ * 0.5;
      if ((mx - cx) * (mx - cx) + (mz - cz) * (mz - cz) > (RANGE + CX) * (RANGE + CX)) continue;
      for (const em of c.emitters) {
        const dx = em.x - cx, dy = em.y - cy, dz = em.z - cz;
        const d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > R2) continue;
        const e = emitOf(em.id);
        cands.push({ d2, x: em.x, y: em.y, z: em.z, r: e * 0.9 + 2, c: lightColorOf(em.id), i: Math.min(1, e / 14) });
      }
    }

    cands.sort((a, b) => a.d2 - b.d2);
    const n = Math.min(cap, cands.length);
    for (let k = 0; k < n; k++) {
      const L = cands[k];
      this._lightPos[k * 3] = L.x; this._lightPos[k * 3 + 1] = L.y; this._lightPos[k * 3 + 2] = L.z;
      // colour carries intensity so the shader just adds it
      this._lightColor[k * 3] = L.c[0] * L.i; this._lightColor[k * 3 + 1] = L.c[1] * L.i; this._lightColor[k * 3 + 2] = L.c[2] * L.i;
      this._lightRad[k] = L.r;
    }
    this._lightCount = n;
  }

  // Volumetric sun shafts. Projects the sun to screen space, then a half-res
  // fullscreen shader marches the depth buffer toward it accumulating sky
  // visibility. Returns the aux target {tex,...} the present pass adds, or null
  // when god-rays are off / the sun isn't a valid on-screen light source.
  renderGodrays(camera, sky) {
    if (!this.quality.godrays || this.quality.godraySamples <= 0) return null;
    if (sky.sunHeight() <= -0.02) return null;        // sun below the horizon
    const gl = this.gl;
    const sun = sky.sunDir();
    // project a far point along the sun direction with the camera view-proj
    const x = camera.pos[0] + sun[0] * 1000, y = camera.pos[1] + sun[1] * 1000, z = camera.pos[2] + sun[2] * 1000;
    const vp = camera.viewProj;
    const cw = vp[3] * x + vp[7] * y + vp[11] * z + vp[15];
    if (cw <= 0) return null;                          // sun behind the camera
    const ndcX = (vp[0] * x + vp[4] * y + vp[8] * z + vp[12]) / cw;
    const ndcY = (vp[1] * x + vp[5] * y + vp[9] * z + vp[13]) / cw;
    // fade out as the sun moves off-screen (allow some overscan so shafts still
    // enter from just past the edge), as it nears the horizon, and at night.
    const edge = Math.max(Math.abs(ndcX), Math.abs(ndcY));
    const fade = 1 - Math.min(1, Math.max(0, (edge - 0.95) / (1.6 - 0.95)));
    const hgt = Math.min(1, Math.max(0, (sky.sunHeight() + 0.02) / 0.17));
    const valid = fade * hgt * sky.dayFactor();
    if (valid <= 0.001) return null;

    const a = this.gbuffer.auxTarget(0, this.quality.godrayScale, gl.LINEAR);
    gl.bindFramebuffer(gl.FRAMEBUFFER, a.fbo);
    gl.viewport(0, 0, a.w, a.h);
    gl.disable(gl.DEPTH_TEST);
    gl.depthMask(false);
    gl.disable(gl.BLEND);
    const g = this.godray;
    gl.useProgram(g);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.gbuffer.depth);
    gl.uniform1i(g.uniform("uDepth"), 0);
    gl.uniform2f(g.uniform("uSunScreen"), ndcX * 0.5 + 0.5, ndcY * 0.5 + 0.5);
    const cel = sky.celestial();
    gl.uniform3f(g.uniform("uSunColor"), cel.color[0], cel.color[1], cel.color[2]);
    gl.uniform1f(g.uniform("uGodrayValid"), valid);
    gl.uniform1f(g.uniform("uGodrayStrength"), this.quality.godrayStrength);
    gl.uniform1i(g.uniform("uGodraySamples"), this.quality.godraySamples);
    gl.bindVertexArray(this.fsVAO);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    return a;
  }

  // Standalone depth texture the water SSR samples (a copy of the scene depth, so
  // we don't sample the live depth attachment bound during the water pass).
  _ensureSSRDepth(w, h) {
    if (this.ssrDW === w && this.ssrDH === h && this.ssrDepthTex) return;
    const gl = this.gl;
    if (this.ssrDepthTex) gl.deleteTexture(this.ssrDepthTex);
    if (this.ssrDepthFBO) gl.deleteFramebuffer(this.ssrDepthFBO);
    this.ssrDW = w; this.ssrDH = h;
    this.ssrDepthTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.ssrDepthTex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.DEPTH_COMPONENT24, w, h, 0, gl.DEPTH_COMPONENT, gl.UNSIGNED_INT, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    this.ssrDepthFBO = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.ssrDepthFBO);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.TEXTURE_2D, this.ssrDepthTex, 0);
    gl.drawBuffers([gl.NONE]); gl.readBuffer(gl.NONE);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  }

  // Screen-space AO in its own pass (so it can be blurred). Returns the blurred
  // AO aux target the composite samples, or null when AO is off (composite then
  // uses the white "no occlusion" texture).
  renderSSAO(camera) {
    if (this.quality.ssaoSamples <= 0) return null;
    const gl = this.gl, gb = this.gbuffer;
    const raw = gb.auxTarget(1, 1.0, gl.LINEAR);
    const blur = gb.auxTarget(2, 1.0, gl.LINEAR);
    const W = gl.canvas.width, H = gl.canvas.height;
    gl.disable(gl.DEPTH_TEST); gl.depthMask(false); gl.disable(gl.BLEND);
    gl.bindVertexArray(this.fsVAO);

    // --- AO pass ---
    gl.bindFramebuffer(gl.FRAMEBUFFER, raw.fbo);
    gl.viewport(0, 0, raw.w, raw.h);
    const p = this.ssao;
    gl.useProgram(p);
    gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, gb.depth); gl.uniform1i(p.uniform("uDepth"), 0);
    gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, gb.gNormal); gl.uniform1i(p.uniform("uNormal"), 1);
    gl.uniformMatrix4fv(p.uniform("uViewProj"), false, camera.viewProj);
    gl.uniform3f(p.uniform("uCamPos"), camera.pos[0], camera.pos[1], camera.pos[2]);
    const cv = camera.view;
    gl.uniform3f(p.uniform("uCamRight"), cv[0], cv[4], cv[8]);
    gl.uniform3f(p.uniform("uCamUp"), cv[1], cv[5], cv[9]);
    gl.uniform3f(p.uniform("uCamFwd"), -cv[2], -cv[6], -cv[10]);
    gl.uniform1f(p.uniform("uTanHalf"), Math.tan((camera.fov * Math.PI) / 180 / 2));
    gl.uniform1f(p.uniform("uAspect"), W / H);
    gl.uniform1f(p.uniform("uNear"), camera.near);
    gl.uniform1f(p.uniform("uFar"), camera.far);
    gl.uniform1i(p.uniform("uSamples"), this.quality.ssaoSamples);
    gl.uniform1f(p.uniform("uRadius"), this.quality.ssaoRadius);
    gl.uniform1f(p.uniform("uStrength"), this.quality.ssaoStrength);
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    // --- depth-aware blur ---
    gl.bindFramebuffer(gl.FRAMEBUFFER, blur.fbo);
    gl.viewport(0, 0, blur.w, blur.h);
    const bp = this.ssaoBlur;
    gl.useProgram(bp);
    gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, raw.tex); gl.uniform1i(bp.uniform("uSSAO"), 0);
    gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, gb.depth); gl.uniform1i(bp.uniform("uDepth"), 1);
    gl.uniform2f(bp.uniform("uTexel"), 1 / blur.w, 1 / blur.h);
    gl.uniform1f(bp.uniform("uNear"), camera.near);
    gl.uniform1f(bp.uniform("uFar"), camera.far);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    return blur;
  }

  // `heldItem` is the selected hotbar item's key (any item — the viewmodel
  // draws blocks and sprite items alike); block-item keys also feed the
  // held-light glow in collectLights. The hand's pose and animation clocks live
  // on this.viewmodel, which the game ticks (see main.updatePlaying).
  render(world, camera, sky, selection, heldItem, underwater = 0) {
    const gl = this.gl;
    const time = performance.now() * 0.001;
    let heldBlockId = 0;
    if (heldItem) { const hit = getItem(heldItem); if (hit && hit.type === "block") heldBlockId = hit.blockId; }
    const W = gl.canvas.width, H = gl.canvas.height;
    this.gbuffer.resize(W, H, this.quality.scale);
    const fog = sky.fogColor();

    const far = (world.renderDist - 1.2) * CX;
    const mf = sky.morningFog();
    const fogNear = Math.max(16, far * 0.55) * (1 - 0.62 * mf);
    const fogFar = Math.max(40, far) * (1 - 0.42 * mf);
    // sun/moon colour, needed by the cloud shading in the sky pass as well as the
    // composite lighting below (computed once).
    const cel = sky.celestial();

    // ================= Pass 0: sun shadow map =================
    this.renderShadowMap(world, camera, sky);

    // ================= Pass 1: scene G-buffer =================
    this.gbuffer.bindScene();
    gl.clearColor(0, 0, 0, 1);
    gl.depthMask(true);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    // sky fills the background (no depth, so geometry overwrites it)
    gl.disable(gl.DEPTH_TEST);
    gl.depthMask(false);
    gl.useProgram(this.skyG);
    const hz = sky.horizon(), zn = sky.zenith();
    const sp = this.skyG;
    gl.uniform3f(sp.uniform("uHorizon"), hz[0], hz[1], hz[2]);
    gl.uniform3f(sp.uniform("uZenith"), zn[0], zn[1], zn[2]);
    const v = camera.view;
    gl.uniform3f(sp.uniform("uRight"), v[0], v[4], v[8]);
    gl.uniform3f(sp.uniform("uUp"), v[1], v[5], v[9]);
    gl.uniform3f(sp.uniform("uFwd"), -v[2], -v[6], -v[10]);
    gl.uniform1f(sp.uniform("uTanHalf"), Math.tan((camera.fov * Math.PI) / 180 / 2));
    gl.uniform1f(sp.uniform("uAspect"), W / H);
    const sun = sky.sunDir();
    gl.uniform3f(sp.uniform("uSunDir"), sun[0], sun[1], sun[2]);
    gl.uniform1f(sp.uniform("uDayFactor"), sky.dayFactor());
    gl.uniform1f(sp.uniform("uTime"), time);
    gl.bindVertexArray(this.fsVAO);
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    // opaque terrain
    gl.enable(gl.DEPTH_TEST);
    gl.depthMask(true);
    gl.disable(gl.BLEND);
    gl.disable(gl.CULL_FACE);
    const p = this.terrainG;
    gl.useProgram(p);
    gl.uniformMatrix4fv(p.uniform("uViewProj"), false, camera.viewProj);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.atlas.texture);
    gl.uniform1i(p.uniform("uAtlas"), 0);
    gl.uniform1f(p.uniform("uTime"), time);
    // (Sun shadows are sampled later in the composite pass from the reconstructed
    // world position — the G-buffer shaders never touch the shadow texture, so
    // toggling shadows off can't leave a deleted texture bound to a live draw.)

    const visible = this._visible;
    visible.length = 0;
    let anyWater = false;
    for (const c of world.chunks.values()) {
      const minx = c.cx * CX, minz = c.cz * CZ;
      if (!aabbInFrustum(camera.planes, minx, 0, minz, minx + CX, WH, minz + CZ)) continue;
      visible.push(c);
      if (c.meshWater) anyWater = true;
      if (c.meshOpaque) {
        gl.bindVertexArray(c.meshOpaque.vao);
        gl.drawArrays(gl.TRIANGLES, 0, c.meshOpaque.count);
      }
    }

    // entities into the G-buffer (they receive sun shadows in the composite via
    // their reconstructed world pos, and cast into the shadow map via drawShadow)
    this.entityRenderer.drawGBuffer(camera, world, sky);

    // ============ Pass 1b: screen-space AO (own pass, blurred) ============
    const ssaoTarget = this.renderSSAO(camera);

    // ================= Pass 2: composite (lighting) =================
    this.gbuffer.bindLitColor();
    gl.disable(gl.DEPTH_TEST);
    gl.depthMask(false);
    gl.disable(gl.BLEND);
    const cp = this.composite;
    gl.useProgram(cp);
    const gb = this.gbuffer;
    gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, gb.gAlbedo); gl.uniform1i(cp.uniform("uAlbedo"), 0);
    gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, gb.gLight); gl.uniform1i(cp.uniform("uLight"), 1);
    gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, gb.gNormal); gl.uniform1i(cp.uniform("uNormal"), 2);
    gl.activeTexture(gl.TEXTURE3); gl.bindTexture(gl.TEXTURE_2D, gb.depth); gl.uniform1i(cp.uniform("uDepth"), 3);
    gl.uniformMatrix4fv(cp.uniform("uViewProj"), false, camera.viewProj);
    gl.uniform3f(cp.uniform("uCamPos"), camera.pos[0], camera.pos[1], camera.pos[2]);
    // camera basis + frustum params for the stable ray/linear-depth unproject
    const cv = camera.view;
    gl.uniform3f(cp.uniform("uCamRight"), cv[0], cv[4], cv[8]);
    gl.uniform3f(cp.uniform("uCamUp"), cv[1], cv[5], cv[9]);
    gl.uniform3f(cp.uniform("uCamFwd"), -cv[2], -cv[6], -cv[10]);
    gl.uniform1f(cp.uniform("uTanHalf"), Math.tan((camera.fov * Math.PI) / 180 / 2));
    gl.uniform1f(cp.uniform("uAspect"), W / H);
    gl.uniform1f(cp.uniform("uNear"), camera.near);
    gl.uniform1f(cp.uniform("uFar"), camera.far);
    gl.uniform3f(cp.uniform("uFogColor"), fog[0], fog[1], fog[2]);
    gl.uniform1f(cp.uniform("uFogNear"), fogNear);
    gl.uniform1f(cp.uniform("uFogFar"), fogFar);
    // directional sun / moon (cel computed once at the top of render)
    gl.uniform3f(cp.uniform("uSunDir"), cel.dir[0], cel.dir[1], cel.dir[2]);
    gl.uniform3f(cp.uniform("uSunColor"), cel.color[0], cel.color[1], cel.color[2]);
    gl.uniform1f(cp.uniform("uSunStrength"), cel.strength);
    gl.uniform1f(cp.uniform("uDaylight"), sky.daylight());
    // volumetric clouds (marched here in the sky-pixel branch) + their moving
    // ground shadows. Uses the TRUE sun dir (cel.dir flips to the moon at night).
    const cloudAmb = [(hz[0] + zn[0]) * 0.55, (hz[1] + zn[1]) * 0.55, (hz[2] + zn[2]) * 0.55];
    gl.uniform1f(cp.uniform("uTime"), time);
    gl.uniform1f(cp.uniform("uCloudCover"), this.quality.cloudCover);
    gl.uniform1i(cp.uniform("uCloudSteps"), this.quality.cloudSteps);
    gl.uniform1f(cp.uniform("uCloudShadow"), (this.quality.cloudSteps > 0 ? this.quality.cloudShadows : 0));
    gl.uniform3f(cp.uniform("uCloudSunDir"), sun[0], sun[1], sun[2]);
    gl.uniform3f(cp.uniform("uCloudAmb"), cloudAmb[0], cloudAmb[1], cloudAmb[2]);
    gl.uniform1f(cp.uniform("uCloudDay"), sky.dayFactor());
    const amb = sky.ambientColor();
    gl.uniform3f(cp.uniform("uSkyAmbient"), amb[0], amb[1], amb[2]);
    gl.uniform3f(cp.uniform("uBlockColor"), 1.0, 0.82, 0.55);
    gl.uniform1i(cp.uniform("uShadowSteps"), this.quality.shadowSteps);
    gl.uniform1f(cp.uniform("uShadowDist"), this.quality.shadowDist);
    gl.uniform1f(cp.uniform("uLightShadow"), this.quality.lightShadow);
    gl.activeTexture(gl.TEXTURE5);
    gl.bindTexture(gl.TEXTURE_2D, ssaoTarget ? ssaoTarget.tex : this.whiteTex);
    gl.uniform1i(cp.uniform("uSSAO"), 5);
    // sun shadow map
    gl.uniformMatrix4fv(cp.uniform("uLightVP"), false, this._lightVP);
    gl.uniform1f(cp.uniform("uShadowEnable"), this._shadowActive ? 1 : 0);
    gl.uniform1f(cp.uniform("uShadowTexel"), this.shadowSize ? 1 / this.shadowSize : 0);
    gl.uniform1f(cp.uniform("uShadowTexelWorld"), this.shadowSize ? (2 * this.quality.shadowRange) / this.shadowSize : 0);
    // world-space bias -> depth bias for the current ortho span (range-independent)
    gl.uniform1f(cp.uniform("uShadowBias"), this._shadowDepthSpan ? this.quality.shadowBias / this._shadowDepthSpan : 0.001);
    gl.uniform1f(cp.uniform("uDebug"), this.debug || 0);
    if (this.shadowTex) {
      gl.activeTexture(gl.TEXTURE4);
      gl.bindTexture(gl.TEXTURE_2D, this.shadowTex);
      gl.uniform1i(cp.uniform("uShadowMap"), 4);
    }
    // dynamic coloured point lights
    this.collectLights(world, camera.pos, heldBlockId);
    gl.uniform1i(cp.uniform("uLightCount"), this._lightCount);
    if (this._lightCount > 0) {
      gl.uniform3fv(cp.uniform("uLightPos"), this._lightPos.subarray(0, this._lightCount * 3));
      gl.uniform3fv(cp.uniform("uLightColor"), this._lightColor.subarray(0, this._lightCount * 3));
      gl.uniform1fv(cp.uniform("uLightRad"), this._lightRad.subarray(0, this._lightCount));
    }
    gl.bindVertexArray(this.fsVAO);
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    // ================= Pass 2b: forward water (SSR) + selection + held =========
    // The reflection blits + depth copy + water pass only run when a water mesh
    // is actually in view — skipping them draws nothing different (there was no
    // water to draw) and saves two full-res blits on dry scenes.
    if (anyWater) {
      // Copy the lit opaque scene so water can sample reflections WITHOUT reading the
      // buffer it draws into (that would be a read/write feedback loop). gbuffer.lit
      // is the litColorFBO's colour attachment; blit it into the reflect aux target.
      const W2 = this.gbuffer.bufW(), H2 = this.gbuffer.bufH();
      const reflect = this.gbuffer.auxTarget(3, 1.0, gl.LINEAR);
      this._ensureSSRDepth(W2, H2);
      // copy lit colour -> reflect aux, and scene depth -> ssr depth copy
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER, this.gbuffer.litColorFBO);
      gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, reflect.fbo);
      gl.blitFramebuffer(0, 0, W2, H2, 0, 0, reflect.w, reflect.h, gl.COLOR_BUFFER_BIT, gl.NEAREST);
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER, this.gbuffer.sceneFBO);
      gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, this.ssrDepthFBO);
      gl.blitFramebuffer(0, 0, W2, H2, 0, 0, W2, H2, gl.DEPTH_BUFFER_BIT, gl.NEAREST);
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
      gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);

      this.gbuffer.bindLit();   // lit colour + scene depth

      const wp = this.water;
      gl.useProgram(wp);
      gl.uniformMatrix4fv(wp.uniform("uViewProj"), false, camera.viewProj);
      gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, this.atlas.texture); gl.uniform1i(wp.uniform("uAtlas"), 0);
      gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, reflect.tex); gl.uniform1i(wp.uniform("uReflect"), 1);
      gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, this.ssrDepthTex); gl.uniform1i(wp.uniform("uDepth"), 2);
      gl.uniform3f(wp.uniform("uCamPos"), camera.pos[0], camera.pos[1], camera.pos[2]);
      const wv = camera.view;
      gl.uniform3f(wp.uniform("uCamRight"), wv[0], wv[4], wv[8]);
      gl.uniform3f(wp.uniform("uCamUp"), wv[1], wv[5], wv[9]);
      gl.uniform3f(wp.uniform("uCamFwd"), -wv[2], -wv[6], -wv[10]);
      gl.uniform1f(wp.uniform("uTanHalf"), Math.tan((camera.fov * Math.PI) / 180 / 2));
      gl.uniform1f(wp.uniform("uAspect"), W2 / H2);
      gl.uniform1f(wp.uniform("uNear"), camera.near);
      gl.uniform1f(wp.uniform("uFar"), camera.far);
      gl.uniform1f(wp.uniform("uDaylight"), sky.daylight());
      gl.uniform1f(wp.uniform("uTime"), time);
      gl.uniform3f(wp.uniform("uFogColor"), fog[0], fog[1], fog[2]);
      gl.uniform1f(wp.uniform("uFogNear"), fogNear);
      gl.uniform1f(wp.uniform("uFogFar"), fogFar);
      gl.uniform3f(wp.uniform("uHorizon"), hz[0], hz[1], hz[2]);
      gl.uniform3f(wp.uniform("uZenith"), zn[0], zn[1], zn[2]);
      gl.uniform3f(wp.uniform("uSunDir"), sun[0], sun[1], sun[2]);
      gl.uniform1f(wp.uniform("uDayFactor"), sky.dayFactor());
      gl.uniform1i(wp.uniform("uSSRSteps"), this.quality.ssrSteps);
      gl.uniform1f(wp.uniform("uReflStrength"), this.quality.ssrStrength);
      // clouds in the reflection (half the sky-pass steps — cheaper, still reads)
      gl.uniform3f(wp.uniform("uSunColor"), cel.color[0], cel.color[1], cel.color[2]);
      gl.uniform1i(wp.uniform("uCloudSteps"), (this.quality.cloudSteps / 2) | 0);
      gl.uniform1f(wp.uniform("uCloudCover"), this.quality.cloudCover);
      gl.enable(gl.DEPTH_TEST);
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
      gl.depthMask(false);
      for (const c of visible) {
        if (c.meshWater) {
          gl.bindVertexArray(c.meshWater.vao);
          gl.drawArrays(gl.TRIANGLES, 0, c.meshWater.count);
        }
      }
      gl.depthMask(true);
      gl.disable(gl.BLEND);
      gl.activeTexture(gl.TEXTURE0);
    } else {
      // no water in view: still bind the lit buffer and restore the state the
      // selection/held draws (and next frame's shadow pass) expect after water
      this.gbuffer.bindLit();
      gl.enable(gl.DEPTH_TEST);
      gl.depthMask(true);
      gl.disable(gl.BLEND);
    }

    if (selection) this.drawSelection(camera, selection);
    if (heldItem) this.drawHeld(world, camera, heldItem, sky);

    // ================= Pass 2c: god-rays (half-res, into an aux target) =======
    const gr = this.renderGodrays(camera, sky);

    // ================= Pass 3: present to screen =================
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, W, H);
    gl.disable(gl.DEPTH_TEST);
    gl.depthMask(false);
    gl.disable(gl.BLEND);
    gl.useProgram(this.present);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, gb.lit);
    gl.uniform1i(this.present.uniform("uTex"), 0);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, gr ? gr.tex : gb.lit);
    gl.uniform1i(this.present.uniform("uGodray"), 1);
    gl.uniform1f(this.present.uniform("uGodrayEnable"), gr ? 1 : 0);
    // underwater post: murk fog needs the scene depth (safe to sample now — we're
    // bound to the default framebuffer, not the FBO that owns this texture).
    gl.activeTexture(gl.TEXTURE2);
    gl.bindTexture(gl.TEXTURE_2D, gb.depth);
    gl.uniform1i(this.present.uniform("uDepth"), 2);
    gl.uniform1f(this.present.uniform("uUnderwater"), underwater);
    gl.uniform1f(this.present.uniform("uTime"), time);
    gl.uniform1f(this.present.uniform("uNear"), camera.near);
    gl.uniform1f(this.present.uniform("uFar"), camera.far);
    gl.uniform3f(this.present.uniform("uWaterTint"), 0.10, 0.26, 0.36);
    gl.bindVertexArray(this.fsVAO);
    gl.drawArrays(gl.TRIANGLES, 0, 3);

    gl.bindVertexArray(null);
    gl.activeTexture(gl.TEXTURE0);
  }

  drawSelection(camera, sel) {
    const gl = this.gl;
    const x = sel.x, y = sel.y, z = sel.z, e = 0.002;
    const x0 = x - e, y0 = y - e, z0 = z - e, x1 = x + 1 + e, y1 = y + 1 + e, z1 = z + 1 + e;
    const a = this._selArr;   // reused: 12 edges * 2 verts * 3 floats
    let i = 0;
    const P = (px, py, pz) => { a[i++] = px; a[i++] = py; a[i++] = pz; };
    P(x0,y0,z0); P(x1,y0,z0); P(x1,y0,z0); P(x1,y0,z1); P(x1,y0,z1); P(x0,y0,z1); P(x0,y0,z1); P(x0,y0,z0);
    P(x0,y1,z0); P(x1,y1,z0); P(x1,y1,z0); P(x1,y1,z1); P(x1,y1,z1); P(x0,y1,z1); P(x0,y1,z1); P(x0,y1,z0);
    P(x0,y0,z0); P(x0,y1,z0); P(x1,y0,z0); P(x1,y1,z0); P(x1,y0,z1); P(x1,y1,z1); P(x0,y0,z1); P(x0,y1,z1);
    gl.useProgram(this.line);
    gl.uniformMatrix4fv(this.line.uniform("uViewProj"), false, camera.viewProj);
    gl.uniform4f(this.line.uniform("uColor"), 0, 0, 0, 0.5);
    gl.bindVertexArray(this.lineVAO);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.lineVBO);
    gl.bufferData(gl.ARRAY_BUFFER, a, gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
    gl.drawArrays(gl.LINES, 0, 24);
  }

  // The first-person hand, drawn last into the lit buffer through its own
  // close-up projection so a held item never clips into the world.
  //
  // It gets a scratch depth buffer of its own (gbuffer.bindHeld): the scene's
  // depth is in a different projection and is still needed by later passes, but
  // without SOME depth test a model just draws in emission order and its back
  // faces paint over its front ones — the old "held block has no top face".
  // The pose and every animation come from render/viewmodel.js as one matrix.
  drawHeld(world, camera, itemKey, sky) {
    const gl = this.gl;
    const it = getItem(itemKey);
    const mesh = this.entityRenderer.itemMeshes.get(itemKey);
    if (!mesh) return;

    this.viewmodel.modelMatrix(this._heldModel, it, mesh,
      (camera.fov * Math.PI) / 180, gl.canvas.width / gl.canvas.height);
    mat4.perspective(this.heldProj, (camera.fov * Math.PI) / 180,
      gl.canvas.width / gl.canvas.height, 0.01, 10);
    mat4.multiply(this.heldMVP, this.heldProj, this._heldModel);

    // Light the item by where the player is standing, so it dims at night and
    // goes near-black in an unlit cave. Skylight keeps a moonlight floor, the
    // same idea as the terrain's night ambient — a hand outdoors at midnight is
    // dim, not invisible.
    const cx = Math.floor(camera.pos[0]), cy = Math.floor(camera.pos[1]), cz = Math.floor(camera.pos[2]);
    const light = Math.max(world.getBlockLight(cx, cy, cz) / 15,
      (world.getSky(cx, cy, cz) / 15) * Math.max(0.10, sky.dayFactor()));

    this.gbuffer.bindHeld();
    const p = this.viewmodelProg;
    gl.useProgram(p);
    gl.enable(gl.DEPTH_TEST);
    gl.depthMask(true);
    gl.disable(gl.CULL_FACE);          // mirrored styles reverse the winding
    gl.disable(gl.BLEND);
    gl.uniformMatrix4fv(p.uniform("uMVP"), false, this.heldMVP);
    gl.uniform1f(p.uniform("uLight"), light);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.atlas.texture);
    gl.uniform1i(p.uniform("uAtlas"), 0);
    gl.bindVertexArray(mesh.vao);
    gl.drawArrays(gl.TRIANGLES, 0, mesh.count);
    gl.bindVertexArray(null);
    this.gbuffer.bindLit();            // leave the scene target bound as found
  }
}
