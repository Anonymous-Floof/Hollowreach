// Where the game keeps its files.
//
// The web build kept worlds/ next to server.py, so the native build defaults to
// a data/ folder next to the executable — portable, and a zip the player
// unpacks anywhere just works. If that location is not writable (Program Files,
// a read-only share) we fall back to the per-user application data directory.

#pragma once

#include <string>

namespace hr::paths {

// Resolves every path. `dataDirOverride` comes from --data-dir and wins outright.
void init(const std::string& dataDirOverride);

const std::string& exeDir();
const std::string& dataDir();

const std::string& worldsDir();
// Where "Export" drops a shareable copy of a world. The browser handed the file to
// the download folder; a native build has nowhere implicit to put it, so it gets a
// folder of its own next to the saves rather than scattering files in the data root.
const std::string& exportsDir();
const std::string& screenshotsDir();
const std::string& resourcePacksDir();
const std::string& settingsFile();
const std::string& logFile();

// Creates the data directories if they do not exist. Reports failures rather
// than throwing, since a read-only data dir should not stop the game booting.
bool ensureDirs();

// Joins with a forward slash and normalises separators.
std::string join(const std::string& a, const std::string& b);

}  // namespace hr::paths
