// Golden-vector dumper for the C++ port's determinism gate.
//
// Runs the *original* JavaScript (../../js) and writes a plain-text dump of every
// value the world generator depends on. The C++ build writes the same format with
// `Hollowreach --dump-golden`, and the two are compared with a plain diff.
//
//   node tools/gen_golden.mjs                  > js.txt
//   node tools/gen_golden.mjs --sections prng  > js-prng.txt
//
// Doubles are printed as their raw IEEE-754 bits in hex. Decimal formatting would
// hide exactly the sub-ulp differences this exists to catch, and "0.30000000000000004"
// vs "0.3" is not a useful diff either way.
//
// The sections are ordered by how much they narrow down a failure:
//   prng     — if this differs, nothing else can match. Check imul/ushr first.
//   noise    — the permutation tables. A single differing swap means the
//              Fisher-Yates loop was transcribed wrong, and every later value is
//              unrelated rather than merely close.
//   fields   — noise2/noise3/fbm samples. Differences here with matching perm
//              tables point at floating-point contraction or expression folding.
//   worldgen — heights, biomes, ravines, ores. Differences here with matching
//              fields point at an integer-semantics bug (floorDiv is the usual one).
//   chunks   — whole generated chunks. The end-to-end check.

// makeLoader goes through pathToFileURL, which handles the spaces and the "C++"
// in this repository's path.
import { findJsDir, makeLoader } from "./jsref.mjs";

const JS_DIR = findJsDir(import.meta.url);
const load = makeLoader(JS_DIR);

const { hashSeed, mulberry32, hash2i, hash3i } = await load("core/prng.js");
const { Noise } = await load("world/noise.js");
const worldgen = await load("world/worldgen.js");
const { BLOCKS, BLOCK } = await load("world/blocks.js");
const { CX, CZ, WH, localIdx } = await load("world/chunk.js");

// --- formatting -------------------------------------------------------------

const scratch = new DataView(new ArrayBuffer(8));

// Exact double as 16 hex digits.
function d(value) {
  scratch.setFloat64(0, value);
  let out = "";
  for (let i = 0; i < 8; i++) out += scratch.getUint8(i).toString(16).padStart(2, "0");
  return out;
}

const u32 = (value) => (value >>> 0).toString(16).padStart(8, "0");

const out = [];
const emit = (line) => out.push(line);
const section = (name) => emit(`\n## ${name}`);

// FNV-1a over bytes, so a whole chunk collapses to one comparable line.
function fnv1a(bytes) {
  let h = 0x811c9dc5;
  for (let i = 0; i < bytes.length; i++) {
    h ^= bytes[i];
    h = Math.imul(h, 0x01000193);
  }
  return h >>> 0;
}

// --- the fixture ------------------------------------------------------------
// The first two are the seeds of the worlds actually in worlds/, and both exceed
// 2^31 — which is the case that catches a signed/unsigned slip in the hashes.
const SEEDS = [3479357960, 3918175327, 1, 12345, 4294967295];
const SEED_STRINGS = ["", "a", "turf_top", "greystone", "papyrus_stem", "item:pick_copper", "0"];
const CHUNKS = [
  [0, 0], [1, 0], [0, 1], [-1, -1], [7, -3], [-12, 25], [100, 100],
];

const args = process.argv.slice(2);
const sectionsArg = args.includes("--sections")
  ? args[args.indexOf("--sections") + 1].split(",")
  : null;
const want = (name) => !sectionsArg || sectionsArg.includes(name);

emit(`# hollowreach golden vectors v1`);
emit(`# source: javascript (js/)`);
emit(`# GEN_VERSION=${worldgen.GEN_VERSION} SEA_LEVEL=${worldgen.SEA_LEVEL}`);
emit(`# blocks=${BLOCKS.length} CX=${CX} CZ=${CZ} WH=${WH}`);

// --- prng -------------------------------------------------------------------
if (want("prng")) {
  section("prng.hashSeed");
  for (const s of SEED_STRINGS) emit(`hashSeed("${s}") = ${u32(hashSeed(s))}`);

  section("prng.mulberry32");
  for (const seed of SEEDS) {
    const rng = mulberry32(seed >>> 0);
    const values = [];
    for (let i = 0; i < 8; i++) values.push(d(rng()));
    emit(`mulberry32(${seed}) = ${values.join(" ")}`);
  }

  section("prng.hash2i");
  for (const seed of SEEDS) {
    for (const [x, y] of [[0, 0], [1, 0], [-1, 0], [0, -1], [-2147483648, 2147483647], [12345, -6789]]) {
      emit(`hash2i(${seed}, ${x}, ${y}) = ${d(hash2i(seed >>> 0, x, y))}`);
    }
  }

  section("prng.hash3i");
  for (const seed of SEEDS) {
    for (const [x, y, z] of [[0, 0, 0], [1, 2, 3], [-1, -2, -3], [999999, -50, 12]]) {
      emit(`hash3i(${seed}, ${x}, ${y}, ${z}) = ${d(hash3i(seed >>> 0, x, y, z))}`);
    }
  }
}

