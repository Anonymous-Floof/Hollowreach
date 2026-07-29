#include "save/transfer.h"

#include <cctype>
#include <filesystem>
#include <vector>

#include "core/log.h"
#include "platform/paths.h"
#include "save/format.h"
#include "save/storage.h"

namespace fs = std::filesystem;

namespace hr::save {

std::string safeFileName(const std::string& name) {
  std::string out;
  bool lastWasUnderscore = false;
  for (const unsigned char c : name) {
    const bool keep = std::isalnum(c) != 0 || c == '-' || c == '_';
    if (keep) {
      out.push_back(static_cast<char>(c));
      lastWasUnderscore = false;
    } else if (!lastWasUnderscore) {
      // Runs collapse to one underscore, as the JS regex's `+` did.
      out.push_back('_');
      lastWasUnderscore = true;
    }
  }
  if (out.empty()) out = "world";
  if (out.size() > 64) out.resize(64);
  return out;
}

bool exportWorld(const std::string& id, const std::string& destination, std::string* outPath,
                 std::string* error) {
  std::vector<std::uint8_t> bytes;
  if (!validId(id)) {
    if (error) *error = "unsafe world id";
    return false;
  }
  if (!readFile(pathFor(id), bytes, error)) return false;

  // Validated on the way out as well as the way in: exporting a corrupt file would
  // hand a friend something that cannot be loaded, and the failure would look like
  // their problem rather than this one.
  WorldMeta meta;
  if (!decodeMeta(bytes.data(), bytes.size(), meta, error)) return false;

  std::string path = destination;
  if (path.empty()) {
    paths::ensureDirs();
    path = paths::join(paths::exportsDir(),
                       safeFileName(meta.name.empty() ? id : meta.name) + ".hrw");
  }
  if (!writeFileAtomic(path, bytes, error)) return false;
  if (outPath) *outPath = path;
  return true;
}

bool importWorld(const std::string& sourcePath, std::string* outId, std::string* error) {
  std::vector<std::uint8_t> bytes;
  if (!readFile(sourcePath, bytes, error)) return false;

  // A full decode, not a header check: this is the one path where bytes the game
  // did not write become a world, so everything in them is parsed and validated
  // before anything is written to the saves folder.
  WorldSave save;
  if (!decode(bytes.data(), bytes.size(), save, error)) return false;

  save.meta.id = newId();
  save.meta.savedAt = nowSeconds();
  if (save.meta.createdAt <= 0) save.meta.createdAt = save.meta.savedAt;
  if (save.meta.name.empty()) save.meta.name = "Imported world";
  if (!write(save, error)) return false;
  if (outId) *outId = save.meta.id;
  return true;
}

int importAllFromExports(int* failures) {
  int imported = 0;
  int failed = 0;
  std::error_code ec;
  const std::string dir = paths::exportsDir();
  if (fs::is_directory(dir, ec)) {
    for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
      if (!entry.is_regular_file(ec)) continue;
      if (entry.path().extension() != ".hrw") continue;
      std::string id;
      std::string error;
      if (importWorld(entry.path().string(), &id, &error)) {
        ++imported;
        log::info("imported %s as %s", entry.path().filename().string().c_str(), id.c_str());
      } else {
        ++failed;
        log::warn("import: %s (%s)", entry.path().filename().string().c_str(), error.c_str());
      }
    }
  }
  if (failures) *failures = failed;
  return imported;
}

}  // namespace hr::save
