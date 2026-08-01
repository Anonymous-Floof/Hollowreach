#include "dev/selftest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "audio/dsp.h"
#include "audio/engine.h"
#include "audio/sfx.h"
#include "core/jobs.h"
#include "core/jsmath.h"
#include "game/blockentities.h"
#include "game/entities/ai.h"
#include "game/entities/manager.h"
#include "game/entities/senses.h"
#include "game/crafting.h"
#include "game/interact.h"
#include "game/inventory.h"
#include "game/items.h"
#include "game/player.h"
#include "game/raycast.h"
#include "render/sky.h"
#include "core/bytes.h"
#include "net/client.h"
#include "net/discovery.h"
#include "net/host.h"
#include "net/protocol.h"
#include "net/transport.h"
#include "save/format.h"
#include "save/storage.h"
#include "save/transfer.h"
#include "ui/dom.h"
#include "ui/text.h"
#include "world/water.h"
#include "world/world.h"

namespace hr::dev {
namespace {

int gFailures = 0;
int gChecks = 0;

void check(bool ok, const char* what) {
  ++gChecks;
  if (!ok) ++gFailures;
  std::printf("  [%s] %s\n", ok ? "ok  " : "FAIL", what);
}

void checkf(bool ok, const char* fmt, ...) {
  char buffer[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buffer, sizeof buffer, fmt, args);
  va_end(args);
  check(ok, buffer);
}

// A scratch world with a hollow pocket of air well above the terrain, so tests can
// build exactly the situation they mean without worldgen in the way.
//
// 140, not 100: gen v3 put the sea at y=100 and the test origin is an ocean column,
// so the old pocket was carved underwater and every pond in the water group filled
// itself before the test could. The pocket has to clear the sea, not just the
// ground. The tallest thing built on it is the survival group's fall, at kY + 25.
constexpr int kY = 140;
constexpr float kOriginX = 8.5f;
constexpr float kOriginZ = 8.5f;

// Facing +x, level. See core/mat4.cpp:lookDir.
constexpr float kYawPlusX = -1.5707963267948966f;

std::unique_ptr<world::World> makeWorld() {
  auto w = std::make_unique<world::World>(3918175327u, 2);
  w->primeSpawn(kOriginX, kOriginZ);
  // Clear a 9x5x9 pocket around the test origin.
  for (int x = 4; x <= 12; ++x) {
    for (int y = kY - 2; y <= kY + 2; ++y) {
      for (int z = 4; z <= 12; ++z) w->setBlock(x, y, z, world::kAir, 0);
    }
  }
  return w;
}

// One frame of input. `holdLeft` is held down across frames; `clickRight` is a
// fresh press each time, so the button is released first — otherwise a second
// consecutive call would produce no edge and the click would silently vanish.
void frame(Input& in, bool holdLeft, bool clickRight) {
  in.endFrame();
  in.feedMouseButton(MouseButton::Right, false);
  in.feedMouseButton(MouseButton::Left, holdLeft);
  if (clickRight) in.feedMouseButton(MouseButton::Right, true);
}

// The eye sits 1.62 blocks above the feet, so a block the player is level with is
// the one a block ABOVE their position. Getting this wrong makes every ray miss.
constexpr int kEyeLevel = kY + 1;

game::InteractHooks silentHooks(std::string* lastNotice = nullptr) {
  game::InteractHooks hooks;
  hooks.notify = [lastNotice](const std::string& message) {
    if (lastNotice) *lastNotice = message;
  };
  hooks.onEat = [](const game::ItemDef&) { return false; };
  hooks.onWarp = [] { return false; };
  return hooks;
}

// --- inventory ---------------------------------------------------------------

void testInventory() {
  std::printf("inventory\n");
  game::Inventory inv;

  check(inv.give("planks", 10) == 0, "give fits in one slot");
  check(inv.countOf("planks") == 10, "count reflects the give");

  // A stack of 64 plus 10 already there needs a second slot, and the first fills.
  check(inv.give("planks", 64) == 0, "a second give spills into a new slot");
  check(inv.countOf("planks") == 74, "both slots counted");
  check(inv.slots()[0].count == 64, "the partial stack is topped up first");

  // Tools never stack and arrive at full durability.
  check(inv.give("pick_stone", 1) == 0, "a tool fits");
  const int toolSlot = 2;
  check(inv.slots()[toolSlot].key == "pick_stone", "the tool took the next free slot");
  check(inv.slots()[toolSlot].dura == game::maxDurability("pick_stone"),
        "a fresh tool arrives at full durability");

  // Wearing it down to nothing destroys it.
  inv.setSelected(toolSlot);
  bool broke = false;
  for (int i = 0; i < game::maxDurability("pick_stone"); ++i) {
    broke = inv.damageSelectedTool(1);
  }
  check(broke, "a tool breaks at zero durability");
  check(inv.selectedSlot().empty(), "the broken tool leaves the slot");

  check(inv.give("nonexistent_item", 5) == 5, "an unknown key is refused entirely");

  inv.removeItems({{"planks", 70}});
  check(inv.countOf("planks") == 4, "removeItems spans slots");
  check(inv.hasItems({{"planks", 4}}), "hasItems at exactly the boundary");
  check(!inv.hasItems({{"planks", 5}}), "hasItems just past it");

  // A full bag reports what did not fit rather than silently dropping it.
  game::Inventory full;
  for (int i = 0; i < game::kInventorySlots; ++i) full.give("aetherite", 64);
  check(full.give("aetherite", 10) == 10, "a full bag returns the whole remainder");
}

// --- raycast -----------------------------------------------------------------

void testRaycast() {
  std::printf("raycast\n");
  auto world = makeWorld();
  const world::WellKnownBlocks& wk = world::wk();

  world->setBlock(11, kY, 8, wk.greystone, 0);
  const Vec3 eye {kOriginX, static_cast<float>(kY) + 0.5f, kOriginZ};
  const Vec3 dir = lookDir(kYawPlusX, 0.0f);

  const game::RayHit hit = game::raycast(*world, eye, dir, 6.0f);
  check(hit.hit, "the ray finds the block");
  checkf(hit.x == 11 && hit.y == kY && hit.z == 8, "hit cell is (11,%d,8), got (%d,%d,%d)", kY,
         hit.x, hit.y, hit.z);
  checkf(hit.nx == 10 && hit.ny == kY && hit.nz == 8,
         "the placement cell is the near neighbour, got (%d,%d,%d)", hit.nx, hit.ny, hit.nz);

  // Water is transparent to the aiming ray and solid to the bucket ray.
  world->setBlock(11, kY, 8, world::kAir, 0);
  world->setBlock(10, kY, 8, wk.water, 0);
  world->setBlock(12, kY, 8, wk.greystone, 0);
  const game::RayHit throughWater = game::raycast(*world, eye, dir, 6.0f);
  check(throughWater.hit && throughWater.x == 12, "the aiming ray passes through water");
  const game::RayHit onWater = game::raycast(*world, eye, dir, 6.0f, /*hitLiquid=*/true);
  check(onWater.hit && onWater.x == 10, "the liquid ray stops on water");

  // Nothing within reach.
  auto empty = makeWorld();
  check(!game::raycast(*empty, eye, dir, 3.0f).hit, "an empty ray returns no hit");
}

// --- breaking ----------------------------------------------------------------

void testBreaking() {
  std::printf("breaking\n");
  auto world = makeWorld();
  const world::WellKnownBlocks& wk = world::wk();

  std::vector<std::string> drops;
  world->setDropSink([&drops](float, float, float, const std::string& key, int count, int) {
    drops.push_back(key + "*" + std::to_string(count));
  });

  game::Player player(kOriginX, static_cast<float>(kY), kOriginZ);
  player.setLook(kYawPlusX, 0.0f);
  game::Inventory inv;
  game::Interact interact;
  std::string notice;
  const game::InteractHooks hooks = silentHooks(&notice);
  Input in;

  // Soft blocks drop by hand.
  world->setBlock(11, kEyeLevel, 8, wk.loam, 0);
  frame(in, /*holdLeft=*/true, false);
  interact.update(5.0f, in, player, *world, inv, hooks);
  check(world->getBlock(11, kEyeLevel, 8) == world::kAir, "loam breaks by hand");
  check(drops.size() == 1 && drops[0] == "loam*1", "loam drops itself");

  // A tiered block refuses to drop without the right tool, and says why.
  drops.clear();
  notice.clear();
  world->setBlock(11, kEyeLevel, 8, wk.greystone, 0);
  frame(in, true, false);
  interact.update(30.0f, in, player, *world, inv, hooks);
  check(world->getBlock(11, kEyeLevel, 8) == world::kAir, "greystone still breaks by hand");
  check(drops.empty(), "greystone drops nothing without a pick");
  check(notice.find("wood+ tier pick") != std::string::npos,
        "the refusal names the tier and the tool");

  // With the right tool it drops, and the tool wears by one.
  drops.clear();
  inv.give("pick_stone", 1);
  inv.setSelected(0);
  const int before = inv.selectedSlot().dura;
  world->setBlock(11, kEyeLevel, 8, wk.greystone, 0);
  frame(in, true, false);
  interact.update(30.0f, in, player, *world, inv, hooks);
  check(drops.size() == 1 && drops[0] == "cobbled*1", "a stone pick yields cobbled");
  check(inv.selectedSlot().dura == before - 1, "breaking wears the tool by one");

  // Bedrock is unbreakable no matter how long the button is held.
  world->setBlock(11, kEyeLevel, 8, wk.bedrock, 0);
  frame(in, true, false);
  interact.update(600.0f, in, player, *world, inv, hooks);
  check(world->getBlock(11, kEyeLevel, 8) == wk.bedrock, "bedrock survives");

  // Retargeting mid-break restarts the progress rather than carrying it over.
  world->setBlock(11, kEyeLevel, 8, wk.greystone, 0);
  world->setBlock(11, kEyeLevel + 1, 8, wk.greystone, 0);
  player.setLook(kYawPlusX, 0.0f);
  frame(in, true, false);
  interact.update(0.3f, in, player, *world, inv, hooks);
  const float partial = interact.breakFraction();
  player.setLook(kYawPlusX, 0.3f);  // tilt up onto the block above
  frame(in, true, false);
  interact.update(0.01f, in, player, *world, inv, hooks);
  check(partial > 0.0f && interact.breakFraction() < partial,
        "aiming elsewhere restarts the break");
}

// --- placing -----------------------------------------------------------------

void testPlacing() {
  std::printf("placing\n");
  auto world = makeWorld();
  const world::WellKnownBlocks& wk = world::wk();

  game::Player player(kOriginX, static_cast<float>(kY), kOriginZ);
  game::Inventory inv;
  game::Interact interact;
  const game::InteractHooks hooks = silentHooks();
  Input in;

  // A wall 2.5 blocks ahead at eye level. Level aim lands 0.62 up its face — the
  // upper half — and a slight tilt down lands 0.37 up, the lower half. Everything
  // below places into (10, kEyeLevel, 8), the near neighbour.
  const int px = 10, py = kEyeLevel, pz = 8;
  world->setBlock(11, kEyeLevel, 8, wk.greystone, 0);
  const float kAimHigh = 0.0f;
  const float kAimLow = -0.10f;

  // A plain cube lands on the near face and consumes one from the stack.
  player.setLook(kYawPlusX, kAimLow);
  inv.give("planks", 5);
  inv.setSelected(0);
  frame(in, false, /*clickRight=*/true);
  interact.update(0.016f, in, player, *world, inv, hooks);
  check(world->getBlock(px, py, pz) == world::blocks().idOf("planks"),
        "the block lands on the near face");
  check(inv.countOf("planks") == 4, "placing consumes one");

  // A slab clicked low on a block's side sits in the bottom half.
  world->setBlock(px, py, pz, world::kAir, 0);
  inv.slots()[0].clear();
  inv.give("planks_slab", 4);
  inv.setSelected(0);
  frame(in, false, true);
  interact.update(0.016f, in, player, *world, inv, hooks);
  const world::BlockId slabId = world::blocks().idOf("planks_slab");
  check(world->getBlock(px, py, pz) == slabId, "the slab is placed");
  check((world->getMeta(px, py, pz) & 1) == 0, "a low click gives a bottom slab");

  // Aiming at the upper half of the same face gives a top slab.
  world->setBlock(px, py, pz, world::kAir, 0);
  player.setLook(kYawPlusX, kAimHigh);
  frame(in, false, true);
  interact.update(0.016f, in, player, *world, inv, hooks);
  check(world->getBlock(px, py, pz) == slabId, "the second slab is placed");
  check((world->getMeta(px, py, pz) & 1) == 1, "a high click gives a top slab");

  // A stair faces the player's cardinal, right way up when clicked low.
  world->setBlock(px, py, pz, world::kAir, 0);
  inv.slots()[0].clear();
  inv.give("plank_stairs", 4);
  inv.setSelected(0);
  player.setLook(kYawPlusX, kAimLow);
  frame(in, false, true);
  interact.update(0.016f, in, player, *world, inv, hooks);
  check(world->getBlock(px, py, pz) == world::blocks().idOf("plank_stairs"),
        "the stair is placed");
  check((world->getMeta(px, py, pz) & 3) == 0, "the stair faces +x, the way the player does");
  check((world->getMeta(px, py, pz) & 4) == 0, "a low click leaves it the right way up");

  // Placing into the player's own body is refused.
  world->setBlock(px, py, pz, world::kAir, 0);
  game::Player crowded(px + 0.5f, static_cast<float>(kY), pz + 0.5f);
  crowded.setLook(kYawPlusX, kAimLow);
  const int countBefore = inv.countOf("plank_stairs");
  frame(in, false, true);
  interact.update(0.016f, in, crowded, *world, inv, hooks);
  check(inv.countOf("plank_stairs") == countBefore, "a block cannot be placed inside you");
  check(world->getBlock(px, py, pz) == world::kAir, "and the cell stays empty");

  // A door occupies two stacked cells, the upper one flagged.
  inv.slots()[0].clear();
  inv.give("door", 3);
  inv.setSelected(0);
  frame(in, false, true);
  interact.update(0.016f, in, player, *world, inv, hooks);
  const world::BlockId doorId = world::blocks().idOf("door");
  check(world->getBlock(px, py, pz) == doorId && world->getBlock(px, py + 1, pz) == doorId,
        "a door fills two stacked cells");
  check((world->getMeta(px, py + 1, pz) & 2) != 0, "the upper half is flagged");

  // Toggling flips both halves together. The door is now the nearest block on the
  // ray, so the same click that placed it now opens it.
  const int lowerBefore = world->getMeta(px, py, pz);
  frame(in, false, true);
  interact.update(0.016f, in, player, *world, inv, hooks);
  check((world->getMeta(px, py, pz) & 1) == ((lowerBefore & 1) ^ 1), "the lower half toggled");
  check((world->getMeta(px, py, pz) & 1) == (world->getMeta(px, py + 1, pz) & 1),
        "both halves toggled together");

  // Breaking one half removes the other.
  frame(in, true, false);
  interact.update(5.0f, in, player, *world, inv, hooks);
  check(world->getBlock(px, py, pz) == world::kAir &&
            world->getBlock(px, py + 1, pz) == world::kAir,
        "breaking a door removes both halves");

  // Papyrus needs wet shore ground: sand under the target cell is not enough.
  auto shore = makeWorld();
  shore->setBlock(11, kEyeLevel, 8, wk.greystone, 0);  // the wall we click
  shore->setBlock(px, py - 1, pz, wk.sand, 0);         // ground under the target cell
  game::Inventory reeds;
  reeds.give("papyrus", 4);
  reeds.setSelected(0);
  game::Interact shoreTest;
  std::string notice;
  const game::InteractHooks noticing = silentHooks(&notice);
  player.setLook(kYawPlusX, kAimLow);
  frame(in, false, true);
  shoreTest.update(0.016f, in, player, *shore, reeds, noticing);
  check(notice.find("wet shore") != std::string::npos, "papyrus refuses dry ground");
  check(shore->getBlock(px, py, pz) == world::kAir, "and nothing is placed");

  // ...and accepts it once water sits beside that ground.
  shore->setBlock(px - 1, py - 1, pz, wk.water, 0);
  notice.clear();
  frame(in, false, true);
  shoreTest.update(0.016f, in, player, *shore, reeds, noticing);
  check(shore->getBlock(px, py, pz) == wk.papyrus, "papyrus accepts a wet shore");

  // --- the two items that are not blocks ---------------------------------------
  //
  // Both of these shipped inert: tryPlace returned early for a boat, and onWarp was
  // hardcoded to false. Neither failure was visible anywhere — the click simply did
  // nothing — which is exactly the kind of thing that needs an assertion rather
  // than a play-test.
  {
    auto boatWorld = makeWorld();
    boatWorld->setBlock(11, kEyeLevel, 8, wk.greystone, 0);
    game::Interact boatTest;
    game::Inventory bag;
    bag.give("boat", 2);
    bag.setSelected(0);
    player.setLook(kYawPlusX, kAimLow);

    Vec3 asked {0, 0, 0};
    int calls = 0;
    game::InteractHooks boatHooks = silentHooks();
    boatHooks.spawnBoat = [&](const Vec3& at) {
      asked = at;
      ++calls;
      return true;
    };
    frame(in, false, true);
    boatTest.update(0.016f, in, player, *boatWorld, bag, boatHooks);
    check(calls == 1, "using a boat asks for one to be spawned");
    checkf(std::fabs(asked.x - (px + 0.5f)) < 0.001f &&
               std::fabs(asked.y - static_cast<float>(py)) < 0.001f &&
               std::fabs(asked.z - (pz + 0.5f)) < 0.001f,
           "the boat is asked for at the centre of the empty cell, got (%.2f, %.2f, %.2f)",
           asked.x, asked.y, asked.z);
    check(bag.countOf("boat") == 1, "and the item is spent");
    check(boatWorld->getBlock(px, py, pz) == world::kAir, "a boat places no block");

    // A refusal — no room, or a host that said no — leaves the item in hand.
    boatHooks.spawnBoat = [](const Vec3&) { return false; };
    frame(in, false, true);
    boatTest.update(0.016f, in, player, *boatWorld, bag, boatHooks);
    check(bag.countOf("boat") == 1, "a refused boat is not consumed");
  }

  {
    game::Interact warpTest;
    game::Inventory bag;
    bag.give("wayshard", 3);
    bag.setSelected(0);

    // silentHooks refuses, which is what the shipped build did permanently.
    game::InteractHooks warpHooks = silentHooks();
    frame(in, false, true);
    warpTest.update(0.016f, in, player, *world, bag, warpHooks);
    check(bag.countOf("wayshard") == 3, "a wayshard with nowhere to go is not spent");

    warpHooks.onWarp = [] { return true; };
    frame(in, false, true);
    warpTest.update(0.016f, in, player, *world, bag, warpHooks);
    check(bag.countOf("wayshard") == 2, "a wayshard that warps is spent");
  }

  // The query the warp is built on. The pocket makeWorld carves runs kY-2..kY+2 in
  // an otherwise open column far above the terrain, so the ground is the real
  // surface, well below, and never the air the player is standing in.
  {
    auto ground = makeWorld();
    const int surface = ground->topSolidY(static_cast<int>(kOriginX),
                                          static_cast<int>(kOriginZ));
    checkf(surface > 0 && surface < kY - 2, "topSolidY finds the surface below the pocket (%d)",
           surface);
    check(world::blocks().solid(ground->getBlock(static_cast<int>(kOriginX), surface,
                                                 static_cast<int>(kOriginZ))),
          "and the block it names is solid");
    check(!world::blocks().solid(ground->getBlock(static_cast<int>(kOriginX), surface + 1,
                                                 static_cast<int>(kOriginZ))),
          "with nothing solid above it");
    check(ground->topSolidY(100000, 100000) == -1, "an unloaded column reports no ground");
  }
}

// --- block entities ----------------------------------------------------------

void testBlockEntities() {
  std::printf("block entities\n");
  auto world = makeWorld();

  game::Player player(kOriginX, static_cast<float>(kY), kOriginZ);
  player.setLook(kYawPlusX, -0.10f);
  game::Inventory inv;
  game::Interact interact;
  const game::InteractHooks hooks = silentHooks();
  Input in;

  // Placing a forge creates its state immediately, so it smelts unopened.
  world->setBlock(11, kEyeLevel, 8, world::blocks().idOf("greystone"), 0);
  inv.give("forge", 1);
  inv.setSelected(0);
  frame(in, false, true);
  interact.update(0.016f, in, player, *world, inv, hooks);
  game::BlockEntity* forge = world->getBlockEntity(10, kEyeLevel, 8);
  check(forge != nullptr && forge->kind == game::BlockEntityKind::Forge,
        "placing a forge creates its block entity");

  // One charcoal (48s) smelts raw copper (6s) many times over.
  forge->input = game::ItemStack {"raw_copper", 3, -1};
  forge->fuel = game::ItemStack {"charcoal", 1, -1};
  for (int i = 0; i < 200; ++i) world->tickBlockEntities(0.1f);
  check(forge->output.key == "copper_ingot" && forge->output.count == 3,
        "the forge smelts its whole input on one charcoal");
  check(forge->input.empty(), "the input is consumed");
  check(forge->fuel.empty(), "the fuel item is consumed when it is lit");

  // Mining it spills the contents and drops the entity.
  std::vector<std::string> spilled;
  world->setDropSink([&spilled](float, float, float, const std::string& key, int count, int) {
    spilled.push_back(key + "*" + std::to_string(count));
  });
  inv.give("pick_stone", 1);
  inv.setSelected(1);
  frame(in, true, false);
  interact.update(30.0f, in, player, *world, inv, hooks);
  check(world->getBlockEntity(10, kEyeLevel, 8) == nullptr,
        "mining removes the block entity");
  bool spilledIngots = false;
  for (const std::string& s : spilled) {
    if (s == "copper_ingot*3") spilledIngots = true;
  }
  check(spilledIngots, "the forge spills its contents");
}

// --- crafting ----------------------------------------------------------------

void testCrafting() {
  std::printf("crafting\n");

  // Consuming a shaped craft decrements exactly the cells the pattern used.
  std::vector<game::ItemStack> grid(9);
  grid[0] = {"planks", 3, -1};
  grid[1] = {"planks", 3, -1};
  grid[2] = {"planks", 3, -1};
  grid[4] = {"stick", 2, -1};
  grid[7] = {"stick", 2, -1};
  const game::CraftMatch m = game::matchGrid(grid, 3, game::CraftStation::Workbench);
  check(m && m.outKey == "pick_wood", "a 3x3 pattern matches");
  game::consumeGrid(grid, 3, *m.recipe);
  check(grid[0].count == 2 && grid[4].count == 1, "consume decrements each used cell");
  check(!grid[0].empty(), "a partly used stack survives");

  // A shapeless craft empties the cell it takes the last of.
  std::vector<game::ItemStack> bag(4);
  bag[0] = {"gloamite", 1, -1};
  bag[3] = {"sparkstone", 1, -1};
  const game::CraftMatch shapeless = game::matchGrid(bag, 2, game::CraftStation::Hand);
  check(shapeless && shapeless.outKey == "wayshard" && shapeless.outCount == 2,
        "a shapeless craft matches anywhere in the grid");
  game::consumeGrid(bag, 2, *shapeless.recipe);
  check(bag[0].empty() && bag[3].empty(), "consume clears emptied cells");

  // An extra ingredient makes a shapeless recipe stop matching.
  bag[0] = {"gloamite", 1, -1};
  bag[3] = {"sparkstone", 1, -1};
  bag[1] = {"leather", 1, -1};
  check(!game::matchGrid(bag, 2, game::CraftStation::Hand), "a stray ingredient breaks a match");

  // Fuel derivation: wooden tools burn, stone ones do not.
  check(game::fuelValue("pick_wood") > 0, "a wooden pick is fuel");
  check(game::fuelValue("pick_stone") == 0, "a stone pick is not");
  check(game::fuelValue("chest") > 0, "a chest is fuel");
  check(game::fuelValue("forge") == 0, "a forge is not");
  check(game::fuelValue("#planks") == game::fuelValue("planks"),
        "an ingredient tag resolves to its representative");
}

// --- survival ----------------------------------------------------------------
//
// Breath, hunger and regeneration are coupled through damage(), which is what makes them
// worth asserting together rather than reading three constants back.

void testSurvival() {
  std::printf("survival\n");
  auto world = makeWorld();
  game::Player player(kOriginX, static_cast<float>(kY), kOriginZ);
  game::PlayerOptions options;
  options.hungerEnabled = false;
  options.fallDamageEnabled = false;

  check(player.health() == 20.0f, "a fresh player has full health");
  check(!player.hungerOn(), "hunger follows the setting");

  // Armour soaks a fraction of a normal hit and none of an environmental one.
  player.damage(10.0f, options);
  check(player.health() == 10.0f, "an unarmoured hit lands in full");
  options.defense = 5.0f;  // 5 * 0.04 = 20% reduction
  player.setHealth(20.0f);
  player.damage(10.0f, options);
  check(std::fabs(player.health() - 12.0f) < 0.001f, "armour soaks a fifth of a 10-point hit");
  player.setHealth(20.0f);
  player.damage(10.0f, options, /*ignoreArmor=*/true);
  check(player.health() == 10.0f, "drowning and starving ignore armour");

  // Regeneration waits out the post-hit delay, then heals a point every 2.5 seconds.
  options.defense = 0.0f;
  player.setHealth(10.0f);
  Input idle;
  for (int i = 0; i < 60; ++i) player.update(0.1f, idle, *world, options, i * 0.1);
  check(player.health() == 10.0f, "regeneration is stalled for six seconds after a hit");
  for (int i = 0; i < 30; ++i) player.update(0.1f, idle, *world, options, 6.0 + i * 0.1);
  check(player.health() > 10.0f, "regeneration resumes once the delay expires");

  // Eating with hunger off heals ceil(food / 2) and refuses at full health.
  player.setHealth(10.0f);
  check(player.eat({8.0f, false}), "food is eaten when there is health to restore");
  check(player.health() == 14.0f, "with hunger off, food heals half its points");
  player.setHealth(20.0f);
  check(!player.eat({8.0f, false}), "food is refused at full health rather than wasted");

  // With hunger on, a full bar refuses and a partial one fills.
  game::Player fed(kOriginX, static_cast<float>(kY), kOriginZ);
  options.hungerEnabled = true;
  fed.update(0.001f, idle, *world, options, 0.0);  // picks up hungerOn
  check(fed.hungerOn(), "the hunger flag arrives through PlayerOptions");
  check(!fed.eat({8.0f, false}), "a full hunger bar refuses food");

  // --- a fatal fall, and the teleport that must not be charged as one -----------
  //
  // Death is polled off health reaching zero (App::respawnPlayer), so the whole
  // chain has to hold: the drop has to be measured, the damage has to land, and the
  // respawn's teleport has to restart the fall tracker. Without that last part the
  // player wakes at spawn still owing the drop they died on, lands, dies again, and
  // the world is a death loop with no way out of it.
  {
    auto ground = makeWorld();
    for (int x = 4; x <= 12; ++x) {
      for (int z = 4; z <= 12; ++z) ground->setBlock(x, kY - 3, z, world::wk().greystone, 0);
    }
    const float floorTop = static_cast<float>(kY - 2);
    const float top = static_cast<float>(kY + 25);  // 27 blocks up, well inside WH
    game::PlayerOptions cliff;
    cliff.hungerEnabled = false;
    cliff.fallDamageEnabled = true;

    constexpr float kStep = 1.0f / 60.0f;
    game::Player faller(kOriginX, top, kOriginZ);
    double t = 0.0;
    for (int i = 0; i < 400 && !faller.dead(); ++i, t += kStep) {
      faller.update(kStep, idle, *ground, cliff, t);
    }
    check(faller.dead(), "a twenty-seven block drop is fatal");

    // The same fall, interrupted a second in by what a respawn does.
    game::Player saved(kOriginX, top, kOriginZ);
    t = 0.0;
    for (int i = 0; i < 60; ++i, t += kStep) saved.update(kStep, idle, *ground, cliff, t);
    check(saved.fallDistance() > 3.5f, "the drop is being measured before the teleport");
    saved.teleport(Vec3{kOriginX, floorTop + 1.0f, kOriginZ});
    saved.reviveFull();
    for (int i = 0; i < 200; ++i, t += kStep) saved.update(kStep, idle, *ground, cliff, t);
    check(saved.health() == 20.0f, "a teleport out of a fall is not charged on landing");
  }
}

// --- interface layout --------------------------------------------------------
//
// The layout engine has no reference dump to diff against, and a wrong flex line is the
// kind of thing that looks plausible in a screenshot. These are the invariants the ported
// CSS actually depends on.

void testLayout() {
  std::printf("layout\n");
  ui::Text text;  // never init()ed: measure() is not needed for fixed-size boxes
  ui::Doc doc;

  // justify-content: center on a row centres the group, not each child.
  doc.reset(nullptr);
  {
    ui::Style root = ui::Doc::row(10, ui::Justify::Center, ui::Align::Center);
    root.width = 200;
    root.height = 50;
    doc.begin(root);
    ui::Style item;
    item.width = 40;
    item.height = 20;
    doc.box(item, 1, 0);
    doc.box(item, 1, 1);
    doc.end();
  }
  doc.layout({0, 0, 200, 50});
  const ui::Rect a = doc.rectOf(1, 0);
  const ui::Rect b = doc.rectOf(1, 1);
  check(std::fabs(a.x - 55.0f) < 0.01f, "a centred row puts the group, not the child, centre");
  check(std::fabs(b.x - (a.right() + 10.0f)) < 0.01f, "the gap sits between the children");
  check(std::fabs(a.y - 15.0f) < 0.01f, "align-items: center centres on the cross axis");

  // space-between pushes the first and last child to the edges.
  doc.reset(nullptr);
  {
    ui::Style root = ui::Doc::row(0, ui::Justify::SpaceBetween, ui::Align::Center);
    root.width = 300;
    root.height = 40;
    doc.begin(root);
    ui::Style item;
    item.width = 50;
    item.height = 20;
    doc.box(item, 2, 0);
    doc.box(item, 2, 1);
    doc.end();
  }
  doc.layout({0, 0, 300, 40});
  check(doc.rectOf(2, 0).x == 0.0f, "space-between anchors the first child");
  check(std::fabs(doc.rectOf(2, 1).right() - 300.0f) < 0.01f,
        "space-between anchors the last child");

  // A block container grows to its widest child plus its own padding, which is what makes
  // a card fit its longest button.
  doc.reset(nullptr);
  {
    ui::Style card;
    card.padding = ui::Edges(10);
    const int root = doc.begin(card);
    ui::Style wide;
    wide.width = 120;
    wide.height = 20;
    doc.box(wide);
    ui::Style narrow;
    narrow.width = 60;
    narrow.height = 20;
    doc.box(narrow);
    doc.end();
    (void)root;
  }
  doc.layout({0, 0, 1000, 1000});
  check(doc.node(0).measuredW == 140.0f, "a block sizes to its widest child plus padding");
  check(doc.node(0).measuredH == 60.0f, "a block stacks its children's heights");

  // repeat(9, 46px) with a 4px gap is exactly the inventory grid.
  doc.reset(nullptr);
  {
    ui::Style grid;
    grid.display = ui::Display::Grid;
    grid.gridCols = 9;
    grid.gridColWidth = 46;
    grid.gap = 4;
    doc.begin(grid);
    for (int i = 0; i < 27; ++i) {
      ui::Style cell;
      cell.width = 46;
      cell.height = 46;
      doc.box(cell, 3, i);
    }
    doc.end();
  }
  doc.layout({0, 0, 1000, 1000});
  check(doc.node(0).measuredW == 46.0f * 9 + 4.0f * 8, "a fixed grid is columns plus gaps wide");
  check(doc.node(0).measuredH == 46.0f * 3 + 4.0f * 2, "27 slots in nine columns is three rows");
  check(doc.rectOf(3, 9).y == doc.rectOf(3, 0).y + 50.0f, "slot 9 starts the second row");
  check(doc.rectOf(3, 9).x == doc.rectOf(3, 0).x, "and returns to the first column");

  // A scroll region reports what it holds without being squashed to fit.
  doc.reset(nullptr);
  {
    ui::Style panel = ui::Doc::column(0, ui::Align::Stretch);
    panel.scrollY = true;
    panel.maxHeight = 100;
    panel.width = 200;
    doc.begin(panel, 4);
    ui::Style tall;
    tall.height = 400;
    doc.box(tall, 5);
    doc.end();
  }
  doc.layout({0, 0, 200, 1000});
  check(doc.node(0).measuredH == 100.0f, "max-height caps a scroll region");
  check(doc.rectOf(5).h == 400.0f, "overflow-y: auto does not squash its content");
  const ui::ScrollState* scroll = doc.scrollIfAny(4);
  check(scroll && std::fabs(scroll->contentHeight - 400.0f) < 0.01f,
        "the scroll state records the content height");

  // Math.round versus std::round: the bug that opened a one-pixel seam across the Atlas.
  check(jsmath::jsRound(-4.5) == -4.0, "jsRound rounds a negative half up");
  check(jsmath::jsRound(11.5) == 12.0, "jsRound rounds a positive half up");
  check(jsmath::jsRound(11.5) - jsmath::jsRound(-4.5) == 16.0,
        "so tiles sixteen apart stay sixteen apart across the origin");
  (void)text;
}

// ---------------------------------------------------------------------------
// Audio
//
// There is no golden vector for "does a stone break sound like a stone breaking",
// so what gets asserted here is the set of Web Audio behaviours a plain C++ reading
// gets wrong. Each of these was a real defect at some point in the port, or would
// have been if the DSP had been written from a textbook instead of from Blink.
// ---------------------------------------------------------------------------

// Steady-state magnitude of a filter at one frequency, measured rather than derived:
// run a sine through it long enough for the transient to die, then take the peak.
float filterGainAt(audio::FilterType type, float cutoff, float q, float probeHz, int sampleRate) {
  audio::Biquad f;
  f.set(type, cutoff, q, sampleRate);
  const int warmup = sampleRate / 8;
  const int measure = sampleRate / 16;
  float peak = 0.0f;
  for (int i = 0; i < warmup + measure; ++i) {
    const float x = std::sin(2.0f * 3.14159265f * probeHz * i / sampleRate);
    const float y = f.process(x);
    if (i >= warmup) peak = std::max(peak, std::fabs(y));
  }
  return peak;
}

// Goertzel: the energy at one frequency, for asking whether an oscillator aliased.
double toneEnergyAt(const std::vector<float>& signal, double hz, int sampleRate) {
  const double w = 2.0 * 3.14159265358979 * hz / sampleRate;
  const double coeff = 2.0 * std::cos(w);
  double s1 = 0, s2 = 0;
  for (float x : signal) {
    const double s0 = x + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2) * 2.0 / signal.size();
}

void testAudio() {
  std::printf("audio\n");
  constexpr int kRate = 48000;

  // --- BiquadFilterNode.Q is in DECIBELS for lowpass and highpass ---
  // Blink folds the parameter in as 10^(Q/20), so a resonance of 12 lifts the
  // response at the cutoff by 12 dB — a factor of four. Read as the Audio EQ
  // Cookbook's dimensionless Q it would be a factor of twelve, and every filter in
  // the game would be three times as resonant as it was tuned to be.
  const float lp12 = filterGainAt(audio::FilterType::Lowpass, 1000.0f, 12.0f, 1000.0f, kRate);
  const float lp0 = filterGainAt(audio::FilterType::Lowpass, 1000.0f, 0.0f, 1000.0f, kRate);
  checkf(std::fabs(lp12 - 4.0f) < 0.35f, "lowpass Q=12 lifts the cutoff by 12 dB (x%.2f)", lp12);
  checkf(lp0 < 1.05f, "lowpass Q=0 has no resonance at all (x%.2f)", lp0);
  const float hp12 = filterGainAt(audio::FilterType::Highpass, 1000.0f, 12.0f, 1000.0f, kRate);
  checkf(std::fabs(hp12 - 4.0f) < 0.35f, "highpass reads Q the same way (x%.2f)", hp12);

  // --- ...but bandpass Q is the conventional dimensionless one ---
  // Unity at the centre whatever Q is, and a -3 dB bandwidth of f0/Q.
  const float bpPeak = filterGainAt(audio::FilterType::Bandpass, 1000.0f, 5.0f, 1000.0f, kRate);
  checkf(std::fabs(bpPeak - 1.0f) < 0.02f, "bandpass is unity at its centre (x%.3f)", bpPeak);
  const float bpEdge = filterGainAt(audio::FilterType::Bandpass, 1000.0f, 5.0f, 1105.0f, kRate);
  checkf(std::fabs(bpEdge - 0.7071f) < 0.05f,
         "and -3 dB half a bandwidth away, so Q is dimensionless here (x%.3f)", bpEdge);

  // --- a swept filter interpolates its coefficients per sample ---
  // Holding them for a whole 128-frame quantum and stepping at the boundary puts a
  // small discontinuity into the signal 375 times a second. On the broadband noise
  // the water sounds are made of, that reads as grit rather than as a splash. It is
  // invisible in a spectrum of noise, so probe it with a pure tone instead: a 1 kHz
  // sine through a bandpass sweeping 2400 Hz down to 420 Hz should come out a 1 kHz
  // sine, and the stepped version instead grows sidebands at multiples of the block
  // rate. Both paths are run here so the ratio is the measurement.
  {
    const double sweepSeconds = 0.3;
    const int n = static_cast<int>(sweepSeconds * kRate);
    std::vector<float> smooth(n), stepped(n);
    audio::Biquad fa, fb;
    audio::Biquad::Coeffs blockA{}, blockB{}, held{};
    for (int i = 0; i < n; ++i) {
      if (i % audio::kBlockFrames == 0) {
        const double t0 = static_cast<double>(i) / kRate;
        const double t1 = std::min(sweepSeconds, t0 + audio::kBlockFrames / double(kRate));
        const double f0 = 2400.0 * std::pow(420.0 / 2400.0, t0 / sweepSeconds);
        const double f1 = 2400.0 * std::pow(420.0 / 2400.0, t1 / sweepSeconds);
        blockA = audio::Biquad::design(audio::FilterType::Bandpass, f0, 0.6, kRate);
        blockB = audio::Biquad::design(audio::FilterType::Bandpass, f1, 0.6, kRate);
        held = blockA;
      }
      const float x = std::sin(2.0f * 3.14159265f * 1000.0f * i / kRate);
      const double u = (i % audio::kBlockFrames) / double(audio::kBlockFrames);
      smooth[i] = fa.process(x, audio::Biquad::lerp(blockA, blockB, u));
      stepped[i] = fb.process(x, held);
    }
    const double sideSmooth = toneEnergyAt(smooth, 1375.0, kRate) + toneEnergyAt(smooth, 625.0, kRate);
    const double sideStepped =
        toneEnergyAt(stepped, 1375.0, kRate) + toneEnergyAt(stepped, 625.0, kRate);
    // About 5 dB on this probe. Part of what sits at 1000 +/- 375 Hz is the sweep's
    // own amplitude modulation of the tone, which is present either way and puts a
    // floor under the ratio — so this is a smaller number than the fix deserves,
    // and an honest one.
    checkf(sideSmooth < sideStepped * 0.7,
           "per-sample filter coefficients cut the block-rate zipper (%.5f vs %.5f)", sideSmooth,
           sideStepped);
    checkf(toneEnergyAt(smooth, 1000.0, kRate) > 0.1,
           "and the tone itself still comes through (%.3f)", toneEnergyAt(smooth, 1000.0, kRate));
  }

  // --- sawtooth and triangle are band-limited ---
  // A 7 kHz sawtooth at 48 kHz has real harmonics at 7, 14 and 21 kHz. A naive
  // sign/ramp oscillator also folds its 7th harmonic (49 kHz) down to 1 kHz, which
  // is audible as a buzz an octave and a half below the note. The wavetable does not.
  audio::PeriodicWave saw;
  saw.build(audio::Wave::Sawtooth, kRate);
  std::vector<float> tone(kRate / 4);
  {
    const audio::PeriodicWave::Reader r = saw.reader(7000.0);
    double phase = 0.0;
    const double inc = 7000.0 * saw.tableSize() / kRate;
    for (float& v : tone) {
      v = audio::PeriodicWave::read(r, phase);
      phase += inc;
      while (phase >= saw.tableSize()) phase -= saw.tableSize();
    }
  }
  const double alias = toneEnergyAt(tone, 1000.0, kRate);
  const double fundamental = toneEnergyAt(tone, 7000.0, kRate);
  checkf(fundamental > 0.3, "the band-limited sawtooth has its fundamental (%.3f)", fundamental);
  checkf(alias < fundamental * 0.02,
         "and nothing where a naive one would alias (%.5f vs %.3f)", alias, fundamental);
  checkf(std::fabs(saw.reader(100.0).high[0]) < 0.01f,
         "a sawtooth starts at zero with a positive slope");

  // --- AudioParam ---
  audio::AudioParam env;
  env.reset(1.0f, 0.0);  // a GainNode's default value is 1
  env.setValueAtTime(0.0001f, 1.0);
  env.linearRampToValueAtTime(0.5f, 1.005);
  env.exponentialRampToValueAtTime(0.0001f, 1.2);
  // The boundary that matters: a voice's first sample lands at exactly its first
  // event's time. Returning the GainNode default there instead of the envelope's
  // floor puts one render quantum of full-scale signal at the head of every sound.
  checkf(env.valueAt(1.0) < 0.001f, "setValueAtTime applies AT its own time (%.4f)",
         env.valueAt(1.0));
  checkf(std::fabs(env.valueAt(1.0025) - 0.25f) < 0.01f, "a linear ramp is linear (%.3f)",
         env.valueAt(1.0025));
  // An exponential ramp passes through the GEOMETRIC mean at the halfway point, not
  // the arithmetic one: sqrt(0.5 * 0.0001) = 0.00707, not 0.25.
  checkf(std::fabs(env.valueAt(1.1025) - 0.00707f) < 0.0005f,
         "an exponential ramp is exponential (%.5f)", env.valueAt(1.1025));

  // setTargetAtTime is `target + (v0 - target) * exp(-dt/tau)`, so one tau covers
  // 63.2% of the distance — not all of it, which is what a linear reading assumes.
  audio::Smoother smooth;
  smooth.snap(0.0f);
  smooth.setTarget(1.0f, 0.5f);
  smooth.advance(0.5f);
  checkf(std::fabs(smooth.value() - 0.6321f) < 0.001f,
         "setTargetAtTime covers 1-1/e of the distance in one tau (%.4f)", smooth.value());

  // --- the panner ---
  audio::ListenerPose listener;  // at the origin, facing -Z
  const float ahead[3] = {0.0f, 0.0f, -3.0f};
  const audio::PanGains centre = audio::equalPowerPan(listener, ahead, 3.0f, 60.0f, 1.6f);
  checkf(std::fabs(centre.left - 0.7071f) < 0.01f && std::fabs(centre.right - 0.7071f) < 0.01f,
         "equal-power pans a source dead ahead to cos(45) per channel (%.3f)", centre.left);
  const float right[3] = {3.0f, 0.0f, 0.0f};
  const audio::PanGains hardRight = audio::equalPowerPan(listener, right, 3.0f, 60.0f, 1.6f);
  check(hardRight.right > 0.99f && hardRight.left < 0.01f, "and hard right to one channel");
  const float far[3] = {0.0f, 0.0f, -26.0f};
  const audio::PanGains distant = audio::equalPowerPan(listener, far, 3.0f, 60.0f, 1.6f);
  const float falloff = std::sqrt(distant.left * distant.left + distant.right * distant.right);
  checkf(falloff < 0.11f, "inverse falloff makes 26 blocks nearly inaudible (%.3f)", falloff);

  // --- the mixer, end to end ---
  audio::Engine& e = audio::engine();
  e.startOffline(kRate);
  e.updateListener(Vec3{0, 0, 0}, 0.0f);
  // The shipped defaults, squared inside the engine.
  e.setVolumes(0.8f, 0.8f, 0.6f, 0.5f);
  std::vector<float> mix(static_cast<std::size_t>(kRate) * 2, 0.0f);
  e.renderOffline(mix.data(), kRate / 10);

  // A voice whose start time falls BETWEEN two frames must not overshoot. Rounding
  // its start frame down puts the first sample before the envelope's own first
  // event, where a gain parameter correctly reports the GainNode default of 1.0 —
  // one render quantum of full-scale signal at the head of the sound. It is a coin
  // flip per voice, so it only ever showed on the recipes whose delays come out of
  // R(): the splash's droplets, the stone-break rubble, the three bites of eat.
  {
    e.startOffline(kRate);
    e.updateListener(Vec3{0, 0, 0}, 0.0f);
    e.setVolumes(1.0f, 1.0f, 1.0f, 1.0f);
    e.renderOffline(mix.data(), kRate / 20);
    const audio::Dest d = e.out(audio::Bus::Sfx);
    e.tone(d, {.delay = 0.05f + 0.5f / kRate, .freq = 900.0f, .dur = 0.1f, .gain = 0.2f});
    float tonePeak = 0.0f;
    for (int i = 0; i < 4; ++i) {
      e.renderOffline(mix.data(), kRate / 10);
      for (int k = 0; k < kRate / 10 * 2; ++k) tonePeak = std::max(tonePeak, std::fabs(mix[k]));
    }
    // 0.2 through the compressor's makeup is about 0.27; the bug gave about 1.35.
    checkf(tonePeak > 0.15f && tonePeak < 0.45f,
           "a voice starting between two frames keeps to its envelope (%.3f)", tonePeak);
  }

  // The 32-event cap is on EVENTS, not voices: one block break is ten voices.
  e.startOffline(kRate);
  int accepted = 0;
  for (int i = 0; i < 64; ++i) {
    if (e.tryVoice(1.0f)) ++accepted;
  }
  checkf(accepted == 32, "the voice cap admits exactly 32 events (%d)", accepted);

  // Headroom at the default volumes, with the loudest thing in the library.
  e.startOffline(kRate);
  e.updateListener(Vec3{0, 0, 0}, 0.0f);
  e.setVolumes(0.8f, 0.8f, 0.6f, 0.5f);
  e.renderOffline(mix.data(), kRate / 20);
  audio::sfx::splash(true);
  audio::sfx::blockBreak(world::blocks().def(world::blocks().idOf("glass")), Vec3{0, 0, -3});
  float peak = 0.0f;
  for (int i = 0; i < 10; ++i) {
    e.renderOffline(mix.data(), kRate / 10);
    for (int k = 0; k < kRate / 10 * 2; ++k) peak = std::max(peak, std::fabs(mix[k]));
  }
  checkf(peak < 1.0f, "the two loudest events together stay under full scale (%.3f)", peak);
  checkf(peak > 0.1f, "and are not silent (%.3f)", peak);
  checkf(e.activeVoices() == 0, "every voice is reaped once its tail ends (%d left)",
         e.activeVoices());

  // The underwater sweep. `19000 * pow(750/19000, t)` is exponential in t, so it is
  // 19 kHz in air and 750 Hz fully submerged, and the master lowpass sitting at
  // those two ends is either transparent or dramatic.
  checkf(std::fabs(19000.0 * std::pow(750.0 / 19000.0, 1.0) - 750.0) < 0.5,
         "the underwater muffle bottoms out at 750 Hz");
  const float air = filterGainAt(audio::FilterType::Lowpass, 19000.0f, 0.5f, 4000.0f, kRate);
  const float sub = filterGainAt(audio::FilterType::Lowpass, 750.0f, 0.5f, 4000.0f, kRate);
  checkf(air > 0.95f, "in air it leaves 4 kHz alone (x%.3f)", air);
  checkf(sub < 0.05f, "submerged it takes 4 kHz down by over 26 dB (x%.4f)", sub);

  // The three noise buffers, which nothing else would notice were empty: crackle in
  // particular is sparse by design and drives every fire and the zombie sizzle.
  audio::NoiseBank noise;
  noise.build(kRate);
  for (auto kind : {audio::NoiseKind::White, audio::NoiseKind::Pink, audio::NoiseKind::Crackle}) {
    const std::vector<float>& buf = noise.buffer(kind);
    double energy = 0.0;
    float loudest = 0.0f;
    for (float v : buf) {
      energy += static_cast<double>(v) * v;
      loudest = std::max(loudest, std::fabs(v));
    }
    checkf(buf.size() == static_cast<std::size_t>(kRate) * 2 && energy > 0.0 && loudest > 0.05f,
           "the %s noise buffer is 2 s of actual signal (peak %.2f)",
           kind == audio::NoiseKind::White ? "white"
           : kind == audio::NoiseKind::Pink ? "pink"
                                            : "crackle",
           loudest);
  }
}

// ---------------------------------------------------------------------------
// Entities and AI
//
// Everything here runs without a window. A* is pure world queries, and the mobs
// only ever touch the world, the player and the inventory, so a scripted tick is
// indistinguishable from a real one.
// ---------------------------------------------------------------------------

// A wider, taller pocket than the shared one, so a pathfinder has room to work.
std::unique_ptr<world::World> makeArena() {
  auto w = std::make_unique<world::World>(3918175327u, 2);
  w->primeSpawn(kOriginX, kOriginZ);
  for (int x = -2; x <= 24; ++x) {
    for (int z = -2; z <= 24; ++z) {
      // A floor at kY - 1 and six clear blocks above it.
      w->setBlock(x, kY - 1, z, world::wk().greystone, 0);
      for (int y = kY; y <= kY + 5; ++y) w->setBlock(x, y, z, world::kAir, 0);
    }
  }
  return w;
}

void testEntities() {
  std::printf("entities and AI\n");

  // --- A* --------------------------------------------------------------------
  {
    auto world = makeArena();
    // A wall clean across the arena at z = 8, with a single gap at x = 12. It has
    // to span the full width: leave an end open and the route goes round it, which
    // proves nothing about pathing through a gap.
    for (int x = -2; x <= 24; ++x) {
      if (x == 12) continue;
      for (int y = kY; y <= kY + 2; ++y) {
        world->setBlock(x, y, 8, world::wk().greystone, 0);
      }
    }
    game::PathOptions options;
    options.maxDist = 32;
    options.maxExpand = 4000;
    game::Path path;
    const bool ok = game::findPath(*world, Vec3{4.5f, kY, 4.5f}, Vec3{4.5f, kY, 14.5f}, options,
                                   nullptr, path);
    check(ok && path.found, "A* finds a way through a wall with one gap");
    bool throughGap = false;
    bool clippedWall = false;
    for (const game::PathPoint& p : path.points) {
      const int px = static_cast<int>(p.x), pz = static_cast<int>(p.z);
      if (pz == 8 && px == 12) throughGap = true;
      if (pz == 8 && px != 12) clippedWall = true;
    }
    check(throughGap, "and the route goes through the gap");
    check(!clippedWall, "without walking through the wall itself");

    // Walling the gap up makes the goal unreachable; the search must still hand
    // back a partial route so a chasing mob starts moving somewhere useful.
    for (int y = kY; y <= kY + 2; ++y) {
      world->setBlock(12, y, 8, world::wk().greystone, 0);
    }
    game::Path blocked;
    const bool ok2 = game::findPath(*world, Vec3{4.5f, kY, 4.5f}, Vec3{4.5f, kY, 14.5f},
                                    options, nullptr, blocked);
    check(ok2 && !blocked.found, "a sealed goal reports the path as partial");
    check(!blocked.points.empty(), "but still returns a route toward it");

    // The budget is shared, so one mob's search cannot eat the whole frame.
    game::PathBudget budget{40};
    game::Path budgeted;
    game::findPath(*world, Vec3{4.5f, kY, 4.5f}, Vec3{20.5f, kY, 20.5f}, options, &budget,
                   budgeted);
    checkf(budget.left <= 0, "the shared A* budget is spent down (%d left)", budget.left);
  }

  // A one-block step up, which the manager's auto-step climbs.
  {
    auto world = makeArena();
    for (int z = 0; z <= 16; ++z) {
      world->setBlock(8, kY, z, world::wk().greystone, 0);
    }
    game::PathOptions options;
    options.maxExpand = 2000;
    game::Path path;
    game::findPath(*world, Vec3{4.5f, kY, 8.5f}, Vec3{12.5f, kY, 8.5f}, options, nullptr, path);
    bool steppedUp = false;
    for (const game::PathPoint& p : path.points) {
      if (p.hint == game::StepHint::Up) steppedUp = true;
    }
    check(path.found && steppedUp, "A* steps up over a one-block ledge");
  }

  // --- drops -----------------------------------------------------------------
  {
    auto world = makeArena();
    game::Player player(kOriginX, static_cast<float>(kY), kOriginZ);
    game::Inventory inv;
    game::EntityManager entities;
    game::EntityContext ctx;
    ctx.world = world.get();
    ctx.player = &player;
    ctx.inventory = &inv;
    ctx.entities = &entities;

    // Two like drops within merge range become one.
    entities.spawnDrop(Vec3{6.5f, static_cast<float>(kY), 6.5f}, "greystone", 3, -1);
    entities.spawnDrop(Vec3{6.9f, static_cast<float>(kY), 6.5f}, "greystone", 4, -1);
    // Freeze them where they are: the merge test is about the rule, not physics.
    for (game::Entity& e : entities.all()) e.vel = Vec3{};
    for (int i = 0; i < 40; ++i) entities.tick(1.0f / 60.0f, ctx);
    check(inv.countOf("greystone") == 7, "two like drops merge and vacuum as one stack");
    check(entities.count() == 0, "and both entities are gone");

    // A tossed drop out of reach waits to be walked over.
    inv.clear();
    entities.spawnTossed(Vec3{20.5f, static_cast<float>(kY), 20.5f}, Vec3{0, 0, 0}, "planks", 2,
                         -1);
    for (int i = 0; i < 120; ++i) entities.tick(1.0f / 60.0f, ctx);
    check(inv.countOf("planks") == 0, "a tossed drop is not vacuumed from across the room");
    check(entities.count() == 1, "and it is still lying there");
    // Walk over it.
    player.setPos(Vec3{20.5f, static_cast<float>(kY), 20.5f});
    for (int i = 0; i < 20; ++i) entities.tick(1.0f / 60.0f, ctx);
    check(inv.countOf("planks") == 2, "walking onto it picks it up");
  }

  // --- grazers ---------------------------------------------------------------
  {
    auto world = makeArena();
    game::Player player(10.5f, static_cast<float>(kY), 10.5f);
    game::Inventory inv;
    game::EntityManager entities;
    game::EntityContext ctx;
    ctx.world = world.get();
    ctx.player = &player;
    ctx.inventory = &inv;
    ctx.entities = &entities;

    game::Entity* sheep = entities.spawn(game::EntityType::Sheep, Vec3{12.5f, kY, 10.5f});
    check(sheep && sheep->data.health == 8.0f, "a sheep spawns with its full health");

    // Spooked, it runs directly away from the player.
    const float before = sheep->pos.x;
    sheep->data.flee = 4.0f;
    sheep->onGround = true;
    for (int i = 0; i < 60; ++i) entities.tick(1.0f / 60.0f, ctx);
    game::Entity* after = entities.byId(sheep->id);
    checkf(after && after->pos.x > before + 0.3f,
           "a fleeing sheep runs away from the player (%.2f -> %.2f)", before,
           after ? after->pos.x : 0.0f);

    // A hit hurts it, makes it flee, and knocks it back.
    after->data.flee = 0.0f;
    const float health = after->data.health;
    const game::EntityDef* def = game::defOf(game::EntityType::Sheep);
    def->interact(*after, ctx, game::InteractButton::Left);
    check(after->data.health < health, "hitting a sheep damages it");
    check(after->data.flee > 0.0f && after->data.hurtFlash > 0.0f,
          "and sets it fleeing with a hurt flash");
    check(after->vel.y > 0.0f, "and knocks it into the air");

    // Enough hits kill it and it drops its wool.
    for (int i = 0; i < 12 && !after->dead; ++i) {
      def->interact(*after, ctx, game::InteractButton::Left);
    }
    check(after->dead, "enough hits kill it");
  }

  // --- the zombie ------------------------------------------------------------
  {
    auto world = makeArena();
    game::Player player(8.5f, static_cast<float>(kY), 8.5f);
    game::Inventory inv;
    game::EntityManager entities;
    render::Sky sky;
    game::EntityContext ctx;
    ctx.world = world.get();
    ctx.player = &player;
    ctx.inventory = &inv;
    ctx.entities = &entities;
    ctx.sky = &sky;

    // Line of sight: clear across the arena, blocked by a wall.
    const Vec3 eye{8.5f, kY + 1.62f, 8.5f};
    const Vec3 far{16.5f, kY + 1.5f, 8.5f};
    check(game::lineOfSight(*world, eye, far, 32.0f), "line of sight is clear across open air");
    for (int y = kY; y <= kY + 2; ++y) {
      world->setBlock(12, y, 8, world::wk().greystone, 0);
    }
    check(!game::lineOfSight(*world, eye, far, 32.0f), "and a wall blocks it");

    // Burning: exposed to full daylight, a zombie loses health once a second.
    world->setBlock(12, kY, 8, world::kAir, 0);
    world->setBlock(12, kY + 1, 8, world::kAir, 0);
    world->setBlock(12, kY + 2, 8, world::kAir, 0);
    game::Entity* zombie = entities.spawn(game::EntityType::Zombie, Vec3{16.5f, kY, 16.5f});
    check(zombie && zombie->data.health == 16.0f, "a zombie spawns with its full health");
    sky.time = 0.5f;  // noon
    checkf(sky.dayFactor() > 0.5f, "noon is full daylight (%.2f)", sky.dayFactor());
    const float startHealth = zombie->data.health;
    for (int i = 0; i < 90; ++i) entities.tick(1.0f / 60.0f, ctx);
    game::Entity* burned = entities.byId(zombie->id);
    checkf(!burned || burned->data.health < startHealth,
           "a zombie in direct sunlight burns (%.1f -> %.1f)", startHealth,
           burned ? burned->data.health : 0.0f);
  }

  // --- the boat --------------------------------------------------------------
  {
    auto world = makeArena();
    // A pool three deep at the arena's centre.
    for (int x = 6; x <= 14; ++x) {
      for (int z = 6; z <= 14; ++z) {
        for (int y = kY; y <= kY + 2; ++y) {
          world->setBlock(x, y, z, world::wk().water, 0);
        }
      }
    }
    game::Player player(2.5f, static_cast<float>(kY), 2.5f);
    game::Inventory inv;
    game::EntityManager entities;
    Input input;
    game::EntityContext ctx;
    ctx.world = world.get();
    ctx.player = &player;
    ctx.inventory = &inv;
    ctx.entities = &entities;
    ctx.input = &input;

    // Dropped in at the bottom of the pool, it floats up to the surface.
    game::Entity* boat = entities.spawn(game::EntityType::Boat, Vec3{10.5f, kY, 10.5f});
    check(boat != nullptr, "a boat spawns");
    const float sank = boat->pos.y;
    for (int i = 0; i < 120; ++i) entities.tick(1.0f / 60.0f, ctx);
    game::Entity* afloat = entities.byId(boat->id);
    checkf(afloat && afloat->pos.y > sank + 1.0f, "and floats up to the surface (%.2f -> %.2f)",
           sank, afloat ? afloat->pos.y : 0.0f);

    // Mounted, it carries the player in its seat.
    const game::EntityDef* def = game::defOf(game::EntityType::Boat);
    def->interact(*afloat, ctx, game::InteractButton::Right);
    check(afloat->data.rider && player.mount() == afloat->id, "right-clicking mounts it");
    for (int i = 0; i < 10; ++i) entities.tick(1.0f / 60.0f, ctx);
    game::Entity* ridden = entities.byId(afloat->id);
    checkf(ridden && std::fabs(player.pos().y - (ridden->pos.y + 0.25f)) < 0.01f,
           "and the rider sits in the seat (%.2f vs %.2f)", player.pos().y,
           ridden ? ridden->pos.y + 0.25f : 0.0f);
  }

  // --- picking ---------------------------------------------------------------
  {
    auto world = makeArena();
    game::EntityManager entities;
    entities.spawn(game::EntityType::Cow, Vec3{14.5f, kY, 8.5f});
    game::EntityRayHit hit;
    // Facing +x from the arena's west side, level with the cow's body.
    const bool found =
        entities.raycast(Vec3{8.5f, kY + 0.7f, 8.5f}, Vec3{1, 0, 0}, 12.0f, hit);
    checkf(found && hit.entity && hit.entity->type == game::EntityType::Cow,
           "a ray picks the cow in front of it (%.2f blocks)", found ? hit.dist : -1.0f);
    game::EntityRayHit miss;
    check(!entities.raycast(Vec3{8.5f, kY + 0.7f, 8.5f}, Vec3{0, 0, 1}, 12.0f, miss),
          "and misses when it is aimed elsewhere");
  }
}

// --- saves -------------------------------------------------------------------

// A save with something in every section, so a round-trip has something to lose.
save::WorldSave makeSave() {
  save::WorldSave s;
  s.meta.id = "wtest0001";
  s.meta.name = "Round Trip";
  s.meta.seed = 3918175327u;
  s.meta.genVersion = world::kGenVersion;
  s.meta.createdAt = 1700000000;
  s.meta.savedAt = 1700003600;
  s.meta.gameVersion = HR_VERSION;
  s.meta.time = 0.7125f;
  s.meta.hasSpawn = true;
  s.meta.spawn = Vec3{12.5f, 101.0f, -7.5f};

  s.player.pos = Vec3{8.5f, 100.0f, -3.25f};
  s.player.yaw = kYawPlusX;
  s.player.pitch = -0.25f;
  s.player.health = 13.5f;
  s.player.hunger = 17.0f;
  s.player.saturation = 2.5f;
  s.player.flying = true;

  s.inventory.give("greystone", 42);
  s.inventory.give("stone_pickaxe", 1);
  s.inventory.slots()[20] = game::ItemStack{"torch", 7, -1};
  s.inventory.armor()[0] = game::ItemStack{"leather_cap", 1, 33};
  s.inventory.setSelected(4);

  // Two chunks, one of them across the origin so the signed key round-trips.
  s.edits[world::chunkKey(0, 0)][world::localIdx(1, 64, 2)] =
      static_cast<std::uint32_t>(world::wk().greystone);
  s.edits[world::chunkKey(0, 0)][world::localIdx(3, 65, 4)] =
      static_cast<std::uint32_t>(world::wk().log) | (5u << 16);
  s.edits[world::chunkKey(-2, -3)][world::localIdx(15, 127, 15)] =
      static_cast<std::uint32_t>(world::wk().water) | (3u << 16);

  s.explored = {world::chunkKey(0, 0), world::chunkKey(-2, -3), world::chunkKey(9, -9)};

  game::BlockEntity chest = game::makeChest();
  chest.slots[2] = game::ItemStack{"planks", 31, -1};
  s.blockEntities[game::blockEntityKey(-5, 70, 12)] = chest;
  game::BlockEntity forge = game::makeForge();
  forge.input = game::ItemStack{"iron_ore", 3, -1};
  forge.fuel = game::ItemStack{"coal", 8, -1};
  forge.fuelLeft = 4.25f;
  forge.fuelMax = 12.0f;
  forge.progress = 1.5f;
  s.blockEntities[game::blockEntityKey(6, 71, -2)] = forge;

  game::EntitySave cow;
  cow.type = game::EntityType::Cow;
  cow.pos = Vec3{4.5f, 100.0f, 6.5f};
  cow.vel = Vec3{0.25f, -0.5f, 0.0f};
  cow.yaw = 1.25f;
  cow.health = 7.0f;
  // A state-machine record, which no shipped mob produces yet — this is the field
  // M8 deferred into this milestone, so the format has to carry it before anything
  // depends on it.
  game::FsmRecord rec;
  rec.slot = "root";
  rec.state = "graze";
  rec.timeIn = 2.5f;
  rec.bb.set("target", 17.0f);
  rec.bb.set("_cd_moo", 0.75f);
  cow.fsm.push_back(rec);
  s.entities.push_back(cow);

  game::EntitySave drop;
  drop.type = game::EntityType::Drop;
  drop.pos = Vec3{9.5f, 100.0f, 9.5f};
  drop.key = "iron_ingot";
  drop.count = 5;
  drop.dura = -1;
  drop.despawn = 412.5f;
  drop.instant = false;
  s.entities.push_back(drop);

  save::WaypointSave point;
  point.x = 128.0f;
  point.y = 72.0f;
  point.z = -64.0f;
  point.name = "Home \xE2\x80\x94 the big oak";
  point.color = 0xE8C84AFFu;
  point.death = true;
  s.waypoints.push_back(point);
  return s;
}

void testSaves() {
  std::printf("saves\n");

  // --- round trip --------------------------------------------------------------
  const save::WorldSave original = makeSave();
  const std::vector<std::uint8_t> bytes = save::encode(original);
  check(bytes.size() > 64, "a world encodes to a non-trivial file");

  save::WorldSave back;
  std::string error;
  const bool decoded = save::decode(bytes.data(), bytes.size(), back, &error);
  checkf(decoded, "it decodes again%s%s", decoded ? "" : ": ", error.c_str());
  if (!decoded) return;

  // Re-encoding is the strongest single check available: it compares every byte of
  // every section rather than the fields somebody remembered to compare by hand,
  // and it only holds because the encoder sorts every map it writes.
  const std::vector<std::uint8_t> again = save::encode(back);
  check(again == bytes, "and re-encodes to byte-identical output");

  checkf(back.meta.name == original.meta.name && back.meta.seed == original.meta.seed &&
             back.meta.genVersion == original.meta.genVersion &&
             back.meta.createdAt == original.meta.createdAt &&
             back.meta.gameVersion == original.meta.gameVersion,
         "the metadata survives (\"%s\", seed %u, gen v%d)", back.meta.name.c_str(),
         back.meta.seed, back.meta.genVersion);
  check(back.meta.time == original.meta.time && back.meta.hasSpawn &&
            back.meta.spawn.y == original.meta.spawn.y,
        "the sky clock and the bound spawn point survive");
  check(back.player.pos.z == original.player.pos.z && back.player.health == 13.5f &&
            back.player.hunger == 17.0f && back.player.saturation == 2.5f &&
            back.player.flying,
        "the player survives, flying and part-fed");
  check(back.inventory.countOf("greystone") == 42 && back.inventory.selected() == 4 &&
            back.inventory.slots()[20].key == "torch" &&
            back.inventory.armor()[0].dura == 33,
        "the inventory survives, worn armour and selected slot included");

  // --- edits, and the palette that keeps them honest ---------------------------
  check(back.edits.size() == 2, "both edited chunks survive");
  {
    const auto& far = back.edits.at(world::chunkKey(-2, -3));
    const auto it = far.find(world::localIdx(15, 127, 15));
    const bool found = it != far.end();
    checkf(found && (it->second & 0xFFFF) == world::wk().water &&
               ((it->second >> 16) & 0xFF) == 3,
           "a cell in a negative-coordinate chunk keeps its block and metadata");
  }
  {
    // The point of a string-keyed palette: what comes back is resolved through the
    // registry by key, so it tracks the block table rather than a frozen id.
    const auto& home = back.edits.at(world::chunkKey(0, 0));
    const auto it = home.find(world::localIdx(3, 65, 4));
    check(it != home.end() &&
              (it->second & 0xFFFF) == world::BlockRegistry::get().idOf("log"),
          "an edited block resolves back through the registry by key");
  }
  check(back.explored.size() == 3, "the explored set survives");

  // --- containers, entities, waypoints -----------------------------------------
  {
    const auto chest = back.blockEntities.find(game::blockEntityKey(-5, 70, 12));
    const bool ok = chest != back.blockEntities.end() &&
                    chest->second.kind == game::BlockEntityKind::Chest &&
                    chest->second.slots.size() == game::kChestSlots &&
                    chest->second.slots[2].key == "planks" &&
                    chest->second.slots[2].count == 31;
    check(ok, "a chest survives with its contents in the right slots");
    const auto forge = back.blockEntities.find(game::blockEntityKey(6, 71, -2));
    check(forge != back.blockEntities.end() &&
              forge->second.kind == game::BlockEntityKind::Forge &&
              forge->second.fuelLeft == 4.25f && forge->second.progress == 1.5f,
          "a forge survives mid-smelt");
  }
  check(back.entities.size() == 2 && back.entities[0].type == game::EntityType::Cow &&
            back.entities[0].health == 7.0f && back.entities[1].key == "iron_ingot" &&
            back.entities[1].count == 5,
        "both entities survive with their whitelisted fields");
  {
    const auto& fsm = back.entities[0].fsm;
    const bool ok = fsm.size() == 1 && fsm[0].slot == "root" && fsm[0].state == "graze" &&
                    fsm[0].timeIn == 2.5f && fsm[0].bb.get("target") == 17.0f &&
                    fsm[0].bb.get("_cd_moo") == 0.75f;
    check(ok, "and a state-machine record comes back with its blackboard");
  }
  check(back.waypoints.size() == 1 && back.waypoints[0].color == 0xE8C84AFFu &&
            back.waypoints[0].death && back.waypoints[0].name == original.waypoints[0].name,
        "a death waypoint survives with its colour and its UTF-8 name");

  // --- the entity manager's own whitelist and load fix-ups ---------------------
  {
    game::EntityManager manager;
    manager.load(back.entities);
    const game::Entity* cow = nullptr;
    const game::Entity* drop = nullptr;
    for (const game::Entity& e : manager.all()) {
      if (e.type == game::EntityType::Cow) cow = &e;
      if (e.type == game::EntityType::Drop) drop = &e;
    }
    check(cow && cow->data.health == 7.0f && cow->pos.z == 6.5f && cow->vel.x == 0.25f,
          "a loaded cow keeps its health, position and velocity");
    check(drop && drop->data.pickupDelay == 0.0f && drop->data.count == 5 &&
              !drop->data.instant,
          "a loaded drop is collectable straight away and still a tossed one");

    game::EntityManager fresh;
    std::vector<game::EntitySave> broken = back.entities;
    broken[0].health = -5.0f;  // a hand-edited or corrupt file
    fresh.load(broken);
    const game::Entity* healed = nullptr;
    for (const game::Entity& e : fresh.all()) {
      if (e.type == game::EntityType::Cow) healed = &e;
    }
    checkf(healed && healed->data.health == 12.0f,
           "a cow with nonsense health loads at full (%.1f)",
           healed ? healed->data.health : -1.0f);
  }
  {
    // Ghosts belong to the network, not to the world.
    game::EntityManager manager;
    manager.spawn(game::EntityType::Cow, Vec3{0, 100, 0});
    manager.spawnGhost(7, game::EntityType::RemotePlayer, Vec3{1, 100, 1});
    game::Entity* dead = manager.spawn(game::EntityType::Pig, Vec3{2, 100, 2});
    if (dead) dead->dead = true;
    check(manager.serialize().size() == 1, "serializing skips ghosts and the dead");
  }

  // --- an edited world reloads to the same blocks ------------------------------
  // The milestone's own criterion, run through the real World rather than the save
  // struct: the edits go back in before the first chunk streams, and generateChunk
  // replays them over freshly generated terrain.
  {
    auto live = makeWorld();
    live->setBlock(6, kY, 6, world::wk().greystone, 0);
    live->setBlock(7, kY, 6, world::wk().log, 2);
    live->setBlock(8, kY, 6, world::kAir, 0);  // a mined cell is an edit too

    save::WorldSave shot;
    shot.meta.id = "wroundtrip";
    shot.meta.seed = live->seed();
    shot.meta.genVersion = live->genVersion();
    shot.edits = live->edits();
    const std::vector<std::uint8_t> raw = save::encode(shot);

    save::WorldSave loaded;
    if (save::decode(raw.data(), raw.size(), loaded, &error)) {
      world::World rebuilt(loaded.meta.seed, 2, loaded.meta.genVersion);
      rebuilt.setEdits(loaded.edits);
      rebuilt.primeSpawn(kOriginX, kOriginZ);
      check(rebuilt.getBlock(6, kY, 6) == world::wk().greystone,
            "a placed block is still there after a save and a reload");
      check(rebuilt.getBlock(7, kY, 6) == world::wk().log &&
                rebuilt.getMeta(7, kY, 6) == 2,
            "and keeps its orientation metadata");
      check(rebuilt.getBlock(8, kY, 6) == world::kAir,
            "a mined block stays mined rather than regenerating");
    } else {
      check(false, "an edited world round-trips");
    }
  }

  // --- where a fresh world puts you --------------------------------------------
  // Not a save field, but the thing a save's absence falls back to, and the reason
  // this milestone found it: a played session's save came back with the health bar
  // most of the way down, because the default spawn column for the default seed is
  // under the sea and the port had been dropping the player two blocks above the
  // seabed rather than above the water (js/world/world.js:555).
  {
    world::World w(3918175327u, 1);
    const int sea = world::seaLevel(w.genVersion());
    const int ground = world::heightAt(w.noise(), 8, 8, w.genVersion());
    checkf(ground < sea, "the default spawn column really is under the sea (%d < %d)",
           ground, sea);
    checkf(w.spawnHeight(8, 8) == sea + 2,
           "so spawnHeight puts the player above the water, not above the seabed (%d)",
           w.spawnHeight(8, 8));
    // Dry land is unaffected: still two blocks above the ground it found.
    int dryX = 0, dryZ = 0, dryGround = 0;
    for (int i = 0; i < 400 && dryGround <= sea; ++i) {
      dryX = 40 + i * 7;
      dryZ = 40 - i * 5;
      dryGround = world::heightAt(w.noise(), dryX, dryZ, w.genVersion());
    }
    checkf(dryGround > sea && w.spawnHeight(dryX, dryZ) == dryGround + 2,
           "and leaves dry land alone (ground %d -> spawn %d)", dryGround,
           w.spawnHeight(dryX, dryZ));
  }

  // --- rejecting what should be rejected ---------------------------------------
  {
    save::WorldSave junk;
    check(!save::decode(nullptr, 0, junk, nullptr), "an empty buffer is rejected");

    const std::vector<std::uint8_t> garbage(512, 0xA5);
    check(!save::decode(garbage.data(), garbage.size(), junk, nullptr),
          "a file of garbage is rejected");

    // Truncation at every length, not just one: each cut lands in a different
    // field, and a reader that checks its bounds in only some of them passes a
    // single-point test.
    int survived = 0;
    for (std::size_t cut = 1; cut < bytes.size(); cut += 7) {
      save::WorldSave partial;
      if (save::decode(bytes.data(), cut, partial, nullptr)) ++survived;
    }
    checkf(survived == 0, "every truncation of a valid save is rejected (%d survived)",
           survived);

    // A single flipped bit anywhere in the payload.
    std::vector<std::uint8_t> corrupt = bytes;
    corrupt[corrupt.size() / 2] ^= 0x40;
    check(!save::decode(corrupt.data(), corrupt.size(), junk, nullptr),
          "a single flipped bit is caught by the payload CRC");

    // A version this build cannot know the layout of. The web build warned and
    // loaded anyway, which is safe for JSON and not for this.
    std::vector<std::uint8_t> future = bytes;
    future[8] = 99;
    std::string why;
    check(!save::decode(future.data(), future.size(), junk, &why) &&
              why.find("newer") != std::string::npos,
          "a save from a newer build is refused, with a reason");
  }
  {
    // An edit naming a block this build does not have becomes air, as the web
    // build's `BLOCK[blockKey] ?? AIR` did.
    save::WorldSave odd;
    odd.meta.id = "wunknown01";
    odd.meta.seed = 1;
    odd.edits[world::chunkKey(0, 0)][world::localIdx(0, 64, 0)] = 60000u;  // no such id
    const std::vector<std::uint8_t> raw = save::encode(odd);
    save::WorldSave back2;
    const bool ok = save::decode(raw.data(), raw.size(), back2, nullptr);
    const auto& cells = back2.edits[world::chunkKey(0, 0)];
    const auto it = cells.find(world::localIdx(0, 64, 0));
    check(ok && it != cells.end() && (it->second & 0xFFFF) == world::kAir,
          "an unknown block key loads as air rather than as whatever id it had");
  }

  // --- ids are filenames, so they are not trusted ------------------------------
  check(save::validId("wm8k2x0z9"), "a generated id is a valid one");
  check(!save::validId("../../etc/passwd") && !save::validId("a/b") &&
            !save::validId("C:evil") && !save::validId(""),
        "a path traversal is not a valid world id");
  {
    const std::string a = save::newId();
    const std::string b = save::newId();
    check(save::validId(a) && a != b, "newId produces distinct, valid ids");
  }
  check(save::safeFileName("Hollow / Reach: v2!") == "Hollow_Reach_v2_" &&
            save::safeFileName("///") == "_",
        "a world name reduces to a safe file name");
}

// --- water -------------------------------------------------------------------

// Runs the automaton to a standstill. Each call to tick() only does work once the
// accumulator passes the period, so the dt is one full tick and the loop is
// bounded — a field that will not settle is a bug, not something to wait out.
int settleWater(world::World& w, int maxTicks = 400) {
  for (int i = 0; i < maxTicks; ++i) {
    if (w.water().pending() == 0) return i;
    w.tickWater(0.16f);
  }
  return maxTicks;
}

// A stone floor for water to sit on. It has to reach further from the source than
// water can flow — seven levels — or the flow runs off the edge, falls a hundred
// blocks to the real terrain and spreads out across it, which is correct behaviour
// and a useless test. Twelve cells of margin is comfortably past seven.
constexpr int kPondMin = -4;
constexpr int kPondMax = 20;

std::unique_ptr<world::World> makeWaterWorld() {
  auto w = makeWorld();
  for (int x = kPondMin; x <= kPondMax; ++x) {
    for (int z = kPondMin; z <= kPondMax; ++z) {
      w->setBlock(x, kY - 1, z, world::wk().greystone, 0);
    }
  }
  settleWater(*w);
  return w;
}

// --- worldgen depth (v3) -----------------------------------------------------
//
// The v3 promise is a arithmetic one — "rare ore ends below where any ravine can
// reach" — and arithmetic in a comment is worth nothing. These sample real terrain
// over a wide area and assert the property that was actually promised.

void testWorldgenDepth() {
  std::printf("worldgen depth\n");
  const world::NoiseSet noise(3918175327u);

  // The band table and the ravine floor must not overlap, by construction.
  checkf(world::deepOreCeiling(3) < world::ravineFloorMin(3),
         "the deep ore ceiling (%d) sits below the deepest ravine floor (%d)",
         world::deepOreCeiling(3), world::ravineFloorMin(3));

  // And by observation. Walk a wide grid, generate real chunks, and record the
  // highest cell each ore actually occupies against the lowest ravine floor found.
  struct Seen {
    const char* key;
    world::BlockId id;
    int highest = -1;
  };
  Seen rare[] = {{"ore_aetherite", 0}, {"ore_gloamite", 0}, {"ore_sparkstone", 0},
                 {"ore_sunbrass", 0}};
  for (Seen& s : rare) s.id = world::blocks().idOf(s.key);

  int lowestSurface = world::WH;
  for (int cx = -6; cx <= 6; ++cx) {
    for (int cz = -6; cz <= 6; ++cz) {
      world::Chunk chunk;
      chunk.cx = cx;
      chunk.cz = cz;
      chunk.data = std::make_shared<world::ChunkData>();
      world::generate(chunk, noise, 3);
      for (int x = 0; x < world::CX; ++x) {
        for (int z = 0; z < world::CZ; ++z) {
          const int h = world::heightAt(noise, cx * world::CX + x, cz * world::CZ + z, 3);
          if (h < lowestSurface) lowestSurface = h;
          for (int y = 0; y < world::WH; ++y) {
            const world::BlockId id = chunk.data->voxels[world::localIdx(x, y, z)];
            for (Seen& s : rare) {
              if (id == s.id && y > s.highest) s.highest = y;
            }
          }
        }
      }
    }
  }

  const int ravineMin = world::ravineFloorMin(3);
  for (const Seen& s : rare) {
    checkf(s.highest >= 0, "%s generates at all in v3 (highest y=%d)", s.key, s.highest);
    checkf(s.highest < ravineMin,
           "%s never reaches ravine depth (highest y=%d, deepest ravine floor %d)", s.key,
           s.highest, ravineMin);
  }
  // The other way a rare ore could be exposed: terrain lower than the ore ceiling.
  checkf(lowestSurface > world::deepOreCeiling(3),
         "no surface column dips to the deep ore band (lowest %d, ceiling %d)", lowestSurface,
         world::deepOreCeiling(3));

  // Raising WH must not have put anything into the new space in a legacy world —
  // this is the other half of the golden gate's shortened hash.
  {
    world::Chunk legacy;
    legacy.cx = 0;
    legacy.cz = 0;
    legacy.data = std::make_shared<world::ChunkData>();
    world::generate(legacy, noise, 2);
    bool clean = true;
    for (int y = 128; y < world::WH; ++y) {
      for (int i = 0; i < world::CX * world::CZ; ++i) {
        if (legacy.data->voxels[y * world::CX * world::CZ + i] != world::kAir) clean = false;
      }
    }
    check(clean, "a v2 chunk leaves the space above y=128 empty");
  }

  // v3 is v2 translated: the same seed must produce the same surface shape, just
  // 54 blocks higher. If this drifts, the "same world, deeper" claim is false.
  {
    const int shift = world::seaLevel(3) - world::seaLevel(2);
    int matched = 0, sampled = 0;
    for (int i = 0; i < 200; ++i) {
      const int wx = i * 37 - 500, wz = i * -53 + 400;
      const int a = world::heightAt(noise, wx, wz, 2);
      const int b = world::heightAt(noise, wx, wz, 3);
      ++sampled;
      if (b == a + shift) ++matched;
    }
    checkf(matched == sampled, "v3 terrain is v2 lifted by %d (%d/%d columns)", shift, matched,
           sampled);
  }
}

// Water anywhere on the floor, for the "it all dried up" check.
bool anyWaterOnFloor(const world::World& w) {
  for (int x = kPondMin; x <= kPondMax; ++x) {
    for (int z = kPondMin; z <= kPondMax; ++z) {
      if (w.getBlock(x, kY, z) == world::wk().water) return true;
    }
  }
  return false;
}

// --- block support, falling blocks, washaway ---------------------------------
//
// The scale claim is the one that has to be checked rather than asserted: a
// generated world is full of grass and torches, and if generation scheduled any
// of it the queue would start in the tens of thousands and never be free again.

int settleBlocks(world::World& w, int maxTicks = 400) {
  for (int i = 0; i < maxTicks; ++i) {
    if (w.blockUpdates().pending() == 0) return i;
    w.tickBlockUpdates(0.0625f);
  }
  return maxTicks;
}

void testBlockSupport() {
  std::printf("block support\n");
  const world::BlockId stone = world::wk().greystone;
  const world::BlockId grass = world::wk().tall_grass;
  const world::BlockId torch = world::wk().emberlight;
  const world::BlockId sand = world::wk().sand;
  const world::BlockId water = world::wk().water;

  // --- a generated world schedules nothing -------------------------------------
  //
  // Deliberately NOT makeWorld(), which clears a 9x5x9 pocket and so performs 405
  // edits of its own — those legitimately queue, and an earlier version of this
  // check counted them and read as a failure of generation. Generation is the
  // claim being tested, so nothing may touch the world first. This is the whole
  // scalability argument: a chunk holds hundreds of plants and a loaded world tens
  // of thousands, and none of them may cost anything until something moves.
  {
    world::World w(3918175327u, 2);
    w.waitForIdle(kOriginX, kOriginZ);
    checkf(w.blockUpdates().pending() == 0,
           "generating a world full of grass and torches queues no block updates (%zu)",
           w.blockUpdates().pending());
    checkf(w.water().pending() == 0, "and no water updates either (%zu)", w.water().pending());
  }

  // --- and neither does simply loading more of it ------------------------------
  {
    world::World w(3918175327u, 3);
    w.waitForIdle(kOriginX, kOriginZ);
    w.update(64.0f, 64.0f);
    w.waitForIdle(64.0f, 64.0f);
    checkf(w.blockUpdates().pending() == 0,
           "streaming further chunks in queues nothing either (%zu)",
           w.blockUpdates().pending());
  }

  // --- a plant falls when its ground goes ---------------------------------------
  {
    auto w = makeWorld();
    w->setBlock(4, kY, 4, stone, 0);
    w->setBlock(4, kY + 1, 4, grass, 0);
    settleBlocks(*w);
    check(w->getBlock(4, kY + 1, 4) == grass, "grass stands on stone");

    w->setBlock(4, kY, 4, world::kAir, 0);
    const int ticks = settleBlocks(*w);
    checkf(ticks > 0 && ticks < 400, "removing its ground settles in %d tick(s)", ticks);
    check(w->getBlock(4, kY + 1, 4) == world::kAir, "and the grass is gone with it");
  }

  // --- a torch on a mined-out block goes too ------------------------------------
  {
    auto w = makeWorld();
    w->setBlock(6, kY, 6, stone, 0);
    w->setBlock(6, kY + 1, 6, torch, 0);
    settleBlocks(*w);
    check(w->getBlock(6, kY + 1, 6) == torch, "a torch stands on stone");
    w->setBlock(6, kY, 6, world::kAir, 0);
    settleBlocks(*w);
    check(w->getBlock(6, kY + 1, 6) == world::kAir, "and drops when the stone is mined");
  }

  // --- a slab-like non-opaque solid still counts as ground ----------------------
  {
    auto w = makeWorld();
    w->setBlock(9, kY, 9, world::wk().glass, 0);
    w->setBlock(9, kY + 1, 9, grass, 0);
    settleBlocks(*w);
    check(w->getBlock(9, kY + 1, 9) == grass,
          "glass is solid, so it holds a plant up even though it is not opaque");
  }

  // --- with no entity sink, the block stays put rather than vanishing -----------
  {
    auto w = makeWorld();
    w->setBlock(12, kY, 12, stone, 0);
    w->setBlock(12, kY + 1, 12, sand, 0);
    w->setBlock(12, kY, 12, world::kAir, 0);
    settleBlocks(*w);
    check(w->getBlock(12, kY + 1, 12) == sand,
          "a world with nothing to hand a falling block to keeps it rather than losing it");
  }

  // --- and with one, sand actually falls and lands ------------------------------
  //
  // The headline behaviour, and the only check here that exercises the whole
  // chain: support rule -> beginFall -> entity -> physics -> written back into the
  // grid. Everything else above stops at the world's edge.
  {
    auto w = makeWorld();
    game::Player player(2.5f, static_cast<float>(kY), 2.5f);
    game::Inventory inv;
    game::EntityManager entities;
    Input input;
    game::EntityContext ctx;
    ctx.world = w.get();
    ctx.player = &player;
    ctx.inventory = &inv;
    ctx.entities = &entities;
    ctx.input = &input;

    w->fallSink = [&](float fx, float fy, float fz, world::BlockId id, int meta) {
      if (game::Entity* e = entities.spawn(game::EntityType::FallingBlock, Vec3{fx, fy, fz})) {
        e->data.dura = static_cast<int>(id);
        e->data.key = world::blocks().def(id).key;
        e->data.count = meta;
      }
    };

    // Floor at kY - 2, a pillar of sand three high starting two cells above it.
    for (int x = 10; x <= 14; ++x) {
      for (int z = 10; z <= 14; ++z) w->setBlock(x, kY - 2, z, stone, 0);
    }
    for (int i = 0; i < 3; ++i) w->setBlock(12, kY + i, 12, sand, 0);
    settleBlocks(*w);
    check(entities.count() > 0, "unsupported sand becomes a falling entity");

    // Run the sim and the entities together, the way the game does.
    for (int i = 0; i < 400; ++i) {
      entities.tick(1.0f / 60.0f, ctx);
      w->tickBlockUpdates(1.0f / 60.0f);
    }

    // All three land in a stack on the floor, in order, with nothing left over.
    checkf(w->getBlock(12, kY - 1, 12) == sand, "the first one lands on the floor");
    checkf(w->getBlock(12, kY, 12) == sand, "the second stacks on top of it");
    checkf(w->getBlock(12, kY + 1, 12) == sand, "and the third on top of that");
    check(w->getBlock(12, kY + 2, 12) == world::kAir,
          "so the column ends up two cells lower than it started");
    int stillFalling = 0;
    for (const game::Entity& e : entities.all()) {
      if (!e.dead && e.type == game::EntityType::FallingBlock) ++stillFalling;
    }
    checkf(stillFalling == 0, "and no falling block is left in the air (%d)", stillFalling);
  }

  // --- water washes a plant away, but only when it actually flows in ------------
  {
    auto w = makeWaterWorld();
    w->setBlock(8, kY, 8, grass, 0);
    settleWater(*w);
    settleBlocks(*w);
    check(w->getBlock(8, kY, 8) == grass, "grass beside still water is left alone");

    // Now put a source next to it and let it spread.
    w->setBlock(6, kY, 8, water, 0);
    settleWater(*w);
    check(w->getBlock(8, kY, 8) == water,
          "but water flowing into its cell washes it away and takes the space");
  }

  // --- a settled world goes quiet again -----------------------------------------
  {
    auto w = makeWorld();
    w->setBlock(3, kY, 3, stone, 0);
    settleBlocks(*w);
    check(w->blockUpdates().pending() == 0, "and the queue empties when everything has settled");
  }
}

void testWater() {
  std::printf("water\n");
  const world::BlockId water = world::wk().water;

  // --- a source spreads outward, one level thinner per step --------------------
  {
    auto w = makeWaterWorld();
    w->setBlock(8, kY, 8, water, 0);  // a source in the middle of the floor
    const int ticks = settleWater(*w);
    checkf(ticks > 0 && ticks < 400, "a placed source settles in %d tick(s)", ticks);
    check(w->getBlock(8, kY, 8) == water && w->getMeta(8, kY, 8) == 0,
          "the source itself stays a source");
    checkf(w->getBlock(9, kY, 8) == water && w->getMeta(9, kY, 8) == 1,
           "the cell beside it is level 1 (%d)", w->getMeta(9, kY, 8));
    checkf(w->getBlock(11, kY, 8) == water && w->getMeta(11, kY, 8) == 3,
           "three cells out is level 3 (%d)", w->getMeta(11, kY, 8));
    checkf(w->getBlock(15, kY, 8) == water && w->getMeta(15, kY, 8) == 7,
           "seven cells out is the thinnest film there is (%d)", w->getMeta(15, kY, 8));
    check(w->getBlock(16, kY, 8) == world::kAir,
          "and the eighth cell stays dry, so a pool has a finite edge");
    check(w->getBlock(8, kY + 1, 8) == world::kAir, "water does not climb");
  }

  // --- removing the source makes the water recede ------------------------------
  {
    auto w = makeWaterWorld();
    w->setBlock(8, kY, 8, water, 0);
    settleWater(*w);
    check(w->getBlock(10, kY, 8) == water, "water reached two cells out");

    w->setBlock(8, kY, 8, world::kAir, 0);
    const int ticks = settleWater(*w);
    checkf(ticks > 0 && ticks < 400, "removing the source settles in %d tick(s)", ticks);
    check(!anyWaterOnFloor(*w), "and every drop of it dries up rather than sitting there");
  }

  // --- water falls, and a falling column spreads at the bottom -----------------
  {
    auto w = makeWaterWorld();
    // Two blocks up, over the floor: it should fall and then run outward.
    w->setBlock(8, kY + 2, 8, water, 0);
    settleWater(*w);
    check(w->getBlock(8, kY + 1, 8) == water &&
              (w->getMeta(8, kY + 1, 8) & world::kWaterFalling) != 0,
          "water falls, and the column is flagged as falling");
    // A falling column spreads sideways at *every* level it passes, and each of
    // those cells then pushes down into the layer below and deepens it into a
    // column of its own. So the floor beside the fall is falling rather than a
    // thin film — which looks wrong until you notice it is what makes a waterfall
    // widen at its base, and is exactly what the web build does.
    check(w->getBlock(9, kY, 8) == water &&
              (w->getMeta(9, kY, 8) & world::kWaterFalling) != 0,
          "the fall widens at its base rather than trickling out one level thinner");
    checkf(w->getBlock(12, kY, 8) == water,
           "and water still reaches four cells from the impact (meta %d)",
           w->getMeta(12, kY, 8));
  }

  // --- two sources over solid ground merge into a third -------------------------
  {
    auto w = makeWaterWorld();
    w->setBlock(7, kY, 8, water, 0);
    w->setBlock(9, kY, 8, water, 0);
    settleWater(*w);
    check(w->getBlock(8, kY, 8) == water && w->getMeta(8, kY, 8) == 0,
          "two sources with a gap between them make the gap a source too");
  }

  // --- a settled pool goes quiet ------------------------------------------------
  {
    auto w = makeWaterWorld();
    w->setBlock(8, kY, 8, water, 0);
    settleWater(*w);
    check(w->water().pending() == 0, "a settled pool schedules no further work");
    // And a generated ocean was never scheduled in the first place — the whole
    // reason this can be simulated per cell.
    world::World fresh(3918175327u, 2);
    fresh.primeSpawn(8.5f, 8.5f);
    check(fresh.water().pending() == 0, "and a freshly generated ocean never ticks at all");
  }

  // --- the batch cap holds, and the overflow is not lost ------------------------
  {
    auto w = makeWaterWorld();
    // More cells than one batch can process, scheduled at once.
    for (int x = -20; x <= 20; ++x) {
      for (int z = -20; z <= 20; ++z) w->water().schedule(x, kY, z);
    }
    const std::size_t before = w->water().pending();
    checkf(before > 1200, "%zu cells queued, past the 1200-cell batch cap", before);
    w->tickWater(0.16f);
    const std::size_t after = w->water().pending();
    checkf(after == before - 1200, "one tick processes exactly 1200 and carries %zu over",
           after);
  }

  // --- the accumulator is the frame-rate-independent kind ----------------------
  {
    auto w = makeWaterWorld();
    w->water().schedule(8, kY, 8);
    // Ten frames at 60 Hz is 0.167 s — just past one 0.16 s period, so exactly one
    // batch should run. A reset-to-zero accumulator would behave the same here;
    // what it would get wrong is the leftover, which the next check covers.
    for (int i = 0; i < 10; ++i) w->tickWater(1.0f / 60.0f);
    check(w->water().pending() == 0, "ten 60 Hz frames run the one queued batch");

    // A 0.31 s stall is nearly two periods. The backlog cap means the second batch
    // comes on the very next call rather than immediately, and never more than one
    // extra however long the stall was.
    w->water().schedule(8, kY, 8);
    w->tickWater(0.31f);
    check(w->water().pending() == 0, "and a long frame still runs at most one batch");
  }
}

// --- multiplayer -------------------------------------------------------------

void testNet() {
  std::printf("multiplayer\n");

  // --- the protocol, round-tripped and then attacked --------------------------
  {
    net::PoseMsg out;
    out.pos = Vec3{12.5f, 64.0f, -3.25f};
    out.vel = Vec3{0.5f, -1.5f, 0.0f};
    out.yaw = 1.25f;
    out.pitch = -0.5f;
    out.health = 13.0f;
    out.flags = 5;

    ByteWriter w;
    net::begin(w, net::MsgType::Pose);
    net::encode(w, out);
    check(net::peekType(w.data().data(), w.size()) == net::MsgType::Pose,
          "a message names its own type in its first byte");

    ByteReader r(w.data().data(), w.size());
    r.skip(1);
    net::PoseMsg back;
    check(net::decode(r, back) && back.pos.z == out.pos.z && back.yaw == out.yaw &&
              back.health == 13.0f && back.flags == 5,
          "a pose round-trips");

    // Every truncation, not one: each cut lands in a different field.
    int survived = 0;
    for (std::size_t cut = 1; cut < w.size(); ++cut) {
      ByteReader partial(w.data().data(), cut);
      partial.skip(1);
      net::PoseMsg junk;
      if (net::decode(partial, junk)) ++survived;
    }
    checkf(survived == 0, "every truncation of a pose is rejected (%d survived)", survived);
  }
  {
    // A NaN compares false against every bound, so a range check written the
    // wrong way round lets it through — and one NaN in a position is a body that
    // vanishes and a physics step that never recovers.
    net::PoseMsg bad;
    bad.pos = Vec3{std::nanf(""), 0, 0};
    ByteWriter w;
    net::encode(w, bad);
    ByteReader r(w.data().data(), w.size());
    net::PoseMsg back;
    check(!net::decode(r, back), "a NaN coordinate is rejected, not clamped");

    net::PoseMsg huge;
    huge.pos = Vec3{9.0e9f, 0, 0};
    ByteWriter w2;
    net::encode(w2, huge);
    ByteReader r2(w2.data().data(), w2.size());
    check(!net::decode(r2, back), "and so is a coordinate past the sanity bound");
  }
  {
    // A hostile count is the one thing a fail-closed reader does not catch by
    // itself: it would reserve gigabytes before the first bad read.
    ByteWriter w;
    w.f32(0.5f);            // time
    w.u32(0xFFFFFFFFu);     // "four billion entities"
    ByteReader r(w.data().data(), w.size());
    net::SnapshotMsg snap;
    check(!net::decode(r, snap), "a snapshot claiming four billion entities is rejected");
  }
  {
    check(!net::validPlayerId("ab") && !net::validPlayerId("has space") &&
              !net::validPlayerId("../../etc") && net::validPlayerId("pm8k2x0z9qwe"),
          "a player id is restricted to what is safe as a map key and a nameplate");
    const std::string name = net::cleanName(std::string("  Bob\x01\x02  "));
    checkf(name == "Bob", "a display name is stripped of control characters (\"%s\")",
           name.c_str());
    check(net::cleanName("") == "Player" && net::cleanName("     ") == "Player",
          "and a blank one falls back rather than drawing an empty plate");
  }
  {
    net::Bucket bucket(5.0, 5.0);  // five a second, burst of five
    int allowed = 0;
    for (int i = 0; i < 20; ++i) {
      if (bucket.take(100.0)) ++allowed;
    }
    checkf(allowed == 5, "a token bucket allows its burst and no more (%d)", allowed);
    check(bucket.take(101.0), "and refills over time");
  }

  // --- invite codes -----------------------------------------------------------
  {
    const std::string code = net::makeInviteCode("192.168.1.42", 25565, "Hollow Reach");
    check(code.rfind("HRW1", 0) == 0 && code.size() > 8,
          "an invite code keeps the HRW1 envelope the web build's UX was built on");
    std::string address, world;
    std::uint16_t port = 0;
    check(net::parseInviteCode(code, address, port, world) && address == "192.168.1.42" &&
              port == 25565 && world == "Hollow Reach",
          "and round-trips address, port and world name");
    // Pasted by hand, so whitespace and case have to survive the trip.
    std::string spaced;
    for (std::size_t i = 0; i < code.size(); ++i) {
      spaced.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(code[i]))));
      if (i % 5 == 4) spaced.push_back(' ');
    }
    check(net::parseInviteCode(spaced, address, port, world) && port == 25565,
          "a code survives being lowercased and broken up with spaces");
    check(!net::parseInviteCode("not a code at all!", address, port, world),
          "and something that is not a code is refused");
  }

  // --- a real connection, over the loopback -----------------------------------
  // The milestone's own criterion, in one process: a host and a guest, a real
  // ENet handshake, and a message each way.
  {
    net::Transport server;
    net::Transport guest;
    std::string error;
    const bool listening = server.listen(0, 4, &error);
    checkf(listening, "a host binds a port (%s)", listening ? "ok" : error.c_str());
    if (listening) {
      const bool dialled = guest.connect("127.0.0.1", server.port(), &error);
      checkf(dialled, "a guest opens a connection (%s)", dialled ? "ok" : error.c_str());

      std::vector<net::TransportEvent> serverEvents, guestEvents;
      net::PeerId guestOnServer = net::kNoPeer;
      bool guestConnected = false;
      std::vector<std::uint8_t> received;

      // Pumped with a small blocking timeout rather than spun: this is a real
      // socket and the handshake takes a round trip.
      for (int i = 0; i < 200 && received.empty(); ++i) {
        server.poll(serverEvents, 5);
        for (const net::TransportEvent& e : serverEvents) {
          if (e.kind == net::TransportEvent::Kind::Connected) guestOnServer = e.peer;
        }
        guest.poll(guestEvents, 5);
        for (const net::TransportEvent& e : guestEvents) {
          if (e.kind == net::TransportEvent::Kind::Connected) guestConnected = true;
          if (e.kind == net::TransportEvent::Kind::Message) received = e.data;
        }
        if (guestOnServer != net::kNoPeer && received.empty()) {
          net::NotifyMsg note;
          note.message = "hello from the host";
          ByteWriter w;
          net::begin(w, net::MsgType::Notify);
          net::encode(w, note);
          server.send(guestOnServer, net::Channel::Reliable, w.data());
        }
      }

      check(guestConnected && guestOnServer != net::kNoPeer,
            "the two ends complete an ENet handshake over the loopback");
      net::NotifyMsg note;
      bool delivered = false;
      if (!received.empty()) {
        ByteReader r(received.data(), received.size());
        r.skip(1);
        delivered = net::peekType(received.data(), received.size()) == net::MsgType::Notify &&
                    net::decode(r, note) && note.message == "hello from the host";
      }
      check(delivered, "and a message sent on the reliable channel arrives intact");
    }
    guest.close();
    server.close();
  }
}

