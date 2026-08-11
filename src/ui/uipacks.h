// Applying resource packs to the interface.
//
// The counterpart of audio/soundbank.h, and deliberately the same shape: packs
// arrive in selection order, the ones that supply anything are read, and what
// they supply replaces what was there. A pack that supplies nothing for the
// interface falls through and costs one directory check.
//
// What a pack may supply today:
//
//     assets/<ns>/ui/theme.json      colours and measurements
//
// and that is the whole surface. It is small on purpose — the interface's own
// token table (ui/theme.h) is what makes it expressive, so a pack sets a dozen
// values and every screen follows, rather than describing every screen.
//
// This lives apart from ui/theme.h so that theme.h — which every file in ui/
// includes — does not have to pull in the resource layer to define a colour.
//
// **Nothing here trusts the pack.** The file is somebody's download: the path is
// checked before it reaches the filesystem, the JSON is size-capped before it is
// parsed, unknown names are reported rather than obeyed, and out-of-range numbers
// are dropped. A theme that cannot be read leaves the interface exactly as it was
// rather than half-applied, because a half-applied theme is an interface with
// invisible text in it and no way to reach the menu that would turn it off.

#pragma once

#include <string>
#include <vector>

#include "resource/pack.h"

namespace hr::ui {

struct UiPackReport {
  // Packs that actually supplied a theme, not packs that were enabled.
  int packs = 0;
  int colors = 0;
  int scalars = 0;
  // One line per problem, already phrased for a player rather than for a log:
  // this is what the Resource Packs screen shows, and a pack that silently does
  // nothing is the worst outcome of all.
  std::vector<std::string> problems;

  bool changedAnything() const { return colors > 0 || scalars > 0; }
};

// Reads every enabled pack's theme and rebuilds the process-wide Theme.
//
// `ordered` is the enabled list in selection order — **highest priority first**,
// matching resource/pack.h. They are applied in reverse of that, so the first
// pack in the list is the last one to speak and therefore the one that wins.
//
// Always rebuilds, even with an empty list: that is what turning every pack off
// has to restore, and a build from no documents is exactly the built-in theme.
UiPackReport applyUiPacks(const std::vector<resource::PackInfo>& ordered);

// The largest theme.json this build will read, in bytes. A theme file is a few
// hundred lines of names and hex; anything past this is not one, and parsing it
// would be work done on behalf of a file that is lying about what it is.
inline constexpr std::size_t kMaxThemeBytes = 256 * 1024;

}  // namespace hr::ui
