// Headless commands for the save folder.
//
// The web build could inspect a save by opening the JSON in an editor and could
// share one through the browser's download and file-picker dialogs. A binary format
// and a native build have neither, so those two capabilities become command-line
// tools — and `--save-info` doubles as the thing that tells you *why* a world will
// not load, which the browser never had to answer because a bad JSON file was
// readable by eye.
//
// All of these run without a window or GL. They assume paths::init has already been
// called, so --data-dir applies to them exactly as it does to the game.

#pragma once

#include <string>

namespace hr::dev {

// One line per world in data/worlds: id, name, seed, size, when it was saved.
int listWorlds();

// Header, section inventory and counts for one .hrw file. Reports what is wrong
// with a file it cannot decode rather than just failing.
int saveInfo(const std::string& path);

// Copies a world out to a shareable file, and a shareable file in under a fresh id.
int exportWorld(const std::string& id, const std::string& destination);
int importWorld(const std::string& sourcePath);

// Locates the nearest dungeon to the origin for a seed and prints its floor plan.
//
// Here rather than in a test because a structure generator needs an eye on it, and
// a dungeon is unlit by design — a screenshot of one is a black rectangle, and the
// AO debug view shows shape without showing what anything IS. A slice through the
// voxels in text is the only view that tells you the altar is where it should be and
// the room is genuinely sealed. It is also the fastest way to see a layout change,
// which is worth having the next time somebody touches the generator.
int dungeonInfo(std::uint32_t seed);

}  // namespace hr::dev
