// A block in mid-air, on its way down.
//
// Sand that loses its footing is lifted out of the voxel grid and handed to one of
// these; it falls under the shared entity physics and writes itself back into the
// grid where it lands. Doing it as an entity rather than as a per-tick block move
// is what makes a collapsing dune look like falling sand instead of a stack of
// blocks blinking downward a layer at a time.
//
// Three things it has to get right, all of them cases where the block would
// otherwise be destroyed:
//
//  * The cell it lands in may not be free — another falling block may have got
//    there first, or a player may have built into the gap. It settles one cell
//    higher in that case, and only drops as an item when there is nowhere at all.
//  * It may land in water. Sand displaces water rather than dissolving, which is
//    what lets a beach be built.
//  * The chunk under it may not be loaded, if it fell past the edge of the loaded
//    area. It waits rather than writing into a chunk that is not there.

#include <algorithm>
#include <cmath>

#include "game/entities/manager.h"
#include "game/entities/types.h"
#include "game/physics.h"
#include "game/player.h"
#include "world/blocks.h"
#include "world/world.h"

namespace hr::game {
namespace {

// Past this it is not coming back — it fell out of the world, or into a hole with
// no floor. Dropped as an item so the material is not silently lost.
constexpr float kMaxFall = 400.0f;

void fallingSpawn(Entity& e) {
  e.data.count = 1;
  e.data.despawn = kMaxFall;
}

// True when a falling block can come to rest ON this cell.
bool restsOn(const world::World& w, int x, int y, int z) {
  if (y < 0) return true;  // the floor of the world
  const world::BlockId id = w.getBlock(x, y, z);
  if (id == world::kAir) return false;
  // Water does not hold sand up; it fills in around it as the sand sinks through.
  if (id == world::wk().water) return false;
  return true;
}

// True when a body is standing in this cell.
//
// The local player is the only one checked, which is the same limit the door rule
// has: a guest's body lives on their own machine and the host holds only a pose
// for them.
bool bodyIn(const EntityContext& ctx, int x, int y, int z) {
  if (!ctx.player) return false;
  const Body& b = ctx.player->body();
  return b.pos.x + b.hw > static_cast<float>(x) &&
         b.pos.x - b.hw < static_cast<float>(x + 1) &&
         b.pos.y + b.h > static_cast<float>(y) &&
         b.pos.y < static_cast<float>(y + 1) &&
         b.pos.z + b.hw > static_cast<float>(z) &&
         b.pos.z - b.hw < static_cast<float>(z + 1);
}

void land(Entity& e, EntityContext& ctx, int x, int y, int z) {
  world::World& w = *ctx.world;
  const world::BlockId id = static_cast<world::BlockId>(e.data.dura);

  // Search upward for the first cell that will actually take it. Bounded, because
  // a column of falling sand landing together must not scan the whole world.
  for (int i = 0; i < 4; ++i) {
    const int ty = y + i;
    if (ty >= world::WH) break;
    const world::BlockId at = w.getBlock(x, ty, z);
    if (at == world::kAir || at == world::wk().water) {
      w.setBlock(x, ty, z, id, e.data.count > 0 ? 0 : 0);
      e.dead = true;
      return;
    }
  }

  // Nowhere to sit. Give the material back rather than deleting it.
  const world::BlockDef& def = world::blocks().def(id);
  if (!def.drop.empty()) {
    w.spawnDrop(x + 0.5f, static_cast<float>(y) + 0.5f, z + 0.5f, def.drop, def.dropCount);
  }
  e.dead = true;
}

void fallingUpdate(Entity& e, float dt, EntityContext& ctx) {
  if (!ctx.world) return;
  world::World& w = *ctx.world;

  if (e.age >= kMaxFall) {
    e.dead = true;
    return;
  }

  const int x = static_cast<int>(std::floor(e.pos.x));
  const int z = static_cast<int>(std::floor(e.pos.z));

  // Off the edge of the loaded world: hold position rather than writing a block
  // into a chunk that does not exist yet, which would be lost when it generates.
  if (!w.chunkReady(world::World::floorDiv16(x), world::World::floorDiv16(z))) {
    e.vel = Vec3{0, 0, 0};
    return;
  }

  // Shared entity physics moves it; this only decides when it has arrived. The
  // cell below the block's own base is what it comes to rest on.
  const int belowY = static_cast<int>(std::floor(e.pos.y)) - 1;
  if (e.vel.y <= 0.0f && restsOn(w, x, belowY, z)) {
    // Somebody is standing exactly where this would become a block. Writing it
    // there leaves a body inside a solid, and the physics sweep resolves that by
    // snapping the body to the nearest face of the box — a jump rather than a
    // slide, and the nearest face can be on the far side of a wall. That is the
    // old close-a-door-on-yourself teleport, which doors avoid by refusing to
    // close at all (game/interact.cpp:233).
    //
    // So it waits, resting on them, and lands the moment they step aside. Waiting
    // rather than settling a cell higher matters: a cell higher would have nothing
    // holding it up once they moved, so the support rule would drop it again
    // immediately and the sand would sit bouncing on their head for as long as
    // they stood there.
    if (bodyIn(ctx, x, belowY + 1, z)) {
      e.vel = Vec3{0, 0, 0};
      e.pos.y = static_cast<float>(belowY + 1);
      return;
    }
    land(e, ctx, x, belowY + 1, z);
  }
}

// A saved falling block comes back as the block it was carrying. Losing the id
// would leave an entity that lands as air, so an unreadable one is dropped rather
// than allowed to erase the cell it settles in.
void fallingLoad(Entity& e) {
  if (e.data.dura <= 0) e.dead = true;
}

}  // namespace

// `dura` carries the BlockId. It is an int field that already saves and
// replicates, and a falling block has no durability to store in it — reusing it
// costs nothing and avoids widening EntityData for one type.
const EntityDef kFallingBlockDef {
    .key = "falling_block",
    .hw = 0.49f,
    .h = 0.98f,
    .physics = true,
    .gravity = 30.0f,
    .flags = {},
    .spawn = fallingSpawn,
    .update = fallingUpdate,
    .interact = nullptr,
    .load = fallingLoad,
};

}  // namespace hr::game
