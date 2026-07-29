// Per-tile hashes of the *original* atlas, for diffing against the C++ port's
// `--dump-atlas` sidecar.
//
//   node tools/atlas_golden.mjs > js-tiles.txt
//
// The atlas layout deliberately differs between the two builds — the native one is
// power-of-two with gutters so a mip chain and a higher-resolution resource pack
// both work — so the images cannot be compared directly. Per-tile hashes compare
// what actually matters: whether each painter draws the same pixels.
//
// js/render/texatlas.js paints with Canvas2D, which needs a canvas. Rather than
// pull in a headless-canvas dependency, this file provides a tiny shim
// implementing only the three calls the painters use: fillStyle, fillRect and
// imageSmoothingEnabled. That is enough because every painter draws 1x1 rects.

import { findJsDir, makeLoader } from "./jsref.mjs";

const JS_DIR = findJsDir(import.meta.url);
const load = makeLoader(JS_DIR);

// --- Canvas2D shim ----------------------------------------------------------

class FakeContext {
  constructor(width, height) {
    this.width = width;
    this.height = height;
    // RGBA8, top-left origin, starting fully transparent — same as a real canvas.
    this.data = new Uint8Array(width * height * 4);
    this._fill = [0, 0, 0, 255];
    this.imageSmoothingEnabled = true;
  }

  set fillStyle(value) {
    this._fill = parseColor(value);
  }
  get fillStyle() {
    return this._fill;
  }

  fillRect(x, y, w, h) {
    const [r, g, b, a] = this._fill;
    const x0 = Math.max(0, Math.round(x));
    const y0 = Math.max(0, Math.round(y));
    const x1 = Math.min(this.width, Math.round(x + w));
    const y1 = Math.min(this.height, Math.round(y + h));
    for (let yy = y0; yy < y1; yy++) {
      for (let xx = x0; xx < x1; xx++) {
        const i = (yy * this.width + xx) * 4;
        // Source-over onto a transparent-or-opaque destination. Every painter
        // writes each pixel at most once per layer and only two use a fractional
        // alpha, both onto empty pixels, so this reduces to a plain replace in
        // practice — but the general form is here so a future painter that does
        // layer translucency still matches the browser.
        if (a >= 255) {
          this.data[i] = r;
          this.data[i + 1] = g;
          this.data[i + 2] = b;
          this.data[i + 3] = 255;
          continue;
        }
        const sa = a / 255;
        const da = this.data[i + 3] / 255;
        const outA = sa + da * (1 - sa);
        if (outA <= 0) {
          this.data[i] = this.data[i + 1] = this.data[i + 2] = this.data[i + 3] = 0;
          continue;
        }
        this.data[i] = Math.round((r * sa + this.data[i] * da * (1 - sa)) / outA);
        this.data[i + 1] = Math.round((g * sa + this.data[i + 1] * da * (1 - sa)) / outA);
        this.data[i + 2] = Math.round((b * sa + this.data[i + 2] * da * (1 - sa)) / outA);
        this.data[i + 3] = Math.round(outA * 255);
      }
    }
  }
}

function parseColor(value) {
  if (Array.isArray(value)) return value;
  const text = String(value).trim();

  // The item sprite grids in js/game/items.js hold plain hex literals, and
  // buildAtlas blits those tiles alongside the block painters.
  if (text.startsWith("#")) {
    const hex = text.slice(1);
    if (hex.length === 3) {
      return [
        parseInt(hex[0] + hex[0], 16),
        parseInt(hex[1] + hex[1], 16),
        parseInt(hex[2] + hex[2], 16),
        255,
      ];
    }
    const n = parseInt(hex, 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255, 255];
  }

  const m = /^rgba?\(([^)]+)\)$/.exec(text);
  if (!m) throw new Error(`shim: unsupported fillStyle ${value}`);
  const parts = m[1].split(",").map((s) => s.trim());
  const r = Number(parts[0]);
  const g = Number(parts[1]);
  const b = Number(parts[2]);
  const a = parts.length > 3 ? Number(parts[3]) : 1;
  return [r, g, b, Math.round(a * 255)];
}

// --- run the painters -------------------------------------------------------
// PAINTERS is module-private, so the tiles are obtained the way the game does:
// by calling buildAtlas with a document shim, then reading tiles back out.

const TILE = 16;

globalThis.document = {
  createElement(tag) {
    if (tag !== "canvas") throw new Error(`shim: createElement(${tag})`);
    const canvas = {
      width: 0,
      height: 0,
      _ctx: null,
      getContext() {
        if (!this._ctx) this._ctx = new FakeContext(this.width, this.height);
        return this._ctx;
      },
    };
    return canvas;
  },
};

// buildAtlas uploads to GL at the end; a stub that records nothing is enough.
const glStub = {
  TEXTURE_2D: 1, RGBA: 2, UNSIGNED_BYTE: 3, TEXTURE_MIN_FILTER: 4,
  TEXTURE_MAG_FILTER: 5, NEAREST: 6, TEXTURE_WRAP_S: 7, TEXTURE_WRAP_T: 8,
  CLAMP_TO_EDGE: 9,
  createTexture: () => ({}),
  bindTexture() {},
  texImage2D() {},
  texParameteri() {},
};

const { buildAtlas } = await load("render/texatlas.js");
const atlas = buildAtlas(glStub);
const ctx = atlas.canvas.getContext("2d");

function fnv1a(bytes) {
  let h = 0x811c9dc5;
  for (let i = 0; i < bytes.length; i++) {
    h ^= bytes[i];
    h = Math.imul(h, 0x01000193);
  }
  return h >>> 0;
}

// Enumerate the same ids the native build puts in its atlas: every texture the
// block table references, plus one sprite tile per non-block item.
//
// The item sprites are the point of this half of the diff. They are ~250 lines of
// hand-placed texels in js/game/items.js, and they are the *only* source of an
// item's appearance — icon, dropped model and held model all come from the same
// grid — so a single transposed coordinate changes three things at once and none
// of them obviously.
const { BLOCKS } = await load("world/blocks.js");
const names = new Map();  // atlas tile name -> namespaced id
for (const b of BLOCKS) {
  if (!b.tex) continue;
  for (const v of Object.values(b.tex)) names.set(v, `block/${v}`);
}
const { ITEMS } = await load("game/items.js");
for (const key in ITEMS) {
  if (ITEMS[key].iconKind === "block") continue;
  names.set(`item:${key}`, `item/${key}`);
}

const rows = [];
for (const [name, id] of names) {
  const [rx, ry, rw, rh] = atlas.pixelRect(name);
  const pixels = new Uint8Array(rw * rh * 4);
  for (let y = 0; y < rh; y++) {
    for (let x = 0; x < rw; x++) {
      const src = ((ry + y) * ctx.width + (rx + x)) * 4;
      const dst = (y * rw + x) * 4;
      pixels[dst] = ctx.data[src];
      pixels[dst + 1] = ctx.data[src + 1];
      pixels[dst + 2] = ctx.data[src + 2];
      pixels[dst + 3] = ctx.data[src + 3];
    }
  }
  rows.push([id, fnv1a(pixels).toString(16).padStart(8, "0")]);
}
rows.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));

const out = [
  "# hollowreach atlas tiles v1",
  "# source: javascript (js/)",
  `# tileRes=${TILE} tiles=${rows.length}`,
  "",
  "## atlas.tiles",
];
for (const [name, hash] of rows) out.push(`tile(${name}) = ${hash}`);
process.stdout.write(out.join("\n") + "\n");
