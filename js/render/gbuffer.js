// Off-screen render targets for the deferred lighting pipeline.
//
// The scene is rendered into a G-buffer (multiple render targets + a depth
// texture) instead of straight to the screen:
//   RT0 gAlbedo  : rgb = albedo,  a = baked face shade*AO
//   RT1 gLight   : r = skylight,  g = blocklight, b = emissive, a = material id
//   RT2 gNormal  : rgb = world-space normal (0..1 encoded), a = unused
//   depth        : DEPTH_COMPONENT24 texture (reconstructs world position)
//
// A fullscreen lighting/composite pass then reads these and writes a lit colour
// target (which shares the scene depth, so forward-blended water can still
// depth-test). Everything is RGBA8 + core WebGL2 — no float-buffer extensions.
//
// Material ids (gLight.a, stored as id/255): 0 opaque, 1 foliage, 2 emissive
// surface (torch), 3 entity, 4 water, 255 sky/background.

export const MAT = { OPAQUE: 0, FOLIAGE: 1, EMISSIVE: 2, ENTITY: 3, WATER: 4, SKY: 255 };

function makeColorTex(gl, w, h, filter) {
  const t = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, t);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  return t;
}

export class GBuffer {
  constructor(gl) {
    this.gl = gl;
    this.w = 0; this.h = 0;
    this.scale = 1;            // render scale (quality): internal buffers = canvas * scale
    this.sceneFBO = null;
    this.litFBO = null;        // lit colour + scene depth (forward water/selection)
    this.litColorFBO = null;   // lit colour only (composite reads scene depth as a texture)
    this.heldFBO = null;       // lit colour + a scratch depth buffer (viewmodel)
    this.gAlbedo = this.gLight = this.gNormal = this.depth = this.lit = null;
    this.heldDepth = null;
    // a pool of extra half/full screen single-target FBOs for effect passes
    this.aux = [];             // [{fbo, tex, w, h}]
  }

  // Internal buffer size (canvas size * render scale, min 1).
  bufW() { return Math.max(1, Math.round(this.w * this.scale)); }
  bufH() { return Math.max(1, Math.round(this.h * this.scale)); }

  resize(w, h, scale) {
    if (w === this.w && h === this.h && scale === this.scale && this.sceneFBO) return;
    this.w = w; this.h = h; this.scale = scale;
    const gl = this.gl;
    const bw = this.bufW(), bh = this.bufH();
    this._dispose();

    // --- scene G-buffer: 3 colour targets + depth texture ---
    this.gAlbedo = makeColorTex(gl, bw, bh, gl.NEAREST);
    this.gLight = makeColorTex(gl, bw, bh, gl.NEAREST);
    this.gNormal = makeColorTex(gl, bw, bh, gl.NEAREST);
    this.depth = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.depth);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.DEPTH_COMPONENT24, bw, bh, 0, gl.DEPTH_COMPONENT, gl.UNSIGNED_INT, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

    this.sceneFBO = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.sceneFBO);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.gAlbedo, 0);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT1, gl.TEXTURE_2D, this.gLight, 0);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT2, gl.TEXTURE_2D, this.gNormal, 0);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.TEXTURE_2D, this.depth, 0);
    gl.drawBuffers([gl.COLOR_ATTACHMENT0, gl.COLOR_ATTACHMENT1, gl.COLOR_ATTACHMENT2]);
    this._check("scene");

    // --- lit colour target. Two FBOs share the SAME colour texture: one carries
    // the scene depth (forward water/selection/held depth-test against it), one is
    // depth-less so the composite pass can sample the depth texture without a
    // read/write feedback loop. ---
    this.lit = makeColorTex(gl, bw, bh, gl.LINEAR);
    this.litFBO = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.litFBO);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.lit, 0);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.TEXTURE_2D, this.depth, 0);
    gl.drawBuffers([gl.COLOR_ATTACHMENT0]);
    this._check("lit");

    this.litColorFBO = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.litColorFBO);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.lit, 0);
    gl.drawBuffers([gl.COLOR_ATTACHMENT0]);
    this._check("litColor");

    // --- viewmodel target: the lit colour again, but with a SCRATCH depth buffer.
    // The held item is drawn through its own close-up projection, so its depths
    // mean nothing against the scene's — yet it still has to hide its own back
    // faces. A private depth buffer we can clear gives the viewmodel a real
    // depth test against itself while leaving the scene depth (which the god-ray
    // and underwater passes still read) untouched. ---
    this.heldDepth = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, this.heldDepth);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT16, bw, bh);
    gl.bindRenderbuffer(gl.RENDERBUFFER, null);
    this.heldFBO = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.heldFBO);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.lit, 0);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, this.heldDepth);
    gl.drawBuffers([gl.COLOR_ATTACHMENT0]);
    this._check("held");

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  }

  // A scratch single-target colour FBO at (full*frac) resolution, created on
  // demand and cached by slot index. Used by effect passes (godrays, SSR, blur).
  auxTarget(slot, frac, filter) {
    const gl = this.gl;
    const w = Math.max(1, Math.round(this.bufW() * frac));
    const h = Math.max(1, Math.round(this.bufH() * frac));
    let a = this.aux[slot];
    if (a && a.w === w && a.h === h) return a;
    if (a) { gl.deleteFramebuffer(a.fbo); gl.deleteTexture(a.tex); }
    const tex = makeColorTex(gl, w, h, filter || gl.LINEAR);
    const fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    a = this.aux[slot] = { fbo, tex, w, h };
    return a;
  }

  bindScene() {
    const gl = this.gl;
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.sceneFBO);
    gl.viewport(0, 0, this.bufW(), this.bufH());
  }
  bindLit() {
    const gl = this.gl;
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.litFBO);
    gl.viewport(0, 0, this.bufW(), this.bufH());
  }
  bindLitColor() {
    const gl = this.gl;
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.litColorFBO);
    gl.viewport(0, 0, this.bufW(), this.bufH());
  }
  // The viewmodel target, with its scratch depth cleared and ready to test.
  bindHeld() {
    const gl = this.gl;
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.heldFBO);
    gl.viewport(0, 0, this.bufW(), this.bufH());
    gl.depthMask(true);
    gl.clear(gl.DEPTH_BUFFER_BIT);
  }

  _check(label) {
    const gl = this.gl;
    const s = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
    if (s !== gl.FRAMEBUFFER_COMPLETE) console.error(`GBuffer ${label} incomplete: 0x${s.toString(16)}`);
  }

  _dispose() {
    const gl = this.gl;
    for (const t of [this.gAlbedo, this.gLight, this.gNormal, this.depth, this.lit]) if (t) gl.deleteTexture(t);
    if (this.heldDepth) gl.deleteRenderbuffer(this.heldDepth);
    if (this.sceneFBO) gl.deleteFramebuffer(this.sceneFBO);
    if (this.litFBO) gl.deleteFramebuffer(this.litFBO);
    if (this.litColorFBO) gl.deleteFramebuffer(this.litColorFBO);
    if (this.heldFBO) gl.deleteFramebuffer(this.heldFBO);
    this.sceneFBO = this.litFBO = this.litColorFBO = this.heldFBO = null;
    this.heldDepth = null;
  }

  dispose() {
    this._dispose();
    const gl = this.gl;
    for (const a of this.aux) if (a) { gl.deleteFramebuffer(a.fbo); gl.deleteTexture(a.tex); }
    this.aux = [];
  }
}
