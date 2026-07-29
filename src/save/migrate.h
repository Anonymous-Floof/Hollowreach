// Save migration, ported from js/save/migrate.js.
//
// The web build's shape: MIGRATIONS[n] upgrades a save from version n to n+1, and a
// load runs every migration from the file's version up to the current one. The same
// idea works here with one difference that comes from the format being binary
// rather than JSON.
//
// In JSON a missing field reads as `undefined` and a migration exists to give it a
// value. In a binary stream there is no "missing" — a decoder that reads a field
// the file does not contain reads whatever came next. So a version bump is handled
// in two places, and both are needed:
//
//   1. The decoder guards the new reads:  `if (version >= 2) s.foo = r.f32();`
//      This is what keeps an old file parseable at all.
//   2. A migration fills in what the old file could not say:
//      `MIGRATIONS[1] = [](WorldSave& s) { s.player.stamina = 20.0f; };`
//      This is what makes the loaded world *correct* rather than merely parsed.
//
// Adding a whole new section needs neither: an old file simply lacks the tag, and
// the decoder leaves that part of WorldSave at its defaults. That is the reason the
// payload is a tagged section list, and it is why kSaveVersion should only move when
// an existing section's layout changes.
//
// The web build warned and loaded anyway when a save came from a newer build. That
// is safe for JSON, where an unknown field is simply ignored, and unsafe here: a
// section whose layout changed would be misparsed into plausible-looking nonsense.
// A newer file is refused with a clear message instead. That is a deliberate
// divergence, and the only one in this file.

#pragma once

#include <cstdint>

#include "save/format.h"

namespace hr::save {

// Upgrades `save` in place from `fromVersion` to kSaveVersion. Versions this build
// cannot migrate are rejected by decode() before this is called.
void migrate(WorldSave& save, std::uint16_t fromVersion);

}  // namespace hr::save
