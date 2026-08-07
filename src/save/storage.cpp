#include "save/storage.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>

#include "core/log.h"
#include "platform/paths.h"

namespace fs = std::filesystem;

namespace hr::save {
namespace {

constexpr const char* kExtension = ".hrw";

std::string base36(std::uint64_t v) {
  if (v == 0) return "0";
  static constexpr char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  std::string out;
  while (v > 0) {
    out.push_back(kDigits[v % 36]);
    v /= 36;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

}  // namespace

std::int64_t nowSeconds() {
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string newId() {
  using namespace std::chrono;
  const auto ms =
      static_cast<std::uint64_t>(
          duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
  // The JS drew from Math.random(); the game's own Mulberry32 is seeded per world
  // and would give the same suffix twice if two worlds were made in the same
  // millisecond, so this one place uses the system's entropy instead.
  static std::mt19937 rng{std::random_device{}()};
  const std::uint64_t salt = std::uniform_int_distribution<std::uint64_t>(0, 1679615)(rng);
  return "w" + base36(ms) + base36(salt);
}

bool validId(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  for (const char c : id) {
    const bool okChar = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!okChar) return false;
  }
  return true;
}

std::string pathFor(const std::string& id) {
  return paths::join(paths::worldsDir(), id + kExtension);
}

bool readFile(const std::string& path, std::vector<std::uint8_t>& out, std::string* error) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (error) *error = "cannot open " + path;
    return false;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 0) {
    std::fclose(f);
    if (error) *error = "cannot size " + path;
    return false;
  }
  out.resize(static_cast<std::size_t>(size));
  const std::size_t got = size > 0 ? std::fread(out.data(), 1, out.size(), f) : 0;
  std::fclose(f);
  if (got != out.size()) {
    if (error) *error = "short read on " + path;
    return false;
  }
  return true;
}

bool writeFileAtomic(const std::string& path, const std::vector<std::uint8_t>& bytes,
                     std::string* error) {
  const std::string temp = path + ".tmp";
  std::FILE* f = std::fopen(temp.c_str(), "wb");
  if (!f) {
    if (error) *error = "cannot write " + temp;
    return false;
  }
  const std::size_t wrote = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), f);
  // Flushed and closed before the rename, or the rename can beat the data to disk
  // and leave a valid-looking file full of zeros.
  const bool flushed = std::fflush(f) == 0;
  std::fclose(f);
  if (wrote != bytes.size() || !flushed) {
    std::error_code ec;
    fs::remove(temp, ec);
    if (error) *error = "short write on " + temp;
    return false;
  }

  std::error_code ec;
  fs::rename(temp, path, ec);
  if (ec) {
    // Windows will not rename onto an existing file on some filesystems; fall back
    // to replacing it, which is still atomic enough that a reader sees one or the
    // other and never a half-written file.
    fs::remove(path, ec);
    fs::rename(temp, path, ec);
  }
  if (ec) {
    fs::remove(temp, ec);
    if (error) *error = "cannot replace " + path;
    return false;
  }
  return true;
}

std::vector<WorldListing> list() {
  std::vector<WorldListing> out;
  std::error_code ec;
  const std::string dir = paths::worldsDir();
  if (!fs::is_directory(dir, ec)) return out;

  for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    if (entry.path().extension() != kExtension) continue;

    const std::string path = entry.path().string();
    std::vector<std::uint8_t> bytes;
    std::string error;
    if (!readFile(path, bytes, &error)) {
      log::warn("world list: %s", error.c_str());
      continue;
    }
    WorldMeta meta;
    if (!decodeMeta(bytes.data(), bytes.size(), meta, &error)) {
      log::warn("world list: %s (%s)", entry.path().filename().string().c_str(), error.c_str());
      continue;
    }

    WorldListing row;
    // The filename is authoritative: it is what erase() and read() address, and a
    // file whose META claims a different id would otherwise be unreachable.
    row.id = entry.path().stem().string();
    row.name = meta.name.empty() ? row.id : meta.name;
    row.path = path;
    row.seed = meta.seed;
    row.savedAt = meta.savedAt;
    row.createdAt = meta.createdAt;
    row.genVersion = meta.genVersion;
    out.push_back(std::move(row));
  }

  // Newest save first, as in js/save/storage.js:49.
  std::sort(out.begin(), out.end(), [](const WorldListing& a, const WorldListing& b) {
    if (a.savedAt != b.savedAt) return a.savedAt > b.savedAt;
    return a.id < b.id;
  });
  return out;
}

bool write(const WorldSave& save, std::string* error) {
  if (!validId(save.meta.id)) {
    if (error) *error = "refusing to save a world with an unsafe id";
    return false;
  }
  paths::ensureDirs();
  const std::vector<std::uint8_t> bytes = encode(save);
  return writeFileAtomic(pathFor(save.meta.id), bytes, error);
}

bool read(const std::string& id, WorldSave& out, std::string* error) {
  if (!validId(id)) {
    if (error) *error = "unsafe world id";
    return false;
  }
  std::vector<std::uint8_t> bytes;
  if (!readFile(pathFor(id), bytes, error)) return false;
  if (!decode(bytes.data(), bytes.size(), out, error)) return false;
  // The filename wins over whatever META says, so a hand-copied file loads and
  // saves back to where it actually is.
  out.meta.id = id;
  return true;
}

bool erase(const std::string& id) {
  if (!validId(id)) return false;
  std::error_code ec;
  return fs::remove(pathFor(id), ec);
}

bool backup(const std::string& id, std::string* newIdOut, std::string* error) {
  WorldSave save;
  if (!read(id, save, error)) return false;

  const std::string copyId = newId();
  save.meta.id = copyId;
  // Trimmed, because a backup of a backup of a backup would otherwise grow a name
  // no row in the list is wide enough to show.
  if (save.meta.name.size() > 40) save.meta.name.resize(40);
  save.meta.name += " (backup)";
  // Deliberately NOT restamped: savedAt is what the list sorts by and what "saved 3m
  // ago" reads, and a copy has not been played. Leaving it means the backup sits
  // beside the world it came from rather than jumping to the top as if it were the
  // newer of the two.
  if (!write(save, error)) return false;
  if (newIdOut) *newIdOut = copyId;
  return true;
}

bool setGenVersion(const std::string& id, std::int32_t version, std::string* error) {
  WorldSave save;
  if (!read(id, save, error)) return false;
  if (save.meta.genVersion == version) return true;
  save.meta.genVersion = version;
  return write(save, error);
}

}  // namespace hr::save
