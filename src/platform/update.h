// The self-updater: fetch the latest public release and install it over this one.
//
// Scope, deliberately narrow, because this is the one part of the game that
// downloads code and runs it:
//
//   - It talks to exactly one host, over HTTPS, at a URL compiled into the
//     binary. There is no setting for it and no redirect it will follow off that
//     host, so there is no configuration a player can be talked into changing.
//   - It accepts exactly one asset per release, matched by name against
//     `Hollowreach-v<version>-Windows.zip`. Anything else attached to a release —
//     by anybody, for any reason — is ignored.
//   - Nothing happens without a click. It never checks on its own, never installs
//     on its own, and the version it found is shown before anything is downloaded.
//
// What it does NOT do is delete anything. The staged files are copied *over* the
// install directory and nothing else is touched, so `data/`, resource packs, and
// whatever else a player has put beside the executable are safe by construction
// rather than by a list of exclusions somebody has to remember to update.
//
// Windows only. The zip is per-platform and so is replacing a running executable;
// on anything else every entry point here reports "not supported" and the button
// does not appear.

#pragma once

#include <functional>
#include <string>

namespace hr::platform::update {

// Where a check got to. One enum for the whole flow, because the interface shows
// it as one line of text.
enum class Stage {
  Idle,
  Checking,
  UpToDate,
  Available,     // `latest` is newer than this build
  Downloading,
  Staging,       // unpacking the zip
  ReadyToApply,  // staged; applying restarts the game
  Failed,
  Unsupported,
};

struct State {
  Stage stage = Stage::Idle;
  std::string latest;   // "2.1.2", empty until a check succeeds
  std::string message;  // one line, for the player
  float progress = 0.0f;  // 0..1 while downloading
};

// True on a platform that can do this at all.
bool supported();

// Starts a check on a worker thread. Returns immediately; poll with state().
// A second call while one is running is ignored.
//
// `asIfVersion` pretends this build is an older one, for testing: the whole
// "there is an update" half of the flow is otherwise unreachable until a newer
// release exists, which is exactly when it is too late to find out it is broken.
// Empty means "use the real version", which is what the game itself passes.
void check(const std::string& asIfVersion = {});

// Downloads and unpacks the release found by the last check. Only valid from
// Stage::Available.
void download();

// Swaps the staged files in and restarts. Only valid from Stage::ReadyToApply.
//
// Returns false if the handover could not be started, in which case nothing has
// changed. On success the caller must quit promptly — the helper is already
// waiting for this process to let go of its own executable.
bool apply();

State state();

// Frees the staging directory. Safe to call at any time; a download in flight is
// left alone.
void cleanup();

}  // namespace hr::platform::update