// --- noise permutation tables ----------------------------------------------
// The twelve salts are the ones worldgen's ensure() uses (js/world/worldgen.js:22-42).
const SALTS = [
  ["terrain", 0], ["hills", 0x9e37], ["cave", 0x1b3f], ["cave2", 0x77d1],
  ["ore", 0x2c91], ["stonevar", 0x5a17], ["flora", 0x0f10], ["temp", 0x3c5a],
  ["moist", 0x66b1], ["mount", 0x14e9], ["ridge", 0x7f23], ["ravine", 0x2ba7],
];

if (want("noise")) {
  section("noise.perm");
  for (const seed of SEEDS) {
    for (const [name, salt] of SALTS) {
      const n = new Noise((seed ^ salt) >>> 0);
      // The whole 512-entry table hashed, plus the first 16 raw so a mismatch
      // shows *how* it differs rather than merely that it does.
      emit(`perm(${seed},${name}) = ${u32(fnv1a(n.perm))} [${[...n.perm.slice(0, 16)].join(",")}]`);
    }
  }
}

// --- noise field samples ----------------------------------------------------
if (want("fields")) {
  section("noise.samples");
  for (const seed of [3918175327, 12345]) {
    const n = new Noise(seed >>> 0);
    for (const [x, y] of [[0, 0], [0.5, 0.5], [1.5, 2.5], [-3.25, 7.125], [1234.5678, -8765.4321]]) {
      emit(`noise2(${seed}; ${x}, ${y}) = ${d(n.noise2(x, y))}`);
    }
    for (const [x, y, z] of [[0, 0, 0], [0.5, 0.5, 0.5], [-3.25, 7.125, 0.75], [123.5, 45.25, -67.125]]) {
      emit(`noise3(${seed}; ${x}, ${y}, ${z}) = ${d(n.noise3(x, y, z))}`);
    }
    for (const oct of [1, 2, 3, 4]) {
      emit(`fbm2(${seed}; 12.34, -56.78, ${oct}) = ${d(n.fbm2(12.34, -56.78, oct))}`);
      emit(`fbm3(${seed}; 1.5, 2.5, 3.5, ${oct}) = ${d(n.fbm3(1.5, 2.5, 3.5, oct))}`);
    }
  }
}

// --- worldgen 2D fields -----------------------------------------------------
if (want("worldgen")) {
  section("worldgen.heightAt");
  for (const seed of SEEDS) {
    for (const ver of [1, 2]) {
      // A coarse grid wide enough to cross biome and mountain-mask boundaries.
      const heights = [];
      for (let i = 0; i < 32; i++) {
        const wx = (i - 16) * 137;
        const wz = (i * 89) - 800;
        heights.push(worldgen.heightAt(seed >>> 0, wx, wz, ver));
      }
      emit(`heightAt(${seed}, v${ver}) = ${heights.join(",")}`);
    }
  }

  section("worldgen.biomeAt");
  for (const seed of SEEDS) {
    // Histogram over a wide area: the thresholds in biomeOf are tuned to the
    // field distribution, so counts drifting means the fields drifted.
    const counts = [0, 0, 0, 0, 0];
    for (let x = -2000; x <= 2000; x += 53) {
      for (let z = -2000; z <= 2000; z += 53) {
        counts[worldgen.biomeAt(seed >>> 0, x, z, 2)]++;
      }
    }
    emit(`biomeHist(${seed}) = ${counts.join(",")}`);
  }

  section("worldgen.surfacePreview");
  for (const seed of [3479357960, 3918175327]) {
    for (const ver of [1, 2]) {
      const cells = [];
      for (let i = 0; i < 24; i++) {
        const p = worldgen.surfacePreview(seed >>> 0, i * 61 - 700, i * -47 + 300, ver);
        cells.push(`${p.key}@${p.h}`);
      }
      emit(`surfacePreview(${seed}, v${ver}) = ${cells.join(" ")}`);
    }
  }
}

// --- whole chunks -----------------------------------------------------------
if (want("chunks")) {
  section("worldgen.generate");
  // Both an id hash and a key hash. The id hash also proves the block table is
  // ordered identically; if only the key hash matches, the ids drifted but the
  // terrain is right.
  const keyOf = BLOCKS.map((b) => b.key);
  for (const seed of [3479357960, 3918175327, 12345]) {
    for (const ver of [1, 2]) {
      for (const [cx, cz] of CHUNKS) {
        const chunk = { cx, cz, voxels: new Uint16Array(CX * WH * CZ) };
        worldgen.generate(chunk, seed >>> 0, ver);

        const idBytes = new Uint8Array(chunk.voxels.buffer);
        const keyBytes = [];
        let nonAir = 0;
        for (let i = 0; i < chunk.voxels.length; i++) {
          const id = chunk.voxels[i];
          if (id !== BLOCK.air) nonAir++;
          const key = keyOf[id] ?? "?";
          for (let k = 0; k < key.length; k++) keyBytes.push(key.charCodeAt(k) & 255);
          keyBytes.push(0);
        }
        emit(
          `chunk(${seed}, v${ver}, ${cx}, ${cz}) ids=${u32(fnv1a(idBytes))} ` +
            `keys=${u32(fnv1a(keyBytes))} nonAir=${nonAir}`
        );
      }
    }
  }
}

process.stdout.write(out.join("\n") + "\n");
