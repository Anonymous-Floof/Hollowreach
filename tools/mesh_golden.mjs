// Mesher + lighting golden vectors from the original JavaScript.
//
//   node tools/mesh_golden.mjs > js-mesh.txt
//
// Compared against `Hollowreach --dump-golden --sections mesh`.
//
// What is hashed, and why not everything: position, shade, skylight, blocklight and
// the wave flag. Deliberately NOT the atlas UVs — the two builds lay their atlases
// out differently on purpose (the native one is power-of-two with gutters so mip
// chains and higher-resolution packs work), so UVs are expected to differ. Which
// texture each face uses is covered by atlas_golden.mjs instead.
//
// That still pins down everything hard about the mesher: face culling, the
// four-level ambient occlusion, smooth-light corner averaging, water surface
// heights, the deterministic plant jitter, and the shaped-block box geometry.

import { findJsDir, makeLoader } from "./jsref.mjs";

const JS_DIR = findJsDir(import.meta.url);
const load = makeLoader(JS_DIR);

const { CX, CZ, WH, chunkKey } = await load("world/chunk.js");
const worldgen = await load("world/worldgen.js");
const { computeLight } = await load("world/lighting.js");
const { meshChunk } = await load("world/mesher.js");

// The mesher only calls atlas.uvForName. Since UVs are not hashed, a stub that
// returns a fixed rect avoids needing the real atlas (and its canvas shim).
const atlasStub = { uvForName: () => [0, 0, 1, 1] };

function makeChunk(cx, cz) {
  return {
    cx,
    cz,
    voxels: new Uint16Array(CX * WH * CZ),
    meta: new Uint8Array(CX * WH * CZ),
    skylight: new Uint8Array(CX * WH * CZ),
    blocklight: new Uint8Array(CX * WH * CZ),
    emitters: [],
  };
}

function fnv1a(bytes) {
  let h = 0x811c9dc5;
  for (let i = 0; i < bytes.length; i++) {
    h ^= bytes[i];
    h = Math.imul(h, 0x01000193);
  }
  return h >>> 0;
}

const u32 = (v) => (v >>> 0).toString(16).padStart(8, "0");

// The vertex layout the C++ side hashes: positions as int32 at 1/16 of a block,
// then four bytes of quantised shade / sky / block / wave.
//
// Positions are quantised rather than hashed as raw float32 bits on purpose. Plant
// billboards are placed with Math.cos/Math.sin, and neither is bit-specified across
// implementations: MSVC's differ from V8's by one ULP on a handful of angles, which
// moves roughly six vertices per chunk. That is invisible (1e-7 of a block near the
// origin), is local render geometry rather than saved or networked state, and is the
// same class of divergence as Math.pow.
//
// The grid is 1/16 because a float32 ULP grows with magnitude — 3e-5 at 400 blocks
// out — so a fine grid is not merely unnecessary but actively flaky: at 1/1024 a
// one-ULP vertex has a ~1.5% chance of landing the other side of a boundary, which
// showed up as two of thirty chunks failing for no real reason. 1/16 still catches
// every real bug: the smallest geometry offset in the whole mesher is a ladder's
// 0.1-block thickness, and a culling, winding or AO mistake moves a vertex by half
// a block or more.
//
// If this ever does report a difference, dump the vertices (--verts on this script,
// --sections meshverts natively) and check whether it is a single one-ULP position
// with everything else identical. That is the benign case; anything else is a bug.
function hashVertices(data) {
  const bytes = new Uint8Array((data.length / 9) * 16);
  const view = new DataView(bytes.buffer);
  let o = 0;
  const q = (v) => {
    const c = v <= 0 ? 0 : v >= 1 ? 1 : v;
    return Math.round(c * 255);
  };
  for (let i = 0; i < data.length; i += 9) {
    view.setInt32(o, Math.round(data[i] * 16), true);
    view.setInt32(o + 4, Math.round(data[i + 1] * 16), true);
    view.setInt32(o + 8, Math.round(data[i + 2] * 16), true);
    bytes[o + 12] = q(data[i + 5]); // shade
    bytes[o + 13] = q(data[i + 6]); // sky
    bytes[o + 14] = q(data[i + 7]); // block
    bytes[o + 15] = data[i + 8] & 3; // wave
    o += 16;
  }
  return fnv1a(bytes);
}

