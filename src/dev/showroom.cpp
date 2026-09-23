#include "dev/showroom.h"

#include <algorithm>

#include "core/log.h"
#include "game/entities/manager.h"
#include "game/items.h"
#include "world/blocks.h"
#include "world/world.h"

namespace hr::dev {
namespace {

using world::BlockDef;
using world::BlockId;
using world::RenderKind;

// Clears a box of air above a floor, so whatever terrain was here cannot hide or
// light-block anything in the grid.
void clearAndFloor(world::World& w, int x0, int x1, int z0, int z1, int floorY, BlockId floor) {
  for (int x = x0; x <= x1; ++x) {
    for (int z = z0; z <= z1; ++z) {
      w.setBlock(x, floorY, z, floor, 0);
      for (int y = floorY + 1; y <= floorY + 6; ++y) w.setBlock(x, y, z, world::kAir, 0);
    }
  }
}

void placeBlocks(world::World& w, int cx, int floorY, int z) {
  const world::WellKnownBlocks& k = world::wk();
  const BlockId polished = world::blocks().idOf("polished");

  std::vector<const BlockDef*> defs;
  for (const BlockDef& def : world::blocks().all()) {
    if (def.id == world::kAir || def.render == RenderKind::Liquid) continue;
    defs.push_back(&def);
  }

  constexpr int kCols = 16;
  constexpr int kStep = 2;
  const int rows = static_cast<int>((defs.size() + kCols - 1) / kCols);
  const int x0 = cx - (kCols * kStep) / 2;
  const int zFront = z - 3;
  // Two spare rows at the back for the dyed samples.
  clearAndFloor(w, x0 - 2, x0 + kCols * kStep + 1, zFront - (rows + 3) * kStep, z + 1, floorY,
                polished);

  for (std::size_t i = 0; i < defs.size(); ++i) {
    const BlockDef& def = *defs[i];
    const int x = x0 + static_cast<int>(i % kCols) * kStep;
    const int bz = zFront - static_cast<int>(i / kCols) * kStep;
    const int y = floorY + 1;
    // At debug level, so --verbose says where to point --at for a close-up.
    log::debug("showroom: %s at %d,%d,%d", def.key.c_str(), x, y, bz);

    // The floor each block needs to stand, so nothing pops off as unsupported.
    if (def.cropStages > 0) w.setBlock(x, floorY, bz, k.farmland, 0);
    else if (def.isPlant) w.setBlock(x, floorY, bz, k.turf, 0);
    if (def.shore) w.setBlock(x + 1, floorY, bz, k.water, 0);

    int meta = 0;
    if (def.cropStages > 0) meta = world::cropMetaFor(def.cropStages - 1);  // ripe
    if (def.render == RenderKind::Ladder || def.render == RenderKind::Painting) meta = 3;
    if (def.directional) meta = 2;  // front toward +z, where the camera stands

    if (def.tall) {
      w.setBlock(x, y, bz, def.id, meta);
      w.setBlock(x, y + 1, bz, def.id, meta | 2);
    } else if (def.render == RenderKind::Bed) {
      w.setBlock(x, y, bz, def.id, 0);          // foot, facing +x
      w.setBlock(x + 1, y, bz, def.id, 0 | 4);  // head
    } else {
      w.setBlock(x, y, bz, def.id, meta);
    }
  }

  // Dyed samples: the same blocks in a spread of colours, and one undyed of each,
  // because the undyed one is what every world without a palette looks at.
  static constexpr std::uint32_t kDyes[] = {0xFFFFFF, 0xD23A34, 0xE8862A, 0xF2C53A,
                                            0x4FAE53, 0x4A6FE0, 0x9A5AC2, 0x2A2A30};
  const int dz = zFront - (rows + 1) * kStep;
  const BlockId dyeable[] = {world::blocks().idOf("wool"), k.glass, k.bed};
  for (int row = 0; row < 3; ++row) {
    for (int c = 0; c < 8; ++c) {
      const int x = x0 + c * kStep;
      const int bz = dz - row * kStep;
      const int y = floorY + 1;
      if (dyeable[row] == k.bed) {
        w.setBlock(x, y, bz, k.bed, 0);
        w.setBlock(x + 1, y, bz, k.bed, 4);
        if (c > 0) {
          w.setTint(x, y, bz, kDyes[c]);
          w.setTint(x + 1, y, bz, kDyes[c]);
        }
      } else {
        w.setBlock(x, y, bz, dyeable[row], 0);
        w.setBlock(x, y + 1, bz, dyeable[row], 0);
        if (c > 0) {
          w.setTint(x, y, bz, kDyes[c]);
          w.setTint(x, y + 1, bz, kDyes[c]);
        }
      }
    }
  }
  log::info("--showroom blocks: %zu blocks in %d rows from z=%d", defs.size(), rows, zFront);
}

void placeItems(world::World& w, game::EntityManager& entities, int cx, int floorY, int z) {
  const auto& all = game::items().all();
  constexpr int kCols = 20;
  const int rows = static_cast<int>((all.size() + kCols - 1) / kCols);
  const int zFront = z - 3;
  const BlockId polished = world::blocks().idOf("polished");
  clearAndFloor(w, cx - kCols / 2 - 2, cx + kCols / 2 + 2, zFront - rows - 2, z + 1, floorY,
                polished);

  int placed = 0;
  for (const game::ItemDef& item : all) {
    const int col = item.index % kCols;
    const int row = item.index / kCols;
    const float px = static_cast<float>(cx - kCols / 2 + col) + 0.5f;
    const float pz = static_cast<float>(zFront - row) + 0.5f;
    // Tossed rather than instant, so it waits to be walked over instead of being
    // vacuumed into the bag the moment it lands.
    game::Entity* e = entities.spawnTossed(Vec3{px, static_cast<float>(floorY + 1), pz},
                                           Vec3{0, 0, 0}, item.key, 1, -1, -1);
    if (!e) continue;
    e->vel = Vec3{0, 0, 0};
    e->yaw = 0.0f;
    ++placed;
  }
  (void)w;
  log::info("--showroom items: %d drops in %d rows from z=%d", placed, rows, zFront);
}

}  // namespace

bool buildShowroom(world::World& world, game::EntityManager& entities, const std::string& mode,
                   int x, int floorY, int z) {
  if (mode == "blocks") {
    placeBlocks(world, x, floorY, z);
    return true;
  }
  if (mode == "items") {
    placeItems(world, entities, x, floorY, z);
    return true;
  }
  log::warn("--showroom: unknown mode \"%s\" (blocks, items)", mode.c_str());
  return false;
}

}  // namespace hr::dev
