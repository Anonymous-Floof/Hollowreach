// Asset access. Shaders and fonts are baked into the executable by
// cmake/embed_assets.cmake; in Debug the on-disk copy under the source tree
// wins, so editing a shader and hot-reloading needs no rebuild.
//
// Names are paths relative to assets with forward slashes, e.g.
// "shaders/terrain.vert".

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hr::assets {

struct Entry {
  const char* name;
  const unsigned char* data;
  std::size_t size;
};

// Defined by the generated translation unit.
extern const Entry kEmbedded[];
extern const unsigned kEmbeddedCount;

// Enables the disk-first lookup path. Pass the directory that contains
// `shaders/`. Called once at startup with HR_SOURCE_ASSET_DIR in Debug, or with
// an `assets` folder beside the executable when one exists.
void setOverrideDir(std::string dir);

// True when a disk override is active — the shader hot-reload watch only makes
// sense then.
bool hasOverrideDir();

// Absolute path of `name` in the override directory, or empty if no override.
std::string overridePath(std::string_view name);

// Reads an asset as text. Returns nullopt when the name is unknown.
std::optional<std::string> readText(std::string_view name);

// Reads an asset as bytes. Returns an empty vector when the name is unknown.
std::vector<unsigned char> readBytes(std::string_view name);

// Last-modified time of the override copy, or 0. Used by the shader watcher.
long long overrideMTime(std::string_view name);

// Every embedded name whose path starts with `prefix`. Used to enumerate
// shader includes and, later, the bundled fonts.
std::vector<std::string> list(std::string_view prefix);

}  // namespace hr::assets
