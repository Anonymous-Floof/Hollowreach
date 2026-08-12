#include "dev/savetool.h"

#include <cstdio>
#include <ctime>
#include <map>
#include <vector>

#include "platform/paths.h"
#include "save/format.h"
#include "save/storage.h"
#include "save/transfer.h"
#include "world/blocks.h"
#include "world/chunk.h"
#include "world/world.h"
#include "world/worldgen.h"

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

int dungeonInfo(std::uint32_t seed) {
  const world::NoiseSet noise(seed);
  world::DungeonSite site;
  if (!world::findDungeon(noise, seed, world::kGenVersion, 0, 0, 6, site)) {
    std::printf("no dungeon within 6 cells of the origin for seed %u\n", seed);
    return 1;
  }

  std::printf("seed %u: nearest dungeon altar at %d %d %d (%d rooms, %s)\n", seed, site.x,
              site.y, site.z, site.rooms,
              site.tunnel
                  ? ("tunnel " + std::to_string(site.tunnelLen) + " toward " +
                     std::to_string(site.tunnelDx) + "," + std::to_string(site.tunnelDz))
                        .c_str()
                  : "sealed");
  std::printf("  --at %d,%d,%d,0,0\n\n", site.x, site.y + 2, site.z - 6);

  // Generate the chunks the plan could touch and read them back, rather than asking
  // the planner to describe itself. The point is to check what actually landed in
  // the voxels, which is the only thing a player ever meets.
  constexpr int kReach = 40;
  const int cx0 = world::World::floorDiv16(site.x - kReach);
  const int cx1 = world::World::floorDiv16(site.x + kReach);
  const int cz0 = world::World::floorDiv16(site.z - kReach);
  const int cz1 = world::World::floorDiv16(site.z + kReach);

  std::vector<world::Chunk> chunks;
  for (int cx = cx0; cx <= cx1; ++cx) {
    for (int cz = cz0; cz <= cz1; ++cz) {
      world::Chunk c;
      c.cx = cx;
      c.cz = cz;
      c.data = std::make_shared<world::ChunkData>();
      world::generate(c, noise, world::kGenVersion);
      chunks.push_back(std::move(c));
    }
  }
  const auto blockAt = [&](int wx, int wy, int wz) -> world::BlockId {
    const int cx = world::World::floorDiv16(wx), cz = world::World::floorDiv16(wz);
    for (const world::Chunk& c : chunks) {
      if (c.cx != cx || c.cz != cz) continue;
      return c.data->voxels.get(world::localIdx(wx & 15, wy, wz & 15));
    }
    return world::kAir;
  };

  const world::BlockId altar = world::blocks().idOf("evil_altar");
  const world::BlockId chest = world::blocks().idOf("chest");
  const world::BlockId bricks = world::blocks().idOf("bricks");
  const world::BlockId cobbled = world::blocks().idOf("cobbled");

  std::printf("floor plan at y=%d  ( . air  # bricks  , floor  A altar  C chest  ~ rock )\n",
              site.y);
  for (int wz = site.z - kReach; wz <= site.z + kReach; ++wz) {
    std::string row;
    for (int wx = site.x - kReach; wx <= site.x + kReach; ++wx) {
      const world::BlockId id = blockAt(wx, site.y, wz);
      if (id == altar) row += 'A';
      else if (id == chest) row += 'C';
      else if (id == bricks) row += '#';
      else if (id == cobbled) row += ',';
      else if (id == world::kAir) row += '.';
      else row += '~';
    }
    std::printf("  %s\n", row.c_str());
  }

  // A vertical slice through the altar, which is what shows the ceiling is closed
  // and the floor is laid.
  std::printf("\nside view through z=%d\n", site.z);
  for (int wy = site.y + 8; wy >= site.y - 3; --wy) {
    std::string row;
    for (int wx = site.x - kReach; wx <= site.x + kReach; ++wx) {
      const world::BlockId id = blockAt(wx, wy, site.z);
      if (id == altar) row += 'A';
      else if (id == chest) row += 'C';
      else if (id == bricks) row += '#';
      else if (id == cobbled) row += ',';
      else if (id == world::kAir) row += '.';
      else row += '~';
    }
    std::printf("  y%3d %s\n", wy, row.c_str());
  }
  return 0;
}

int cropInfo(std::uint32_t seed) {
  // The same shape as dungeonInfo above: generate the chunks, then read back what
  // actually landed in the voxels. A wild crop stand is far too sparse to find by
  // flying around and looking, which made "are the stands generating, and are they
  // coherent rather than confetti?" a question nothing could answer.
  const world::NoiseSet noise(seed);
  constexpr int kChunkReach = 6;  // +/- 96 blocks

  struct Found {
    int x, y, z;
    world::BlockId id;
  };
  std::vector<Found> found;
  std::map<std::string, int> tally;

  for (int cx = -kChunkReach; cx <= kChunkReach; ++cx) {
    for (int cz = -kChunkReach; cz <= kChunkReach; ++cz) {
      world::Chunk c;
      c.cx = cx;
      c.cz = cz;
      c.data = std::make_shared<world::ChunkData>();
      world::generate(c, noise, world::kGenVersion);
      for (int x = 0; x < world::CX; ++x) {
        for (int z = 0; z < world::CZ; ++z) {
          for (int y = 1; y < world::WH; ++y) {
            const world::BlockId id = c.data->voxels.get(world::localIdx(x, y, z));
            if (id == world::kAir) continue;
            const world::BlockDef& d = world::blocks().def(id);
            if (d.cropStages <= 0) continue;
            found.push_back({cx * world::CX + x, y, cz * world::CZ + z, id});
            ++tally[d.key];
          }
        }
      }
    }
  }

  if (found.empty()) {
    std::printf("no wild crops within %d chunks of the origin for seed %u\n", kChunkReach,
                seed);
    return 1;
  }

  std::printf("seed %u: %zu wild crop cells within %d chunks of the origin\n", seed,
              found.size(), kChunkReach);
  for (const auto& [key, n] : tally) std::printf("  %-18s %4d\n", key.c_str(), n);

  // The DENSEST 16x16 region, not the nearest cell. The nearest cell is usually a
  // lone straggler on the edge of something, which frames a screenshot on one plant
  // and a lot of grass; the densest region is the stand you actually wanted to look
  // at, and whether stands form at all is the thing worth checking.
  std::map<std::pair<int, int>, int> byRegion;
  for (const Found& f : found) {
    ++byRegion[{f.x >> 4, f.z >> 4}];
  }
  std::pair<int, int> bestRegion = byRegion.begin()->first;
  int bestCount = 0;
  for (const auto& [region, n] : byRegion) {
    if (n > bestCount) {
      bestCount = n;
      bestRegion = region;
    }
  }
  const int rx = bestRegion.first * 16 + 8, rz = bestRegion.second * 16 + 8;

  // A representative cell inside that region, for the height and the name.
  const Found* best = &found.front();
  for (const Found& f : found) {
    if ((f.x >> 4) == bestRegion.first && (f.z >> 4) == bestRegion.second) {
      best = &f;
      break;
    }
  }
  std::printf("\ndensest stand: %d cells around %d,%d (%s)\n", bestCount, rx, rz,
              world::blocks().def(best->id).key.c_str());
  std::printf("  --at %d,%d,%d,0,-0.55       (standing in it)\n", rx, best->y + 2, rz - 7);
  std::printf("  --at %d,%d,%d,0,-1.50       (looking down on it)\n", rx, best->y + 22, rz);
  return 0;
}

}  // namespace hr::dev
