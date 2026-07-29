// Headless behaviour checks for the gameplay layer.
//
// The determinism harness (dev/golden.h) proves the ported *data* matches the
// original — terrain, textures, recipes. It cannot say anything about behaviour:
// whether holding the mouse actually breaks a block, whether the drop lands in the
// bag, whether a slab placed against a ceiling comes out upside down. Those are
// the parts of M5 with no reference dump to diff against, so they get asserted
// instead.
//
// Everything here runs without a window: World only touches GL when it has a tile
// table, and Input exposes the same feed methods the platform layer uses, so a
// scripted click is indistinguishable from a real one.

#pragma once

namespace hr::dev {

// Runs every check and prints a line per case. Returns 0 when all pass.
int runSelfTest();

}  // namespace hr::dev
