#include "dev/savetool.h"

#include <cstdio>
#include <ctime>
#include <vector>

#include "platform/paths.h"
#include "save/format.h"
#include "save/storage.h"
#include "save/transfer.h"

namespace hr::dev {
namespace {

std::string stamp(std::int64_t unixSeconds) {
  if (unixSeconds <= 0) return "-";
  const auto t = static_cast<std::time_t>(unixSeconds);
  std::tm tm {};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M", &tm);
  return buffer;
}

std::size_t countEdits(const world::World::EditMap& edits) {
  std::size_t n = 0;
  for (const auto& [key, cells] : edits) n += cells.size();
  return n;
}

}  // namespace

int listWorlds() {
  const std::vector<save::WorldListing> worlds = save::list();
  if (worlds.empty()) {
    std::printf("no worlds in %s\n", paths::worldsDir().c_str());
    return 0;
  }
  std::printf("%-18s %-28s %12s  %s\n", "id", "name", "seed", "saved");
  for (const save::WorldListing& w : worlds) {
    std::printf("%-18s %-28s %12u  %s\n", w.id.c_str(), w.name.c_str(), w.seed,
                stamp(w.savedAt).c_str());
  }
  return 0;
}

int saveInfo(const std::string& path) {
  // A bare id is accepted as well as a path, because that is what --list-worlds
  // prints and typing the whole path back in is friction for nothing.
  std::string file = path;
  std::vector<std::uint8_t> bytes;
  std::string error;
  if (!save::readFile(file, bytes, &error)) {
    file = save::pathFor(path);
    if (!save::readFile(file, bytes, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return 1;
    }
  }

  std::printf("file        %s\n", file.c_str());
  std::printf("bytes       %zu\n", bytes.size());
  const std::uint16_t version = save::peekVersion(bytes.data(), bytes.size());
  std::printf("version     %u%s\n", static_cast<unsigned>(version),
              version == 0 ? "  (not a Hollowreach world file)" : "");

  save::WorldSave data;
  if (!save::decode(bytes.data(), bytes.size(), data, &error)) {
    std::fprintf(stderr, "decode failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("id          %s\n", data.meta.id.c_str());
  std::printf("name        %s\n", data.meta.name.c_str());
  std::printf("seed        %u  (gen v%d)\n", data.meta.seed, data.meta.genVersion);
  std::printf("written by  %s\n", data.meta.gameVersion.c_str());
  std::printf("created     %s\n", stamp(data.meta.createdAt).c_str());
  std::printf("saved       %s\n", stamp(data.meta.savedAt).c_str());
  std::printf("sky time    %.3f\n", static_cast<double>(data.meta.time));
  if (data.meta.hasSpawn) {
    std::printf("spawn       %.1f %.1f %.1f\n", static_cast<double>(data.meta.spawn.x),
                static_cast<double>(data.meta.spawn.y), static_cast<double>(data.meta.spawn.z));
  }
  std::printf("player      %.1f %.1f %.1f  hp %.1f  food %.1f%s\n",
              static_cast<double>(data.player.pos.x), static_cast<double>(data.player.pos.y),
              static_cast<double>(data.player.pos.z), static_cast<double>(data.player.health),
              static_cast<double>(data.player.hunger), data.player.flying ? "  flying" : "");
  std::printf("edits       %zu cells in %zu chunks\n", countEdits(data.edits), data.edits.size());
  std::printf("explored    %zu chunks\n", data.explored.size());
  std::printf("containers  %zu\n", data.blockEntities.size());
  std::printf("entities    %zu\n", data.entities.size());
  std::printf("waypoints   %zu\n", data.waypoints.size());
  return 0;
}

int exportWorld(const std::string& id, const std::string& destination) {
  std::string outPath;
  std::string error;
  if (!save::exportWorld(id, destination, &outPath, &error)) {
    std::fprintf(stderr, "export failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("exported %s -> %s\n", id.c_str(), outPath.c_str());
  return 0;
}

int importWorld(const std::string& sourcePath) {
  paths::ensureDirs();
  std::string id;
  std::string error;
  if (!save::importWorld(sourcePath, &id, &error)) {
    std::fprintf(stderr, "import failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("imported %s -> %s\n", sourcePath.c_str(), id.c_str());
  return 0;
}

}  // namespace hr::dev
