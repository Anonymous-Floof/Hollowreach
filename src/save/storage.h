// Where saves live on disk, ported from js/save/storage.js.
//
// The web build talked to server.py's /api/world endpoints, which wrote one JSON
// file per world into `worlds/` beside the server. This writes one `.hrw` file per
// world into `data/worlds/`, which is the same folder in the same place relative to
// the program — so the two builds' save directories sit side by side and neither is
// hidden inside a browser's per-origin storage. The whole localStorage migration
// path in the original has no native counterpart and is gone.
//
// Two things the original did not do, both cheap and both worth having:
//
// **Writes are atomic.** A save goes to `<id>.hrw.tmp` and is renamed over the real
// file only once it is completely written. Losing power mid-save costs the save,
// never the world — with a plain overwrite it costs the world, because the moment
// the file is opened for writing the old bytes are gone.
//
// **Ids are validated, not trusted.** A world id becomes a filename, and an id is
// something an imported file can carry. `../../` in one would be a path traversal,
// so anything but lowercase alphanumerics, dash and underscore is rejected.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "save/format.h"

namespace hr::save {

// "w" + base36 milliseconds + base36 randomness, as in js/save/storage.js:14.
std::string newId();

// True when `id` is safe to use as a filename.
bool validId(const std::string& id);

// data/worlds/<id>.hrw
std::string pathFor(const std::string& id);

// One row of the world list. Read from each file's header and META section only, so
// listing forty worlds does not decode forty edit maps.
struct WorldListing {
  std::string id;
  std::string name;
  std::string path;
  std::uint32_t seed = 0;
  std::int64_t savedAt = 0;
  std::int64_t createdAt = 0;
};

// Every readable world, newest save first. Unreadable files are logged and skipped
// rather than failing the listing — one corrupt world must not hide the rest.
std::vector<WorldListing> list();

bool write(const WorldSave& save, std::string* error = nullptr);
bool read(const std::string& id, WorldSave& out, std::string* error = nullptr);
bool erase(const std::string& id);

// Raw file helpers, shared with save/transfer and used by the self-test.
bool readFile(const std::string& path, std::vector<std::uint8_t>& out, std::string* error);
bool writeFileAtomic(const std::string& path, const std::vector<std::uint8_t>& bytes,
                     std::string* error);

// Unix seconds. One place, so every timestamp in a save agrees.
std::int64_t nowSeconds();

}  // namespace hr::save
