// The determinism gate.
//
// Writes the same plain-text dump as tools/gen_golden.mjs does from the
// original JavaScript, so `compare_golden.py` can diff them. This is the first
// thing to run after touching anything under world/ or core/prng: a mismatch here
// means the terrain has changed, which in multiplayer means a guest and the host
// no longer generate the same world.
//
// Runs headless — no window, no GL — so it works in a console build and could run
// in CI.

#pragma once

#include <string>

namespace hr::dev {

// `sections` is a comma-separated subset of prng,noise,fields,worldgen,chunks, or
// empty for all of them. Writes to `path`, or stdout when it is "-".
bool dumpGolden(const std::string& path, const std::string& sections);

// Builds the texture atlas headlessly and writes it as a PNG, plus a sidecar text
// file listing each tile's position and a hash of its pixels. Diffing that sidecar
// against the browser's atlas is how the ported painters are checked; the PNG is
// for looking at when one of them is wrong.
bool dumpAtlas(const std::string& pngPath, int maxTileResolution, bool mipmaps);

// Writes the crafting tables: every recipe in order, the smelting table, the
// derived fuel value of every item, and the result of a fixed set of grids run
// through the matcher. These are the one part of the game with no visual signal —
// a mistyped pattern or a swapped output count looks exactly like a correct one
// until a player cannot craft something.
bool dumpRecipes(const std::string& path);

}  // namespace hr::dev
