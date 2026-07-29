// The crafting tables of the *original*, for diffing against the C++ port's
// `--dump-recipes`.
//
//   node tools/recipes_golden.mjs > js-recipes.txt
//
// Recipes, smelting and fuel are the one part of the game with no visual signal at
// all: a mistyped pattern or a swapped output count looks exactly like a correct
// one until a player cannot craft something. Most of the table is generated from
// the block registry too, so a single wrong loop silently drops a whole family.
//
// Three sections:
//   recipes   every recipe in order, canonicalised. Order matters — matching is
//             first-match-wins, so two tables with the same *set* of recipes can
//             still behave differently.
//   smelting  the forge table.
//   fuel      the derived burn time of every item. This exercises the recursive
//             derivation in crafting.fuelValue, which is where "a wooden shovel
//             burns, a stone one does not" actually comes from.
//   craft     a fixed set of grids run through matchGrid, covering shapeless,
//             shaped, ingredient tags, pattern translation and station gating.

import { findJsDir, makeLoader } from "./jsref.mjs";

const JS_DIR = findJsDir(import.meta.url);
const load = makeLoader(JS_DIR);

const { RECIPES, SMELTING } = await load("game/recipes.js");
const { matchGrid, fuelValue } = await load("game/crafting.js");
const { ITEMS } = await load("game/items.js");

const out = [];
const line = (s) => out.push(s);

// --- recipes ----------------------------------------------------------------

function canonical(r) {
  const station = r.station;
  if (r.type === "shapeless") {
    const ings = Object.entries(r.in)
      .map(([k, v]) => `${k}*${v}`)
      .sort();
    return `shapeless|${station}|${ings.join(" ")}|out=${r.out.key}*${r.out.count}`;
  }
  const rows = r.pattern;
  const h = rows.length;
  const w = Math.max(...rows.map((s) => s.length));
  const cells = [];
  for (let rr = 0; rr < h; rr++) {
    for (let cc = 0; cc < w; cc++) {
      const ch = rows[rr][cc] || " ";
      if (ch !== " " && ch !== ".") cells.push(`${rr},${cc},${r.legend[ch]}`);
    }
  }
  return `shaped|${station}|${w}x${h}|${cells.join(" ")}|out=${r.out.key}*${r.out.count}`;
}

line("# hollowreach recipes v1");
line("# source: javascript (js/)");
line(`# recipes=${RECIPES.length} smelting=${SMELTING.length}`);
line("");
line("## recipes");
RECIPES.forEach((r, i) => line(`recipe(${String(i).padStart(3, "0")}) = ${canonical(r)}`));

// --- smelting ---------------------------------------------------------------

line("");
line("## smelting");
SMELTING.forEach((s, i) =>
  line(`smelt(${String(i).padStart(2, "0")}) = ${s.in} -> ${s.out} in ${s.time}s`),
);

// --- fuel -------------------------------------------------------------------
// Queried in item-registration order, which is what pins down the order-dependent
// memo in crafting.fuelValue.

line("");
line("## fuel");
for (const key in ITEMS) {
  const v = fuelValue(key);
  if (v > 0) line(`fuel(${key}) = ${v}`);
}

// --- craft ------------------------------------------------------------------
// Each case is a grid size, a station, and cell placements. Keep this list in
// step with the same list in src/dev/golden.cpp.

const CASES = [
  ["log->planks (2x2 hand)", 2, "hand", [[0, 0, "log"]]],
  ["sticks (2x2 hand)", 2, "hand", [[0, 0, "planks"], [1, 0, "planks"]]],
  ["sticks from pine (tag)", 2, "hand", [[0, 0, "pine_planks"], [1, 0, "pine_planks"]]],
  ["sticks mixed woods (tag)", 2, "hand", [[0, 0, "planks"], [1, 0, "birch_planks"]]],
  ["workbench (2x2 hand)", 2, "hand",
    [[0, 0, "planks"], [0, 1, "planks"], [1, 0, "planks"], [1, 1, "planks"]]],
  ["torch (2x2 hand)", 2, "hand", [[0, 0, "embercoal"], [1, 0, "stick"]]],
  ["forge needs a bench (2x2)", 2, "hand",
    [[0, 0, "cobbled"], [0, 1, "cobbled"], [1, 0, "cobbled"], [1, 1, "cobbled"]]],
  ["chest (3x3 bench)", 3, "workbench",
    [[0, 0, "planks"], [0, 1, "planks"], [0, 2, "planks"],
     [1, 0, "planks"], [1, 2, "planks"],
     [2, 0, "planks"], [2, 1, "planks"], [2, 2, "planks"]]],
  ["wooden pick (3x3 bench)", 3, "workbench",
    [[0, 0, "planks"], [0, 1, "planks"], [0, 2, "planks"],
     [1, 1, "stick"], [2, 1, "stick"]]],
  ["iron chestguard (3x3 bench)", 3, "workbench",
    [[0, 0, "ferralite_ingot"], [0, 2, "ferralite_ingot"],
     [1, 0, "ferralite_ingot"], [1, 1, "ferralite_ingot"], [1, 2, "ferralite_ingot"],
     [2, 0, "ferralite_ingot"], [2, 1, "ferralite_ingot"], [2, 2, "ferralite_ingot"]]],
  ["slab row, offset in the grid", 3, "workbench",
    [[2, 0, "cobbled"], [2, 1, "cobbled"], [2, 2, "cobbled"]]],
  ["stairs (3-2-1)", 3, "workbench",
    [[0, 0, "greystone"],
     [1, 0, "greystone"], [1, 1, "greystone"],
     [2, 0, "greystone"], [2, 1, "greystone"], [2, 2, "greystone"]]],
  ["bucket (V of iron)", 3, "workbench",
    [[0, 0, "ferralite_ingot"], [0, 2, "ferralite_ingot"], [1, 1, "ferralite_ingot"]]],
  ["soul anchor (full 3x3)", 3, "workbench",
    [[0, 0, "embercoal"], [0, 1, "raw_copper"], [0, 2, "raw_ferralite"],
     [1, 0, "azurite"], [1, 1, "sparkstone"], [1, 2, "raw_sunbrass"],
     [2, 0, "verdanite"], [2, 1, "aetherite"], [2, 2, "gloamite"]]],
  ["wayshard (shapeless hand)", 2, "hand", [[0, 0, "gloamite"], [0, 1, "sparkstone"]]],
  ["slab <-> vertical slab", 2, "hand", [[0, 0, "cobbled_slab"]]],
  ["nothing (empty grid)", 3, "workbench", []],
  ["nothing (nonsense)", 2, "hand", [[0, 0, "aetherite"], [1, 1, "leather"]]],
];

line("");
line("## craft");
for (const [label, size, station, cells] of CASES) {
  const grid = new Array(size * size).fill(null);
  for (const [r, c, key] of cells) grid[r * size + c] = { key, count: 1 };
  const m = matchGrid(grid, size, station);
  line(`craft(${label}) = ${m ? `${m.out.key}*${m.out.count}` : "none"}`);
}

process.stdout.write(out.join("\n") + "\n");