// Builds and lights a 3x3 neighbourhood and meshes the centre.
function meshOne(seed, ver, cx, cz) {
  const chunks = new Map();
  for (let dz = -1; dz <= 1; dz++) {
    for (let dx = -1; dx <= 1; dx++) {
      const c = makeChunk(cx + dx, cz + dz);
      worldgen.generate(c, seed >>> 0, ver);
      chunks.set(chunkKey(c.cx, c.cz), c);
    }
  }
  const world = { chunks };
  for (const c of chunks.values()) computeLight(c, world);
  const centre = chunks.get(chunkKey(cx, cz));
  return { centre, ...meshChunk(centre, world, atlasStub) };
}

// --verts <seed> <ver> <cx> <cz>: one line per vertex, for locating the first
// disagreement with the native build when the chunk hashes differ.
if (process.argv.includes("--verts")) {
  const i = process.argv.indexOf("--verts");
  const [seed, ver, cx, cz] = process.argv.slice(i + 1, i + 5).map(Number);
  const { opaque } = meshOne(seed, ver, cx, cz);
  const q = (v) => {
    const c = v <= 0 ? 0 : v >= 1 ? 1 : v;
    return Math.round(c * 255);
  };
  // Raw float32 bits, not decimals: a one-ULP position difference is exactly what
  // a hash mismatch with visually identical output means, and %.4f hides it.
  const bits = new DataView(new ArrayBuffer(4));
  const fx = (v) => {
    bits.setFloat32(0, v);
    return bits.getUint32(0).toString(16).padStart(8, "0");
  };
  const lines = [];
  for (let k = 0, n = 0; k < opaque.length; k += 9, n++) {
    lines.push(
      `${n} ${fx(opaque[k])} ${fx(opaque[k + 1])} ${fx(opaque[k + 2])} ` +
        `${q(opaque[k + 5])} ${q(opaque[k + 6])} ${q(opaque[k + 7])} ${opaque[k + 8] & 3}`
    );
  }
  process.stdout.write(lines.join("\n") + "\n");
  process.exit(0);
}

const SEEDS = [3479357960, 3918175327, 12345];
const CHUNKS = [
  [0, 0], [1, 0], [-1, -1], [7, -3], [-12, 25],
];

const out = [
  "# hollowreach mesh vectors v1",
  "# source: javascript (js/)",
  "",
  "## mesher.chunks",
];

for (const seed of SEEDS) {
  for (const ver of [1, 2]) {
    for (const [cx, cz] of CHUNKS) {
      // A 3x3 neighbourhood, generated and lit, so border faces and the smooth
      // light at chunk edges are correct rather than approximated.
      const chunks = new Map();
      for (let dz = -1; dz <= 1; dz++) {
        for (let dx = -1; dx <= 1; dx++) {
          const c = makeChunk(cx + dx, cz + dz);
          worldgen.generate(c, seed >>> 0, ver);
          chunks.set(chunkKey(c.cx, c.cz), c);
        }
      }
      const world = { chunks };
      // Light every chunk before meshing, matching how the streamer orders it.
      for (const c of chunks.values()) computeLight(c, world);

      const centre = chunks.get(chunkKey(cx, cz));
      const { opaque, water } = meshChunk(centre, world, atlasStub);

      // Light hashes too: a mesh difference with matching light hashes points at
      // the mesher, and vice versa.
      const skyHash = fnv1a(centre.skylight);
      const blkHash = fnv1a(centre.blocklight);

      out.push(
        `mesh(${seed}, v${ver}, ${cx}, ${cz}) ` +
          `opaqueVerts=${opaque.length / 9} waterVerts=${water.length / 9} ` +
          `opaque=${u32(hashVertices(opaque))} water=${u32(hashVertices(water))} ` +
          `sky=${u32(skyHash)} blk=${u32(blkHash)} emitters=${centre.emitters.length}`
      );
    }
  }
}

process.stdout.write(out.join("\n") + "\n");
