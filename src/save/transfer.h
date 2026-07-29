// Sharing a world as a single file, ported from js/save/transfer.js.
//
// The browser could hand a Blob to the download folder and put up a file picker; a
// native build has neither, and pulling in a system file-dialog library for two
// buttons is not worth a dependency. So the shape changes slightly while the
// feature stays the same:
//
//   export  writes data/exports/<name>.hrw and tells the player where it went
//   import  reads a path — from --import-world, or from anything dropped into
//           data/exports — and copies it into data/worlds under a fresh id
//
// Import assigns a new id rather than keeping the file's own, which is the one
// piece of real work here. Two players who both started a world called "Hollow"
// would otherwise carry the same id, and importing a friend's copy would silently
// overwrite yours. A fresh id also means importing the same file twice gives two
// worlds, which is what a copy is.

#pragma once

#include <string>

namespace hr::save {

// Copies world `id` to `destination`, or to data/exports/<world name>.hrw when the
// destination is empty. Fills `outPath` with where it actually landed.
bool exportWorld(const std::string& id, const std::string& destination, std::string* outPath,
                 std::string* error);

// Reads a `.hrw` from anywhere, gives it a fresh id, and writes it into the worlds
// folder. Fills `outId` with the new id. Rejects anything that does not decode —
// import is the one place a file the game did not write gets read, so it is
// validated in full rather than by its header.
bool importWorld(const std::string& sourcePath, std::string* outId, std::string* error);

// Imports every `.hrw` sitting in data/exports, which is what the menu's Import
// button does. Returns how many came in; `failures` counts files that were there
// but would not decode. Importing the same file twice gives two worlds, exactly as
// picking it twice in the browser's dialog would have.
int importAllFromExports(int* failures);

// A world name reduced to something safe to use as a filename, matching the JS's
// `replace(/[^a-z0-9_-]+/gi, "_")`.
std::string safeFileName(const std::string& name);

}  // namespace hr::save
