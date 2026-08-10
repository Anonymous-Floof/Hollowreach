// Headless commands for resource packs.
//
// The same argument the dungeon inspector makes: a system whose whole job is to
// resolve names through a stack of folders needs a way to be asked *what it
// resolved*, or every question about it becomes "play the game and listen".
// Whether `block.stone.break` came from your pack, from the one below it, or from
// nowhere at all is one line of output here and a guessing game otherwise.
//
// Both run without a window, GL or an audio device. They assume paths::init has
// been called, so --data-dir applies exactly as it does to the game.

#pragma once

#include <string>

namespace hr::dev {

// Lists installed packs, then every sound event, saying which file each resolves
// to under the currently enabled selection — or that it is still synthesised.
// `all` includes the events no pack supplies, which is the view you want when
// filling a pack in; without it only the replacements are printed.
int listPacks(bool all);

// Writes a fill-in-the-blanks pack: pack.mcmeta, a sounds.json naming every event,
// and the folder tree. `directory` is created; an existing one is written over
// file by file, which is deliberate — regenerating after adding an event should
// update sounds.json without discarding the sounds already dropped in.
int makeExamplePack(const std::string& directory);

}  // namespace hr::dev