// A whole host and a whole guest, in one process, over the loopback. This is the
// milestone's own criterion — "two local instances: edits, combat, containers" —
// run headlessly, where it is repeatable and where a failure names itself instead
// of being a body that did not appear in a screenshot.
void testNetSession() {
  std::printf("multiplayer session\n");

  // The host's world and player.
  auto hostWorld = makeWorld();
  game::Player hostPlayer(kOriginX, kY, kOriginZ);
  game::Inventory hostInventory;
  game::EntityManager hostEntities;
  render::Sky hostSky;
  hostSky.time = 0.42f;

  net::GameRefs hostRefs;
  hostRefs.world = hostWorld.get();
  hostRefs.player = &hostPlayer;
  hostRefs.inventory = &hostInventory;
  hostRefs.entities = &hostEntities;
  hostRefs.sky = &hostSky;

  net::SessionHooks hostHooks;
  hostHooks.buildSave = [&] {
    save::WorldSave data;
    data.meta.id = "whostworld";
    data.meta.name = "Shared";
    data.meta.seed = hostWorld->seed();
    data.meta.genVersion = hostWorld->genVersion();
    data.meta.time = hostSky.time;
    data.edits = hostWorld->edits();
    data.player = hostPlayer.state();
    return data;
  };
  hostHooks.notify = [](const std::string&) {};

  net::Host host;
  std::string error;
  if (!host.start(0, "phost0000001", "Alice", hostRefs, hostHooks, &error)) {
    checkf(false, "the host binds a port (%s)", error.c_str());
    return;
  }
  check(true, "a host opens a world to guests");

  // The guest. Its world is created when the host's payload arrives, exactly as
  // App::adoptRemoteWorld does it.
  std::unique_ptr<world::World> guestWorld;
  game::Player guestPlayer(kOriginX, kY, kOriginZ);
  game::Inventory guestInventory;
  game::EntityManager guestEntities;
  render::Sky guestSky;
  bool adopted = false;

  net::Client client;
  net::SessionHooks guestHooks;
  guestHooks.notify = [](const std::string&) {};
  guestHooks.onDisconnected = [](const std::string&) {};
  guestHooks.adoptWorld = [&](const save::WorldSave& data) {
    guestWorld = std::make_unique<world::World>(data.meta.seed, 2, data.meta.genVersion);
    guestWorld->setEdits(data.edits);
    guestWorld->primeSpawn(kOriginX, kOriginZ);
    guestSky.time = data.meta.time;
    adopted = true;

    net::GameRefs refs;
    refs.world = guestWorld.get();
    refs.player = &guestPlayer;
    refs.inventory = &guestInventory;
    refs.entities = &guestEntities;
    refs.sky = &guestSky;
    client.attachGame(refs);
  };

  if (!client.start("127.0.0.1", host.port(), "pguest000001", "Bob", guestHooks, &error)) {
    checkf(false, "the guest connects (%s)", error.c_str());
    host.stop();
    return;
  }

  // A simulated clock: both ends are pumped from it, so the test does not depend
  // on how fast the machine runs.
  double now = 0.0;
  const auto pump = [&](int steps) {
    for (int i = 0; i < steps; ++i) {
      now += 0.02;
      host.update(0.02, now);
      client.update(0.02, now);
      // Real sockets need real time to carry a packet across the loopback.
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  };

  pump(150);
  checkf(adopted && client.state() == net::Client::State::Playing,
         "the guest receives the host's world and enters it");
  if (!adopted) {
    client.stop(false);
    host.stop();
    return;
  }
  check(guestSky.time == 0.42f, "and adopts the host's clock rather than its own");

  // --- the roster ------------------------------------------------------------
  {
    const std::vector<net::RosterEntry> hostSide = host.roster();
    const std::vector<net::RosterEntry> guestSide = client.roster();
    checkf(hostSide.size() == 2, "the host lists both players (%zu)", hostSide.size());
    checkf(guestSide.size() == 2, "and so does the guest (%zu)", guestSide.size());
    bool sawAlice = false;
    for (const net::RosterEntry& e : guestSide) {
      if (e.name == "Alice" && e.host) sawAlice = true;
    }
    check(sawAlice, "and the guest knows which of them is the host");
  }

  // --- an edit made on the host reaches the guest -----------------------------
  {
    const world::BlockId stone = world::wk().greystone;
    hostWorld->setBlock(6, kY, 6, stone, 0);
    host.onLocalEdit(6, kY, 6, static_cast<std::uint16_t>(stone), 0);
    pump(40);
    check(guestWorld->getBlock(6, kY, 6) == stone,
          "a block the host places appears in the guest's world");
  }

  // --- an edit made by the guest is validated and relayed back ----------------
  {
    const world::BlockId stone = world::wk().greystone;
    // The guest has to have told the host where it is, or every edit is out of
    // reach by definition — which is the check working, not failing.
    guestPlayer.setPos(Vec3{kOriginX, static_cast<float>(kY), kOriginZ});
    pump(20);
    client.sendEdit(7, kY, 7, static_cast<std::uint16_t>(stone), 0);
    pump(40);
    check(hostWorld->getBlock(7, kY, 7) == stone,
          "a block the guest places is accepted by the host");

    // And one it could not possibly reach is refused, with the real cell sent back.
    client.sendEdit(900, kY, 900, static_cast<std::uint16_t>(stone), 0);
    pump(40);
    check(hostWorld->getBlock(900, kY, 900) != stone,
          "an edit a thousand blocks away is refused rather than applied");
  }

  // --- the host's body arrives as a ghost -------------------------------------
  {
    hostPlayer.setPos(Vec3{kOriginX + 4.0f, static_cast<float>(kY), kOriginZ});
    pump(60);
    const game::Entity* body = nullptr;
    for (const game::Entity& e : guestEntities.all()) {
      if (e.ghost && e.type == game::EntityType::RemotePlayer && !e.dead) body = &e;
    }
    checkf(body != nullptr, "the host's player appears in the guest's world as a ghost");
    if (body) {
      // Interpolated 150 ms behind, so it trails the true position rather than
      // matching it — the check is that it is following, not that it is exact.
      const float dx = std::fabs(body->pos.x - (kOriginX + 4.0f));
      checkf(dx < 4.0f, "and follows where the host actually is (%.2f blocks behind)", dx);
    }
    check(!client.ghosts().nameplates().empty() &&
              client.ghosts().nameplates().front().name == "Alice",
          "with a nameplate carrying the host's name");
  }

  // --- an entity the host owns is mirrored ------------------------------------
  {
    hostEntities.spawn(game::EntityType::Cow, Vec3{kOriginX + 2.0f, static_cast<float>(kY),
                                                   kOriginZ + 2.0f});
    pump(40);
    int cows = 0;
    for (const game::Entity& e : guestEntities.all()) {
      if (e.ghost && e.type == game::EntityType::Cow && !e.dead) ++cows;
    }
    checkf(cows == 1, "a mob the host spawned is mirrored to the guest (%d)", cows);

    // And when the host's copy dies, the guest's ghost of it goes too.
    for (game::Entity& e : hostEntities.all()) {
      if (e.type == game::EntityType::Cow) e.dead = true;
    }
    game::EntityContext ctx;
    ctx.world = hostWorld.get();
    ctx.player = &hostPlayer;
    ctx.inventory = &hostInventory;
    ctx.entities = &hostEntities;
    ctx.sky = &hostSky;
    hostEntities.tick(0.02f, ctx);
    pump(40);
    cows = 0;
    for (const game::Entity& e : guestEntities.all()) {
      if (e.ghost && e.type == game::EntityType::Cow && !e.dead) ++cows;
    }
    check(cows == 0, "and vanishes from the guest when the host's copy dies");
  }

  // --- sleep is a vote, not a button ------------------------------------------
  {
    const float before = hostSky.time;
    host.onLocalSleep(true);
    pump(10);
    check(hostSky.time == before, "one of two players in bed does not skip the night");
    client.sendSleep(true);
    pump(30);
    check(hostSky.isSleeping(), "and the second vote carries it");
  }

  // --- guest progress survives the host's save ---------------------------------
  {
    guestInventory.give("greystone", 17);
    // The periodic send is every ten seconds; the goodbye below carries the last
    // state, so this is what the host will have when the guest reconnects.
    const std::vector<save::GuestSave> saved = host.guestsForSave();
    (void)saved;
    client.stop(true);
    pump(30);

    const std::vector<save::GuestSave> after = host.guestsForSave();
    checkf(after.size() == 1, "the host keeps the guest's progress after they leave (%zu)",
           after.size());
    if (after.size() == 1) {
      check(after.front().playerId == "pguest000001" &&
                after.front().inventory.countOf("greystone") == 17,
            "with the inventory they left with");

      // And it round-trips through the save format, which is the point of the
      // section — the web build carried this as `remotePlayers`.
      save::WorldSave carrier;
      carrier.meta.id = "wguestcarry";
      carrier.meta.seed = 1;
      carrier.guests = after;
      const std::vector<std::uint8_t> bytes = save::encode(carrier);
      save::WorldSave back;
      std::string why;
      const bool ok = save::decode(bytes.data(), bytes.size(), back, &why);
      check(ok && back.guests.size() == 1 &&
                back.guests.front().inventory.countOf("greystone") == 17,
            "and it survives a save and a reload");
    }
  }

  // --- leaving ----------------------------------------------------------------
  checkf(host.guestCount() == 0, "the host drops a guest that says goodbye (%d left)",
         host.guestCount());
  host.stop();
}

// --- threading ---------------------------------------------------------------

// FNV-1a over every generated chunk, in sorted key order: voxels, metadata and
// both light channels. This is the whole observable state of a world's terrain, so
// two runs that agree here produced the same world — and a mesh is a pure function
// of exactly these bytes, which is why meshing does not need its own comparison.
std::uint64_t hashWorld(const world::World& w) {
  std::uint64_t h = 1469598103934665603ull;
  const auto mix = [&h](const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
  };

  std::vector<world::ChunkKey> keys;
  for (const auto& [key, lc] : w.chunks()) {
    if (lc->chunk.generated) keys.push_back(key);
  }
  std::sort(keys.begin(), keys.end());
  for (const world::ChunkKey key : keys) {
    mix(&key, sizeof key);
    const world::ChunkData& d = *w.chunkAt(world::keyCx(key), world::keyCz(key))->chunk.data;
    mix(d.voxels.data(), d.voxels.size() * sizeof(world::BlockId));
    mix(d.meta.data(), d.meta.size());
    mix(d.skylight.data(), d.skylight.size());
    mix(d.blocklight.data(), d.blocklight.size());
  }
  return h;
}

struct WorldFingerprint {
  std::uint64_t hash = 0;
  std::size_t chunks = 0;
};

struct WorldFingerprintTimed {
  WorldFingerprint print;
  double ms = 0;
};

WorldFingerprintTimed buildWorldWith(int threads) {
  jobs::system().stop();
  jobs::system().start(threads);
  const auto t0 = std::chrono::steady_clock::now();
  world::World w(3918175327u, 4);
  w.primeSpawn(8.5f, 8.5f);
  w.waitForIdle(8.5f, 8.5f);
  const auto t1 = std::chrono::steady_clock::now();
  WorldFingerprintTimed out;
  out.print.hash = hashWorld(w);
  out.print.chunks = w.loadedChunkCount();
  out.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  return out;
}

void testThreading() {
  std::printf("threading\n");

  // --- the job system itself ---------------------------------------------------
  {
    // Unstarted means inline, which is what every headless tool relies on and what
    // --threads 0 selects.
    jobs::system().stop();
    int ran = 0;
    jobs::system().submit(jobs::Lane::Mesh, [&ran] { ++ran; });
    check(!jobs::system().threaded() && ran == 1,
          "an unstarted job system runs the job inline, on the calling thread");
  }
  {
    jobs::system().stop();
    jobs::system().start(4);
    std::atomic<int> ran{0};
    for (int i = 0; i < 400; ++i) {
      jobs::system().submit(jobs::Lane::Generate, [&ran] { ++ran; });
    }
    jobs::system().drain();
    checkf(ran.load() == 400, "drain() waits for every submitted job (%d of 400)", ran.load());
    check(jobs::system().pending() == 0, "and leaves nothing outstanding");
  }
  {
    // A throw in a worker must not take the process down, and must not lose the
    // bookkeeping that drain() waits on — a leaked count would hang the next one.
    jobs::system().stop();
    jobs::system().start(2);
    jobs::system().submit(jobs::Lane::Mesh, [] { throw std::runtime_error("boom"); });
    std::atomic<int> after{0};
    jobs::system().submit(jobs::Lane::Mesh, [&after] { ++after; });
    jobs::system().drain();
    check(after.load() == 1 && jobs::system().pending() == 0,
          "a throwing job is contained and the pool keeps working");
  }
  {
    // The reason the pool is shared rather than split: a burst of meshing must not
    // get in front of generation. One worker, a queue stuffed with mesh work, then
    // one generate job — which should still come out first.
    jobs::system().stop();
    jobs::system().start(1);
    std::mutex order;
    std::vector<int> seen;
    std::atomic<bool> release{false};
    // Block the single worker until the whole queue is built, or it would drain
    // the first lane entries before the generate job is even submitted.
    jobs::system().submit(jobs::Lane::Mesh, [&release] {
      while (!release.load()) std::this_thread::yield();
    });
    for (int i = 0; i < 8; ++i) {
      jobs::system().submit(jobs::Lane::Mesh, [&order, &seen] {
        std::lock_guard<std::mutex> lock(order);
        seen.push_back(2);
      });
    }
    jobs::system().submit(jobs::Lane::Generate, [&order, &seen] {
      std::lock_guard<std::mutex> lock(order);
      seen.push_back(0);
    });
    release.store(true);
    jobs::system().drain();
    check(!seen.empty() && seen.front() == 0,
          "generation jumps a queue of eight remeshes rather than waiting behind it");
  }

  // --- copy-on-write ------------------------------------------------------------
  {
    jobs::system().stop();
    auto w = makeWorld();
    world::LoadedChunk* lc = w->chunkAt(0, 0);
    check(lc != nullptr, "the test world has a chunk at the origin");
    if (lc) {
      // A worker holding the snapshot is exactly what a shared_ptr copy means.
      std::shared_ptr<const world::ChunkData> held = lc->chunk.data;
      const world::ChunkData* before = lc->chunk.data.get();
      w->setBlock(6, kY, 6, world::wk().greystone, 0);
      check(lc->chunk.data.get() != before,
            "writing while a job holds the chunk clones it instead of racing");
      check(held->voxels[world::localIdx(6, kY, 6)] == world::kAir,
            "and the job's snapshot still shows what it was given");
      check(w->getBlock(6, kY, 6) == world::wk().greystone, "while the world sees the edit");

      // Second write, nobody looking: no clone. This is what makes a batch of edits
      // in one frame cost one copy rather than one per cell.
      const world::ChunkData* afterClone = lc->chunk.data.get();
      w->setBlock(7, kY, 6, world::wk().greystone, 0);
      check(lc->chunk.data.get() == afterClone,
            "a second write with no reader reuses the copy it already made");
    }
  }

  // --- the milestone's own criterion: the same world at every thread count -----
  {
    const WorldFingerprintTimed inline0 = buildWorldWith(0);
    const WorldFingerprintTimed one = buildWorldWith(1);
    const WorldFingerprintTimed four = buildWorldWith(4);
    const WorldFingerprintTimed eight = buildWorldWith(8);
    jobs::system().stop();

    checkf(inline0.print.chunks > 40, "a render-distance-4 world settles at %zu chunks",
           inline0.print.chunks);
    checkf(one.print.hash == inline0.print.hash && one.print.chunks == inline0.print.chunks,
           "1 worker builds the same world as none (%016llx)",
           static_cast<unsigned long long>(one.print.hash));
    check(four.print.hash == inline0.print.hash && four.print.chunks == inline0.print.chunks,
          "4 workers build the same world");
    check(eight.print.hash == inline0.print.hash && eight.print.chunks == inline0.print.chunks,
          "8 workers build the same world");
    // Not asserted — a loaded machine would make it flaky — but printed, because
    // "identical output" is only half of what this milestone claims.
    std::printf("    ...  build times: inline %.0f ms, 1 worker %.0f, 4 workers %.0f, "
                "8 workers %.0f\n",
                inline0.ms, one.ms, four.ms, eight.ms);
  }

  // --- edits made while jobs are out survive ------------------------------------
  {
    jobs::system().stop();
    jobs::system().start(4);
    world::World w(3918175327u, 4);
    w.primeSpawn(8.5f, 8.5f);
    // Dirty the whole neighbourhood so lighting and meshing jobs are in flight,
    // then edit underneath them.
    w.update(8.5f, 8.5f);
    w.setBlock(8, 100, 8, world::wk().greystone, 0);
    w.setMeta(8, 100, 8, 5);
    w.update(8.5f, 8.5f);
    w.waitForIdle(8.5f, 8.5f);
    check(w.getBlock(8, 100, 8) == world::wk().greystone && w.getMeta(8, 100, 8) == 5,
          "a block placed while jobs were in flight is still there afterwards");
    // The metadata path is the sharp one: setMeta does not dirty light, so a light
    // result landing on top of it would silently undo a door opening.
    jobs::system().stop();
  }
}

}  // namespace

int runSelfTest() {
  gFailures = 0;
  gChecks = 0;
  std::printf("Hollowreach self-test\n\n");
  testInventory();
  testRaycast();
  testBreaking();
  testPlacing();
  testBlockEntities();
  testCrafting();
  testSurvival();
  testLayout();
  testEntities();
  testSaves();
  testWorldgenDepth();
  testWater();
  testBlockSupport();
  testNet();
  testNetSession();
  testThreading();
  testAudio();
  std::printf("\n%d checks, %d failure(s)\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}

}  // namespace hr::dev
