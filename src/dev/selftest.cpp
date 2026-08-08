#include "dev/selftest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
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
#include "game/loot.h"
#include "game/recipes.h"
#include "render/sky.h"
#include "render/viewmodel.h"
#include "ui/inventoryui.h"
#include "core/bytes.h"
#include "net/client.h"
#include "net/discovery.h"
#include "net/host.h"
#include "net/protocol.h"
#include "net/transport.h"
#include "save/format.h"
#include "platform/paths.h"
#include "save/storage.h"
#include "save/transfer.h"
#include "ui/confirm.h"
#include "ui/dom.h"
#include "ui/text.h"
#include "ui/settings.h"
#include "ui/timewheel.h"
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

  // --- opening something is not using something ---------------------------------
  //
  // swung() drives the held item's animation, and it used to be set for EVERY
  // right-click. Opening a chest or a workbench hands the click to a screen and
  // pauses the world with it, so the swing had nowhere to play: it sat queued
  // behind the pause and ran the moment the screen closed, which looks like the
  // tool swinging on its own several seconds after you put something away.
  {
    auto stationWorld = makeWorld();
    stationWorld->setBlock(11, kEyeLevel, 8, world::blocks().idOf("workbench"), 0);
    game::Interact openTest;
    game::Inventory bag;
    player.setLook(kYawPlusX, kAimLow);

    int opened = 0;
    game::InteractHooks openHooks = silentHooks();
    openHooks.onOpenStation = [&](world::Station, int, int, int) { ++opened; };
    frame(in, false, /*clickRight=*/true);
    openTest.update(0.016f, in, player, *stationWorld, bag, openHooks);
    checkf(opened == 1, "right-clicking a workbench opens it (%d)", opened);
    check(!openTest.swung(), "and does not swing the held item");

    // The same click on plain ground, with something to place, still does.
    auto placeWorld = makeWorld();
    placeWorld->setBlock(11, kEyeLevel, 8, wk.greystone, 0);
    game::Interact placeTest;
    bag.give("planks", 4);
    bag.setSelected(0);
    frame(in, false, true);
    placeTest.update(0.016f, in, player, *placeWorld, bag, silentHooks());
    check(placeTest.swung(), "while placing a block does swing it");
  }

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

  // --- pick block ------------------------------------------------------------
  //
  // Aiming at the same wall the placing checks above use. There is no creative
  // mode, so every case here is about WHERE a block you already own ends up, and
  // the one that matters is the last: with a full bar it must swap, because
  // overwriting would destroy whatever you were holding.
  {
    game::Interact pick;
    game::Inventory bag;
    game::InteractHooks hooks2 = silentHooks();
    const auto middleClick = [&] {
      in.endFrame();
      in.feedMouseButton(MouseButton::Middle, false);
      in.endFrame();
      in.feedMouseButton(MouseButton::Middle, true);
      pick.update(0.016f, in, player, *world, bag, hooks2);
    };
    // The wall is greystone; make sure the target is what these checks assume.
    world->setBlock(11, kEyeLevel, 8, wk.greystone, 0);
    player.setLook(kYawPlusX, kAimLow);

    std::string notice;
    hooks2 = silentHooks(&notice);
    middleClick();
    check(!notice.empty(), "picking a block you do not own says so rather than conjuring one");
    check(bag.slots()[0].empty(), "and leaves the bar alone");

    // In the pack: it comes to a free bar slot, and the pack slot empties.
    bag.slots()[20] = game::ItemStack{"greystone", 12, -1};
    bag.setSelected(3);
    middleClick();
    check(bag.slots()[20].empty(), "one from the pack moves to the bar");
    checkf(bag.selectedSlot().key == "greystone" && bag.selectedSlot().count == 12,
           "and is selected, whole (%s x%d)", bag.selectedSlot().key.c_str(),
           bag.selectedSlot().count);

    // Already on the bar: selected, not moved.
    bag.setSelected(7);
    middleClick();
    checkf(bag.selectedSlot().key == "greystone" && bag.countOf("greystone") == 12,
           "picking one already on the bar just selects it (%d in all)",
           bag.countOf("greystone"));

    // A full bar swaps rather than overwrites, so nothing is destroyed.
    game::Inventory full;
    for (int i = 0; i < game::kHotbarSlots; ++i) full.slots()[i] = {"planks", 1, -1};
    full.slots()[15] = game::ItemStack{"greystone", 3, -1};
    full.setSelected(4);
    game::Interact pick2;
    in.endFrame();
    in.feedMouseButton(MouseButton::Middle, false);
    in.endFrame();
    in.feedMouseButton(MouseButton::Middle, true);
    pick2.update(0.016f, in, player, *world, full, hooks2);
    check(full.selectedSlot().key == "greystone", "with a full bar the picked block takes the hand");
    check(full.slots()[15].key == "planks" && full.slots()[15].count == 1,
          "and what it displaced goes back where it came from rather than being lost");
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

// --- crouching ---------------------------------------------------------------

// --- whose setting is it -------------------------------------------------------
//
// Graphics, controls and audio are the installation's and follow the player from
// world to world. Difficulty and cheats are the WORLD's: they live in its save,
// travel with it when it is shared, and in multiplayer belong to the host, because
// a rule only half the room agreed on is not a rule.
//
// The failure this is really guarding against is silent and one-directional: a
// world-scoped value leaking back into the global store would follow the player
// into every other world they own, quietly changing worlds they never opened.
void testSettingScope() {
  std::printf("world settings\n");

  ui::SettingsStore& s = ui::settings();
  s.endWorld();  // whatever ran before this must not decide the answers

  // --- which rows belong to which ---
  check(s.isWorldScoped("monsters"), "spawning monsters is a property of the world");
  check(s.isWorldScoped("flight"), "and so is whether you may fly in it");
  check(!s.isWorldScoped("highStep"), "auto step is the player's own preference");
  check(!s.isWorldScoped("fov"), "as is the field of view");
  check(!s.isWorldScoped("masterVolume"), "and the volume");
  check(ui::categoryScope("Difficulty") == ui::SettingScope::World, "Difficulty is the world's");
  check(ui::categoryScope("Cheats") == ui::SettingScope::World, "Cheats are the world's");
  check(ui::categoryScope("Graphics") == ui::SettingScope::Global, "Graphics are not");

  // --- with no world open, a world row reads its default and cannot be set ---
  check(!s.editable("monsters"), "with no world open there is no world rule to change");
  check(s.editable("fov"), "though the global ones are always yours");
  const bool defaultMonsters = s.flag("monsters");
  check(defaultMonsters, "and monsters default to on");

  // --- a world's own values ---
  s.beginWorld({{"monsters", "false"}});
  check(!s.flag("monsters"), "opening a world installs the rules it stored");
  check(s.flag("fallDamage"), "and a rule it never stored keeps its default");
  check(s.editable("monsters"), "which the host may change");

  s.setFlag("flight", true);
  check(s.flag("flight"), "changing one takes effect at once");

  // The point of the whole exercise: none of that touched the installation.
  s.endWorld();
  checkf(s.flag("monsters") == defaultMonsters,
         "and leaving the world takes its rules with it (monsters %s)",
         s.flag("monsters") ? "on" : "off");
  check(!s.flag("flight"), "including the cheat that was switched on inside it");

  // --- two worlds do not leak into each other ---
  s.beginWorld({{"flight", "true"}, {"hunger", "false"}});
  check(s.flag("flight") && !s.flag("hunger"), "a second world brings its own rules");
  s.endWorld();
  s.beginWorld({});
  check(!s.flag("flight") && s.flag("hunger"),
        "and a world that stored none gets the defaults, not the last world's");

  // --- what gets written down ---
  {
    s.setFlag("monsters", false);
    const ui::SettingsStore::WorldValues out = s.worldValues();
    bool sawMonsters = false, sawGlobal = false;
    for (const auto& [key, value] : out) {
      if (key == "monsters") {
        sawMonsters = value == "false";
      }
      if (key == "fov" || key == "masterVolume" || key == "highStep") sawGlobal = true;
    }
    check(sawMonsters, "the world's values are what gets saved with it");
    check(!sawGlobal, "and nothing of the player's own goes into their save");
  }

  // --- a guest is in somebody else's world ---
  s.setWorldLocked(true);
  check(!s.editable("monsters"), "a guest cannot change the host's rules");
  check(s.editable("fov"), "but their own graphics are still their own");
  check(!s.flag("monsters"), "and they can still see what they are playing under");
  s.setWorldLocked(false);
  check(s.editable("monsters"), "the host can");

  s.endWorld();
}

// --- dropping with Q -----------------------------------------------------------
//
// Q over a stack used to bin the whole thing, which is a lot to lose to one key.
// It now takes one item, shift takes the lot, and holding it repeats at a rate
// that climbs — because emptying sixty-four one press at a time is no better.
void testDropping() {
  std::printf("dropping with Q\n");

  // --- what leaves the stack ---
  {
    game::ItemStack s {"greystone", 12, -1};
    const game::ItemStack one = ui::takeFromStack(s, false);
    checkf(one.count == 1 && one.key == "greystone", "Q takes a single item (%d)", one.count);
    checkf(s.count == 11, "and leaves the rest of the stack behind (%d)", s.count);

    const game::ItemStack lot = ui::takeFromStack(s, true);
    checkf(lot.count == 11, "shift takes everything that is left (%d)", lot.count);
    check(s.empty(), "emptying the slot");
  }
  {
    // The last one empties the slot rather than leaving a count of zero behind.
    game::ItemStack s {"stick", 1, -1};
    const game::ItemStack one = ui::takeFromStack(s, false);
    check(one.count == 1 && s.empty(), "taking the last item clears the slot");
    const game::ItemStack none = ui::takeFromStack(s, false);
    check(none.empty(), "and an empty slot gives nothing");
  }
  {
    // Wear rides along, or a worn tool would be duplicated as a fresh one.
    game::ItemStack s {"pick_copper", 1, 42};
    const game::ItemStack one = ui::takeFromStack(s, false);
    checkf(one.dura == 42, "a dropped tool keeps its wear (%d)", one.dura);
  }

  // --- the cadence of holding it ---
  {
    ui::DropRun run;
    checkf(run.tick(false, 0.0) == 1, "pressing Q drops exactly one at once");
    // Nothing more until the first delay is up: a tap must never cost two.
    int during = 0;
    for (int i = 0; i < 15; ++i) during += run.tick(true, 1.0 / 60.0);  // 0.25 s
    checkf(during == 0, "and a quarter second of holding adds nothing yet (%d)", during);

    // Held on: the run starts and speeds up.
    int firstSecond = 0;
    for (int i = 0; i < 60; ++i) firstSecond += run.tick(true, 1.0 / 60.0);
    int laterSecond = 0;
    for (int i = 0; i < 180; ++i) run.tick(true, 1.0 / 60.0);  // let it reach full rate
    for (int i = 0; i < 60; ++i) laterSecond += run.tick(true, 1.0 / 60.0);
    checkf(firstSecond > 0, "holding it starts a run (%d in the first second)", firstSecond);
    checkf(laterSecond > firstSecond, "which drops faster the longer it is held (%d then %d)",
           firstSecond, laterSecond);
    checkf(laterSecond <= 30, "but never faster than about thirty a second (%d)", laterSecond);
  }
  {
    // Moving to another slot restarts the run and drops there at once, which is
    // what makes dragging Q across a row work.
    ui::DropRun run;
    run.tick(false, 0.0);
    for (int i = 0; i < 240; ++i) run.tick(true, 1.0 / 60.0);  // at full rate
    checkf(run.tick(false, 1.0 / 60.0) == 1,
           "moving onto a new slot drops one there immediately");
    int next = 0;
    for (int i = 0; i < 15; ++i) next += run.tick(true, 1.0 / 60.0);
    checkf(next == 0, "and the new slot waits out the first delay again (%d)", next);
  }
  {
    // A frame that took a very long time must not empty a chest.
    ui::DropRun run;
    run.tick(false, 0.0);
    checkf(run.tick(true, 30.0) <= 8, "one enormous frame drops a bounded number");
  }
}

// --- walking up things ---------------------------------------------------------
//
// The auto-step lifts the body by its step height and tries the move again, which
// is what walks you up a slab without jumping. A stair's bottom tread is the same
// height as a bottom slab and should behave identically from the low side — it is
// the single most common thing anybody builds a staircase out of.
void testAutoStep() {
  std::printf("walking up steps\n");

  const world::BlockId stone = world::wk().greystone;
  game::PlayerOptions options;
  options.fallDamageEnabled = false;

  // Floor at kY-1, and one obstacle at kY in a wall across the path. The player
  // starts to its -x side and holds the key that walks toward +x.
  const auto walkInto = [&](world::BlockId id, int meta, float stepHeight) {
    auto w = makeWorld();
    for (int x = 0; x <= 16; ++x) {
      for (int z = 4; z <= 12; ++z) {
        w->setBlock(x, kY - 1, z, stone, 0);
        for (int y = kY; y <= kY + 3; ++y) w->setBlock(x, y, z, world::kAir, 0);
      }
    }
    for (int z = 4; z <= 12; ++z) w->setBlock(9, kY, z, id, meta);
    w->waitForIdle(kOriginX, kOriginZ);

    game::PlayerOptions o = options;
    o.stepHeight = stepHeight;
    game::Player player(6.5f, static_cast<float>(kY), 8.5f);
    Input in;
    for (int i = 0; i < 150; ++i) {
      in.endFrame();
      in.feedKey(Key::D, true, false);  // +x when facing -z
      player.update(1.0f / 60.0f, in, *w, o, i / 60.0);
    }
    return player.pos();
  };

  const auto& slabs = world::blocks().slabOf();
  const auto slabIt = slabs.find("greystone");
  const world::BlockId slab =
      slabIt == slabs.end() ? 0 : world::blocks().idOf(slabIt->second);
  const world::BlockId stair = world::blocks().idOf("greystone_stairs");
  const world::BlockId vslab = world::blocks().idOf("greystone_vslab");
  check(slab != 0, "there is a greystone slab to walk at");
  check(stair != 0, "and a greystone stair");

  // Every orientation, at both step heights, printed rather than asserted — this
  // is the sweep that has to say WHICH shapes behave like a wall before anything
  // is asserted about them.
  std::printf("       %-22s %-8s %-8s\n", "obstacle", "step .6", "step 1.0");
  const auto sweep = [&](const char* label, world::BlockId id, int meta) {
    const float low = walkInto(id, meta, game::playerConst::kStep).x;
    const float high = walkInto(id, meta, 1.0f).x;
    std::printf("       %-22s %-8s %-8s\n", label, low > 9.5f ? "over" : "WALL",
                high > 9.5f ? "over" : "WALL");
  };
  sweep("full block", stone, 0);
  sweep("slab bottom", slab, 0);
  sweep("slab top", slab, 1);
  for (int m = 0; m < 4; ++m) {
    char label[32];
    std::snprintf(label, sizeof label, "stair facing %d", m);
    sweep(label, stair, m);
  }
  for (int m = 0; m < 4; ++m) {
    char label[32];
    std::snprintf(label, sizeof label, "stair facing %d, top", m);
    sweep(label, stair, m | 4);
  }
  if (vslab != 0) {
    for (int m = 0; m < 4; ++m) {
      char label[32];
      std::snprintf(label, sizeof label, "vslab %d", m);
      sweep(label, vslab, m);
    }
  }

  // The case anybody actually builds: a flight of stairs, each one a block higher
  // than the last, facing the way you climb. Walking into it should carry you up.
  const auto climbStaircase = [&](float stepHeight, bool useStairs, int ceilingAbove = 0) {
    auto w = makeWorld();
    for (int x = 0; x <= 20; ++x) {
      for (int z = 4; z <= 12; ++z) {
        w->setBlock(x, kY - 1, z, stone, 0);
        for (int y = kY; y <= kY + 8; ++y) w->setBlock(x, y, z, world::kAir, 0);
      }
    }
    // Six treads climbing toward +x from x = 9. Facing 0 puts the riser on the +x
    // side, which is the low side for somebody walking that way.
    for (int i = 0; i < 6; ++i) {
      for (int z = 4; z <= 12; ++z) {
        if (useStairs) {
          w->setBlock(9 + i, kY + i, z, stair, 0);
        } else {
          w->setBlock(9 + i, kY + i, z, stone, 0);
        }
        // Fill under each tread so it is a solid flight rather than floating steps.
        for (int y = kY; y < kY + i; ++y) w->setBlock(9 + i, y, z, stone, 0);
      }
    }
    // A ceiling that follows the flight up, which is what a staircase inside a
    // building has and an open ramp does not.
    if (ceilingAbove > 0) {
      for (int z = 4; z <= 12; ++z) {
        for (int x = 0; x < 9; ++x) w->setBlock(x, kY + ceilingAbove, z, stone, 0);
        for (int i = 0; i < 6; ++i) {
          w->setBlock(9 + i, kY + i + ceilingAbove, z, stone, 0);
        }
      }
    }
    w->waitForIdle(kOriginX, kOriginZ);
    game::PlayerOptions o = options;
    o.stepHeight = stepHeight;
    game::Player player(6.5f, static_cast<float>(kY), 8.5f);
    Input in;
    // The HIGHEST point reached, not the final one: past the top tread the flight
    // runs out of floor and the player walks off it, so the last frame describes
    // a fall rather than the climb being asked about.
    float best = player.pos().y;
    for (int i = 0; i < 400; ++i) {
      in.endFrame();
      in.feedKey(Key::D, true, false);
      player.update(1.0f / 60.0f, in, *w, o, i / 60.0);
      best = std::max(best, player.pos().y);
    }
    return best;
  };
  {
    const float s6 = climbStaircase(game::playerConst::kStep, true);
    const float s10 = climbStaircase(1.0f, true);
    const float b10 = climbStaircase(1.0f, false);
    const float base = static_cast<float>(kY);
    std::printf("       %-22s +%-7.1f +%-7.1f\n", "stair flight", s6 - base, s10 - base);
    std::printf("       %-22s %-8s +%-7.1f\n", "block flight", "-", b10 - base);
    // The same flight with a roof over it, at the two headrooms a builder would
    // actually use. High Step lifts the whole body a full block before testing, so
    // it needs 0.4 more clearance than the default does — if that is the fault,
    // this is where it shows.
    bool highStepNeverWorse = true;
    float roofed4High = 0, roofed4Low = 0;
    for (int head : {3, 4, 5}) {
      const float c6 = climbStaircase(game::playerConst::kStep, true, head);
      const float c10 = climbStaircase(1.0f, true, head);
      char label[40];
      std::snprintf(label, sizeof label, "roofed flight, %d up", head);
      std::printf("       %-22s +%-7.1f +%-7.1f\n", label, c6 - base, c10 - base);
      if (c10 < c6 - 0.01f) highStepNeverWorse = false;
      if (head == 4) {
        roofed4Low = c6 - base;
        roofed4High = c10 - base;
      }
    }
    checkf(s6 > base + 4.0f, "a flight of stairs is climbable at the default step (up %.1f)",
           s6 - base);
    // The invariant the fix is really about, and the one worth keeping: a taller
    // step must never climb LESS than a shorter one. It could, because the probe
    // used to lift the body by the whole step height and demand headroom there —
    // so switching High Step on made an indoor staircase unclimbable.
    check(highStepNeverWorse, "High Step never climbs less than the default step does");
    checkf(roofed4High > 4.0f,
           "and a staircase with a ceiling over it is climbable with High Step on "
           "(up %.1f, default manages %.1f)",
           roofed4High, roofed4Low);
  }

  // A bottom slab is half a block, well under the 0.6 default step.
  {
    const Vec3 after = walkInto(slab, 0, game::playerConst::kStep);
    checkf(after.x > 9.5f, "a bottom slab is walked straight over (x %.2f)", after.x);
  }

  // With High Step on, a full block is walkable too — that is the whole feature.
  {
    const Vec3 after = walkInto(stone, 0, 1.0f);
    checkf(after.x > 9.5f, "High Step walks up a whole block (x %.2f)", after.x);
  }

  // And without it, a full block is still a wall.
  {
    const Vec3 after = walkInto(stone, 0, game::playerConst::kStep);
    checkf(after.x < 9.0f, "but a whole block stops you without it (x %.2f)", after.x);
  }
}

void testCrouch() {
  std::printf("crouching\n");
  const world::BlockId stone = world::wk().greystone;
  game::PlayerOptions options;
  options.fallDamageEnabled = false;

  // A one-block-wide ledge with a long drop off the +x side, and the player stood
  // on it. kY - 1 is solid; everything beyond x = 9 is open air down to the pocket
  // floor, which makes "did they walk off" a question with an obvious answer.
  const auto makeLedge = [&] {
    auto w = makeWorld();
    for (int x = 4; x <= 9; ++x) {
      for (int z = 4; z <= 12; ++z) w->setBlock(x, kY - 1, z, stone, 0);
    }
    return w;
  };

  const auto hold = [](Input& in, bool shift, bool forward) {
    in.endFrame();
    in.feedKey(Key::ShiftLeft, shift, false);
    in.feedKey(Key::D, forward, false);  // +x when facing -z, i.e. toward the drop
  };

  // --- the view drops, and comes back ----------------------------------------
  {
    auto w = makeLedge();
    game::Player player(6.5f, static_cast<float>(kY), 6.5f);
    Input in;
    const float standing = player.eye().y - player.pos().y;

    for (int i = 0; i < 60; ++i) {
      hold(in, true, false);
      player.update(1.0f / 60.0f, in, *w, options, i / 60.0);
    }
    const float crouched = player.eye().y - player.pos().y;
    checkf(standing - crouched > 0.2f,
           "crouching lowers the eye by something you can see (%.3f blocks)",
           standing - crouched);
    check(player.sneaking(), "and the player reports that they are crouching");
    checkf(player.sneakAmount() > 0.99f, "with the ease fully in (%.3f)", player.sneakAmount());

    for (int i = 0; i < 60; ++i) {
      hold(in, false, false);
      player.update(1.0f / 60.0f, in, *w, options, 1.0 + i / 60.0);
    }
    checkf(std::fabs((player.eye().y - player.pos().y) - standing) < 0.001f,
           "and standing up puts it back exactly (%.3f vs %.3f)",
           player.eye().y - player.pos().y, standing);
    check(!player.sneaking(), "and clears the flag");
  }

  // --- walking off the edge, which is what it is for --------------------------
  {
    auto w = makeLedge();
    game::Player walker(8.5f, static_cast<float>(kY), 6.5f);
    Input in;
    for (int i = 0; i < 180; ++i) {
      hold(in, false, true);
      walker.update(1.0f / 60.0f, in, *w, options, i / 60.0);
    }
    checkf(walker.pos().y < kY - 2.0f, "walking off a ledge upright falls off it (y %.2f)",
           walker.pos().y);

    auto w2 = makeLedge();
    game::Player crouched(8.5f, static_cast<float>(kY), 6.5f);
    Input in2;
    for (int i = 0; i < 180; ++i) {
      hold(in2, true, true);
      crouched.update(1.0f / 60.0f, in2, *w2, options, i / 60.0);
    }
    checkf(crouched.pos().y >= static_cast<float>(kY) - 0.01f,
           "but crouching holds you on it (y %.2f)", crouched.pos().y);
    // The last solid cell is x=9, so its far face is at x=10. A foothold is any
    // part of the footprint still over ground, which lets you lean out over the
    // drop by up to half a body width and no further — the same overhang Minecraft
    // gives you, and the reason crouching at an edge looks like leaning rather than
    // like hitting an invisible wall.
    checkf(crouched.pos().x > 9.5f && crouched.pos().x <= 10.0f + game::playerConst::kHalfWidth,
           "right out to the lip, hanging over it but not off it (x %.2f)", crouched.pos().x);
    check(crouched.onGround(), "and still standing on something");
  }

  // --- but it is not a licence to hover ---------------------------------------
  //
  // The guard is armed from the ground that was under the feet before the step, so
  // someone already over a drop is not held up by holding crouch. Without that, a
  // player whose footing was mined out from under them would hang in the air.
  {
    auto w = makeLedge();
    game::Player faller(6.5f, static_cast<float>(kY) + 6.0f, 6.5f);
    Input in;
    for (int i = 0; i < 30; ++i) {
      hold(in, true, true);
      faller.update(1.0f / 60.0f, in, *w, options, i / 60.0);
    }
    checkf(faller.pos().y < static_cast<float>(kY) + 6.0f,
           "crouching in mid-air does not stop you falling (y %.2f)", faller.pos().y);
  }
}

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

  // --- where monsters may appear ---------------------------------------------
  //
  // The rule is light level zero and nothing else: no clock, no "must be the
  // surface". Every check below is the same predicate the world spawner and the
  // Evil Altar both run.
  {
    auto world = makeArena();
    world->waitForIdle(kOriginX, kOriginZ);

    // A sealed chamber inside the arena: floor already there at kY-1, walls at
    // kY..kY+1 around a 5x5, roof at kY+2. Nothing else in the arena is roofed,
    // so this is the only genuinely dark cell in it.
    for (int x = 17; x <= 23; ++x) {
      for (int z = 17; z <= 23; ++z) {
        world->setBlock(x, kY + 2, z, world::wk().greystone, 0);
        const bool wall = x == 17 || x == 23 || z == 17 || z == 23;
        if (!wall) continue;
        world->setBlock(x, kY, z, world::wk().greystone, 0);
        world->setBlock(x, kY + 1, z, world::wk().greystone, 0);
      }
    }
    world->waitForIdle(kOriginX, kOriginZ);

    render::Sky sky;
    sky.time = 0.5f;
    const float noon = sky.dayFactor();
    sky.time = 0.0f;
    const float midnight = sky.dayFactor();
    checkf(noon > 0.9f && midnight == 0.0f, "noon is %.2f and midnight is %.2f", noon, midnight);

    checkf(game::effectiveLight(*world, 10, kY, 10, noon) == 15,
           "open arena floor reads full light at noon (%d)",
           game::effectiveLight(*world, 10, kY, 10, noon));
    check(!game::monsterSpawnable(*world, 10, kY, 10, noon),
          "so nothing spawns on open ground in daylight");
    checkf(game::effectiveLight(*world, 10, kY, 10, midnight) == 0,
           "the same cell reads zero at midnight (%d)",
           game::effectiveLight(*world, 10, kY, 10, midnight));
    check(game::monsterSpawnable(*world, 10, kY, 10, midnight),
          "and monsters spawn on it after dark, as they always did");

    // The headline change: a roofed cave is dark at noon.
    checkf(game::effectiveLight(*world, 20, kY, 20, noon) == 0,
           "a sealed chamber reads zero light at noon (%d)",
           game::effectiveLight(*world, 20, kY, 20, noon));
    check(game::monsterSpawnable(*world, 20, kY, 20, noon),
          "so monsters spawn in caves regardless of the time of day");
    check(game::monsterSpawnable(*world, 20, kY, 20, midnight),
          "and at night too, which is the same rule and not a second one");

    // A torch is a torch at every hour.
    world->setBlock(21, kY, 20, world::wk().emberlight, 0);
    world->waitForIdle(kOriginX, kOriginZ);
    checkf(game::effectiveLight(*world, 20, kY, 20, noon) > 0,
           "a torch lights the chamber (%d)", game::effectiveLight(*world, 20, kY, 20, noon));
    check(!game::monsterSpawnable(*world, 20, kY, 20, noon),
          "and lighting a cave is what stops it spawning");
    world->setBlock(21, kY, 20, world::kAir, 0);
    world->waitForIdle(kOriginX, kOriginZ);
    check(game::monsterSpawnable(*world, 20, kY, 20, noon), "take the torch away and it is back");

    // Standing room and dry footing, both still required.
    world->setBlock(20, kY + 1, 20, world::wk().greystone, 0);
    check(!game::monsterSpawnable(*world, 20, kY, 20, noon),
          "a cell with no headroom is not a spawn");
    world->setBlock(20, kY + 1, 20, world::kAir, 0);
    world->setBlock(20, kY - 1, 20, world::wk().water, 0);
    check(!game::monsterSpawnable(*world, 20, kY, 20, noon),
          "and nor is one standing on water");
    world->setBlock(20, kY - 1, 20, world::wk().greystone, 0);

    // A generated-but-unlit chunk must read as bright, not as dark. Its light
    // arrays are all zero until a pass lands on them, and zero is exactly what a
    // cave looks like — so without the guard every meadow that had just streamed
    // in would be a spawn site for the frame or two before it was lit.
    //
    // The apron ring is the reproducible version of that window: generation
    // reaches renderDistance + 1 and lighting only reaches renderDistance, so a
    // chunk out there is generated and permanently unlit. Here that is chunk 3,
    // which at this render distance sits inside the 20-40 block spawn ring.
    constexpr int kApronX = 3 * 16 + 8;
    check(world->chunkReady(3, 0), "the apron chunk is generated");
    check(!world->lightReady(3, 0), "and deliberately never lit");
    world->setBlock(kApronX, kY - 1, 8, world::wk().greystone, 0);
    world->setBlock(kApronX, kY, 8, world::kAir, 0);
    world->setBlock(kApronX, kY + 1, 8, world::kAir, 0);
    checkf(game::effectiveLight(*world, kApronX, kY, 8, noon) == 15,
           "an unlit chunk reads bright rather than dark (%d)",
           game::effectiveLight(*world, kApronX, kY, 8, noon));
    check(!game::monsterSpawnable(*world, kApronX, kY, 8, noon),
          "so ground whose light has not been computed is not a spawn site");
  }

  // --- the world spawner finds a cave ----------------------------------------
  {
    auto world = makeArena();
    world->waitForIdle(kOriginX, kOriginZ);

    // A sealed room out in the 20-40 block spawn ring, at (39, 8) — 30 blocks from
    // the player's column. Its own floor, because out here the arena's has ended.
    //
    // Kept clear of a chunk border on purpose: lighting is chunk-local with
    // one-cell neighbour seeding (world/lighting.h), so a sealed room straddling a
    // seam keeps a little leaked skylight along it for as long as neither side is
    // relit again. That is a known approximation of the lighting, not of the spawn
    // rule, and a test of the spawn rule should not be measuring it.
    constexpr int kRx = 39, kRz = 8;
    for (int x = kRx - 3; x <= kRx + 3; ++x) {
      for (int z = kRz - 3; z <= kRz + 3; ++z) {
        world->setBlock(x, kY - 1, z, world::wk().greystone, 0);
        world->setBlock(x, kY, z, world::kAir, 0);
        world->setBlock(x, kY + 1, z, world::kAir, 0);
        world->setBlock(x, kY + 2, z, world::wk().greystone, 0);
        const bool wall = x == kRx - 3 || x == kRx + 3 || z == kRz - 3 || z == kRz + 3;
        if (!wall) continue;
        world->setBlock(x, kY, z, world::wk().greystone, 0);
        world->setBlock(x, kY + 1, z, world::wk().greystone, 0);
      }
    }
    world->waitForIdle(kOriginX, kOriginZ);

    render::Sky sky;
    sky.time = 0.5f;  // noon, where the old rule would have refused outright
    game::EntityManager entities;
    const Vec3 player{kOriginX, static_cast<float>(kY), kOriginZ};

    // Stated separately from the search below, so a failure says whether the room
    // is not dark or the search is not finding it.
    checkf(game::monsterSpawnable(*world, kRx, kY, kRz, sky.dayFactor()),
           "the buried room is spawnable ground at noon (light %d)",
           game::effectiveLight(*world, kRx, kY, kRz, sky.dayFactor()));

    int inRoom = 0, elsewhere = 0;
    for (int i = 0; i < 600; ++i) {
      entities.trySpawnZombie(*world, player, sky.dayFactor());
      for (game::Entity& e : entities.all()) {
        if (e.dead) continue;
        const bool here = std::fabs(e.pos.x - (kRx + 0.5f)) <= 2.5f &&
                          std::fabs(e.pos.z - (kRz + 0.5f)) <= 2.5f &&
                          std::fabs(e.pos.y - static_cast<float>(kY)) < 0.5f;
        if (here) {
          ++inRoom;
        } else {
          ++elsewhere;
        }
        e.dead = true;  // clear the cap so the next call is a fresh draw
      }
    }
    checkf(inRoom > 0, "the spawner finds a buried room at noon (%d spawns in it)", inRoom);
    checkf(elsewhere == 0, "and puts nothing on the sunlit ground around it (%d)", elsewhere);
  }

  // --- hostiles despawn at range, animals do not ------------------------------
  {
    auto world = makeArena();
    game::Player player(kOriginX, static_cast<float>(kY), kOriginZ);
    game::Inventory inv;
    game::EntityManager entities;
    render::Sky sky;
    game::EntityContext ctx;
    ctx.world = world.get();
    ctx.player = &player;
    ctx.inventory = &inv;
    ctx.entities = &entities;
    ctx.sky = &sky;

    const float far = game::EntityManager::kHostileDespawn + 20.0f;
    entities.spawn(game::EntityType::Zombie, Vec3{kOriginX + far, static_cast<float>(kY), kOriginZ});
    entities.spawn(game::EntityType::Sheep, Vec3{kOriginX + far, static_cast<float>(kY), kOriginZ});
    entities.spawn(game::EntityType::Zombie, Vec3{kOriginX + 6.0f, static_cast<float>(kY),
                                                  kOriginZ});
    entities.tick(1.0f / 60.0f, ctx);

    int zombies = 0, sheep = 0;
    for (const game::Entity& e : entities.all()) {
      if (e.dead) continue;
      if (e.type == game::EntityType::Zombie) ++zombies;
      if (e.type == game::EntityType::Sheep) ++sheep;
    }
    checkf(zombies == 1, "a hostile far from the player is culled, a near one is not (%d left)",
           zombies);
    check(sheep == 1, "and an animal you left somewhere stays there");
  }

  // --- the click that opens a screen stays outside it ---------------------------
  // Right clicking a chest opened the inventory and split a stack in the same
  // breath, because the pointer was still resting where it was left the last time
  // a screen was up. Sixty frames a second, so a frame is about 16 ms.
  {
    constexpr double kFrame = 1.0 / 60.0;
    ui::MouseGuard g;
    check(!g.guarding(), "nothing is guarded before a screen opens");
    check(!g.update(kFrame, true), "and a held button on an ordinary frame is heard");

    // Right click, held: the frame the screen opens on, and the ones just after.
    g.arm();
    check(g.update(kFrame, true), "the click that opens a screen does not reach it");
    check(g.update(kFrame, true), "nor does the frame after");

    // Held well past the timer. This is the case a bare timer misses.
    for (int i = 0; i < 60; ++i) g.update(kFrame, true);
    check(g.guarding(), "a button still held keeps the screen deaf however long");
    check(!g.update(kFrame, false), "and letting go is what finally opens it up");

    // The very next click is a real one and must land.
    check(!g.update(kFrame, true), "so the next click is the player's, and is heard");

    // Released quickly instead: the timer alone has to carry it.
    ui::MouseGuard q;
    q.arm();
    check(q.update(kFrame, true), "a quick click is caught by the timer");
    check(q.update(kFrame, false), "and stays caught after the button comes up");
    int frames = 2;
    while (q.guarding() && frames < 600) {
      q.update(kFrame, false);
      ++frames;
    }
    checkf(frames < 600, "and the guard always lifts on its own (%d frames)", frames);
    const double waited = frames * kFrame;
    checkf(waited >= ui::MouseGuard::kWindow - kFrame && waited < 0.25,
           "after about the window it promises, not longer (%.0f ms)", waited * 1000.0);
  }

  // --- the drop pile has a ceiling ---------------------------------------------
  // Drops in unloaded chunks never age, and the pile went into the save, so a
  // world only ever gained them. Far enough out that every one of these is frozen
  // rather than ticking, which is exactly the ones that used to be immortal.
  {
    auto world = makeArena();
    game::Player player(kOriginX, static_cast<float>(kY), kOriginZ);
    game::Inventory inv;
    game::EntityManager entities;
    render::Sky sky;
    game::EntityContext ctx;
    ctx.world = world.get();
    ctx.player = &player;
    ctx.inventory = &inv;
    ctx.entities = &entities;
    ctx.sky = &sky;

    constexpr std::size_t kOver = 40;
    const std::size_t want = game::EntityManager::kMaxDrops + kOver;
    int firstId = 0, lastId = 0;
    for (std::size_t i = 0; i < want; ++i) {
      game::Entity* d = entities.spawnDrop(
          Vec3{4000.5f + static_cast<float>(i), static_cast<float>(kY), 4000.5f}, "greystone", 1,
          -1);
      if (i == 0) firstId = d->id;
      if (i + 1 == want) lastId = d->id;
    }
    entities.tick(1.0f / 60.0f, ctx);

    std::size_t left = 0;
    for (const game::Entity& e : entities.all()) {
      if (!e.dead && e.type == game::EntityType::Drop) ++left;
    }
    checkf(left == game::EntityManager::kMaxDrops,
           "a world cannot collect more than kMaxDrops frozen drops (%zu)", left);
    check(entities.byId(firstId) == nullptr, "and the pile is trimmed from the oldest end");
    check(entities.byId(lastId) != nullptr, "leaving what was dropped most recently");

    // The ceiling is high enough that an ordinary world never meets it — the
    // check that keeps this a safety valve rather than a rule players notice.
    game::EntityManager ordinary;
    ctx.entities = &ordinary;
    for (int i = 0; i < 200; ++i) {
      ordinary.spawnDrop(Vec3{4000.5f + static_cast<float>(i), static_cast<float>(kY), 4000.5f},
                         "greystone", 1, -1);
    }
    const int before = ordinary.count();
    ordinary.tick(1.0f / 60.0f, ctx);
    checkf(ordinary.count() == before, "and a normal scattering of them is left alone (%d -> %d)",
           before, ordinary.count());
  }

  // --- the Evil Altar ---------------------------------------------------------
  {
    const world::BlockId altar = world::blocks().idOf("evil_altar");
    check(altar != world::kAir, "the Evil Altar is registered");
    check(world::blocks().def(altar).drop.empty(),
          "and mining one destroys it rather than dropping it");
    bool crafted = false;
    for (const game::Recipe& r : game::recipeBook().recipes()) {
      if (r.outKey == "evil_altar") crafted = true;
    }
    check(!crafted, "and there is no recipe for one");

    auto world = makeArena();
    world->waitForIdle(kOriginX, kOriginZ);
    // The same sealed chamber as above, with an altar in the middle of it.
    for (int x = 17; x <= 23; ++x) {
      for (int z = 17; z <= 23; ++z) {
        world->setBlock(x, kY + 2, z, world::wk().greystone, 0);
        const bool wall = x == 17 || x == 23 || z == 17 || z == 23;
        if (!wall) continue;
        world->setBlock(x, kY, z, world::wk().greystone, 0);
        world->setBlock(x, kY + 1, z, world::wk().greystone, 0);
      }
    }
    world->setBlock(20, kY, 20, altar, 0);
    world->waitForIdle(kOriginX, kOriginZ);

    render::Sky sky;
    sky.time = 0.5f;  // noon: a dungeon does not wait for nightfall
    game::EntityManager entities;
    const Vec3 player{20.5f, static_cast<float>(kY), 20.5f};

    for (int i = 0; i < 60 && entities.count() == 0; ++i) {
      entities.tickEvilAltars(*world, player, sky.dayFactor());
    }
    checkf(entities.count() > 0, "an altar breeds zombies around itself at noon (%d)",
           entities.count());

    // It fills its own room and then stops, rather than emptying into it forever.
    for (int i = 0; i < 400; ++i) entities.tickEvilAltars(*world, player, sky.dayFactor());
    checkf(entities.count() <= game::EntityManager::kAltarCrowd,
           "and stops at its own crowd limit (%d of %d)", entities.count(),
           game::EntityManager::kAltarCrowd);

    // Light the room and it goes quiet — the same rule, so the same off switch.
    for (game::Entity& e : entities.all()) e.dead = true;
    world->setBlock(19, kY, 19, world::wk().emberlight, 0);
    world->setBlock(21, kY, 21, world::wk().emberlight, 0);
    world->waitForIdle(kOriginX, kOriginZ);
    game::EntityManager lit;
    for (int i = 0; i < 200; ++i) lit.tickEvilAltars(*world, player, sky.dayFactor());
    checkf(lit.count() == 0, "torching the room disarms the altar (%d)", lit.count());

    // Out of range it does nothing at all, which is what keeps a distant dungeon
    // from filling the world while you are nowhere near it.
    for (game::Entity& e : entities.all()) e.dead = true;
    game::EntityManager away;
    const Vec3 distant{20.5f + game::EntityManager::kAltarRange + 4.0f,
                       static_cast<float>(kY), 20.5f};
    for (int i = 0; i < 200; ++i) away.tickEvilAltars(*world, distant, sky.dayFactor());
    check(away.count() == 0, "and an altar nobody is near stays asleep");
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

  // How long the player has been awake, so the bed's gate survives a reload rather
  // than handing everyone a fresh night every time they load the world.
  s.hoursAwake = 5.75f;

  // The world's own rules. Deliberately not every world-scoped key: a save written
  // before one existed, or by an older build, has to load with the missing ones at
  // their defaults rather than refusing or zeroing them.
  s.worldSettings = {{"monsters", "false"}, {"flight", "true"}};

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

  checkf(back.hoursAwake == original.hoursAwake, "how long you have been awake survives (%.2f)",
         back.hoursAwake);
  checkf(back.worldSettings == original.worldSettings,
         "and so do the world's own rules (%zu of them)", back.worldSettings.size());

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

// --- underground water (v4) ---------------------------------------------------
//
// v2 flooded every carved cell below `seaLevel - 34`, which at a sea level of 46
// meant y<=12: a ten-layer sump on the bedrock. v3 raised sea level to 100 and the
// line rose with it to y=66 -- sixty-five layers, and every ore worth mining for
// sits under it. Measured on this seed, v3 leaves *100%* of the carved space in
// the deep ore band underwater. v4 replaces the line with lakes.
//
// The numbers are asserted loosely, as bands rather than exact values: the point
// is "caves are mostly dry and water is a thing you come across", and pinning that
// to three significant figures would just break every time the field is retuned.
// Creative: the three rules, each at the chokepoint it is enforced in.
void testCreative() {
  std::printf("\n-- creative --\n");

  // --- nothing can take health off you ---------------------------------------
  {
    game::Player p(kOriginX, static_cast<float>(kY), kOriginZ);
    game::PlayerOptions opts;
    p.damage(5.0f, opts);
    checkf(p.health() < 20.0f, "an ordinary hit lands (%.1f)", p.health());

    game::Player safe(kOriginX, static_cast<float>(kY), kOriginZ);
    game::PlayerOptions god;
    god.invulnerable = true;
    safe.damage(5.0f, god);
    checkf(safe.health() == 20.0f, "an invulnerable player takes none of it (%.1f)",
           safe.health());
    // Drowning and starving both come through here with ignoreArmor set, which is
    // a different path through the same door.
    safe.damage(1.0f, god, /*ignoreArmor=*/true);
    checkf(safe.health() == 20.0f, "including the kinds armour cannot stop (%.1f)",
           safe.health());
    check(!safe.dead(), "and so cannot die");
  }

  // --- and that includes the zombie ------------------------------------------
  // Its own check because this one site builds its own PlayerOptions rather than
  // being handed the real ones, which meant it silently opted out of every rule
  // added to that struct. A zombie was the last thing in the world that could
  // kill a player nothing else could touch.
  {
    auto world = makeArena();
    game::Player player(kOriginX, static_cast<float>(kY), kOriginZ);
    game::Inventory inv;
    game::EntityManager entities;
    render::Sky sky;
    game::PlayerOptions god;
    god.invulnerable = true;
    game::EntityContext ctx;
    ctx.world = world.get();
    ctx.player = &player;
    ctx.inventory = &inv;
    ctx.entities = &entities;
    ctx.sky = &sky;
    ctx.playerOptions = &god;

    game::Entity* zombie = entities.spawn(game::EntityType::Zombie,
                                          Vec3{kOriginX + 0.8f, static_cast<float>(kY),
                                               kOriginZ});
    check(zombie != nullptr, "a zombie is standing next to an invulnerable player");
    const float before = player.health();
    for (int i = 0; i < 400; ++i) entities.tick(1.0f / 60.0f, ctx);
    checkf(player.health() == before, "and cannot lay a finger on them (%.1f -> %.1f)", before,
           player.health());

    // The control: the same zombie, the same seconds, an ordinary player.
    game::Player mortal(kOriginX, static_cast<float>(kY), kOriginZ);
    game::PlayerOptions plain;
    ctx.player = &mortal;
    ctx.playerOptions = &plain;
    entities.spawn(game::EntityType::Zombie,
                   Vec3{kOriginX + 0.8f, static_cast<float>(kY), kOriginZ});
    for (int i = 0; i < 400; ++i) entities.tick(1.0f / 60.0f, ctx);
    checkf(mortal.health() < 20.0f, "while an ordinary one is bitten (%.1f)", mortal.health());
  }

  // --- the world stops being solid -------------------------------------------
  {
    auto world = makeArena();
    // A wall directly in front of the origin.
    for (int y = kY; y <= kY + 2; ++y) {
      for (int z = -2; z <= 24; ++z) world->setBlock(12, y, z, world::wk().greystone, 0);
    }

    // Driven through the real update loop with a real key held, which is what makes
    // this a test of the game rather than of one method: the auto-step probes, the
    // edge guard and the flight branch all sit between the key and the move.
    const auto walkInto = [&](bool noClip) {
      game::Player p(9.5f, static_cast<float>(kY), kOriginZ);
      p.setLook(kYawPlusX, 0.0f);
      game::PlayerOptions o;
      o.noClip = noClip;
      o.flightAllowed = true;
      o.fallDamageEnabled = false;
      Input in;
      for (int i = 0; i < 240; ++i) {
        in.endFrame();
        in.feedKey(Key::W, true, false);
        p.update(1.0f / 60.0f, in, *world, o, i / 60.0);
      }
      return p.pos().x;
    };
    const float stopped = walkInto(false);
    const float through = walkInto(true);
    checkf(stopped < 12.0f, "a solid wall stops an ordinary player (x %.1f)", stopped);
    checkf(through > 13.0f, "and no-clip walks straight through it (x %.1f)", through);
  }

  // --- flight follows the rule, in both directions ---------------------------
  {
    auto world = makeArena();
    game::Player p(kOriginX, static_cast<float>(kY), kOriginZ);
    game::PlayerOptions o;
    o.noClip = true;
    Input in;
    p.update(1.0f / 60.0f, in, *world, o, 0.0);
    check(p.flying(), "switching no-clip on starts you flying, since the floor has gone");

    o.noClip = false;
    o.flightAllowed = false;
    p.update(1.0f / 60.0f, in, *world, o, 0.1);
    check(!p.flying(), "and taking the rule away puts you back on your feet");
  }

  // --- the world remembers what it was made as -------------------------------
  {
    save::WorldSave s;
    s.meta.id = "wtestcreative";
    s.meta.seed = 7u;
    s.createdCreative = true;
    std::vector<std::uint8_t> bytes = save::encode(s);
    save::WorldSave back;
    std::string error;
    check(save::decode(bytes.data(), bytes.size(), back, &error), "a creative world encodes");
    check(back.createdCreative, "and says so when it is read back");

    save::WorldSave plain;
    plain.meta.id = "wtestsurvival";
    plain.meta.seed = 7u;
    bytes = save::encode(plain);
    check(save::decode(bytes.data(), bytes.size(), back, &error), "a survival world encodes");
    check(!back.createdCreative, "and says that too");

    // The section is new, so every world written before it existed simply has no
    // such tag. That has to read as Survival rather than as anything else.
    save::WorldSave fresh;
    check(!fresh.createdCreative, "and a save with no mode section at all is survival");
  }
}

// Asking before destroying something.
//
// Almost nothing in src/ui can be tested — it needs a GL context and a Doc — and
// ConfirmPrompt is written the way it is partly so this test can exist. It is the
// one piece of interface in the game standing between a misclick and a deleted
// world, so it being untestable would have been the wrong trade.
void testConfirmPrompt() {
  std::printf("\n-- confirmations --\n");

  // No Ui2D at all: handle() takes the viewport as two numbers precisely so this
  // needs no GL context.
  constexpr float kW = 1280.0f, kH = 720.0f;

  const auto clickAt = [](float x, float y) {
    ui::UiEvent e;
    e.mouseX = x;
    e.mouseY = y;
    e.leftClick = true;
    return e;
  };
  ui::UiEvent idle;
  idle.mouseX = 640;
  idle.mouseY = 360;

  ui::ConfirmPrompt p;
  check(!p.active(), "nothing is being asked to begin with");
  check(!p.handle(kW, kH, clickAt(10, 10)), "and a click passes straight through");

  int fired = 0;
  p.open("Delete Elder?", "Delete", [&fired] { ++fired; });
  check(p.active(), "opening one arms it");

  // The click that opened it is still going. Without the guard the prompt appears
  // under a cursor mid-click and answers itself with the same press.
  check(p.handle(kW, kH, clickAt(760, 430)), "the click that opened it is swallowed");
  checkf(fired == 0, "and does not answer it (%d)", fired);
  check(p.handle(kW, kH, clickAt(760, 430)), "nor the frame after");
  checkf(fired == 0, "still unanswered (%d)", fired);

  // Past the guard: the screen behind must still see nothing.
  check(p.handle(kW, kH, idle), "an armed prompt keeps the frame even when idle");
  check(p.active(), "and stays up until it is answered");

  // Confirm. The button is the right-hand one of a centred 380x150 card.
  const float cx = kW * 0.5f, cy = kH * 0.5f;
  const float confirmX = cx + 95.0f, confirmY = cy + 75.0f - 20.0f - 19.0f;
  check(p.handle(kW, kH, clickAt(confirmX, confirmY)), "clicking Delete is taken");
  checkf(fired == 1, "and runs the action exactly once (%d)", fired);
  check(!p.active(), "and closes");
  check(!p.handle(kW, kH, clickAt(confirmX, confirmY)), "after which clicks pass through again");
  checkf(fired == 1, "and it does not run again (%d)", fired);

  // Cancel, and anywhere-else, are both no.
  fired = 0;
  p.open("Delete Elder?", "Delete", [&fired] { ++fired; });
  p.handle(kW, kH, idle);
  p.handle(kW, kH, idle);
  const float cancelX = cx - 95.0f;
  p.handle(kW, kH, clickAt(cancelX, confirmY));
  checkf(fired == 0, "cancelling runs nothing (%d)", fired);
  check(!p.active(), "and closes it too");

  fired = 0;
  p.open("Delete Elder?", "Delete", [&fired] { ++fired; });
  p.handle(kW, kH, idle);
  p.handle(kW, kH, idle);
  p.handle(kW, kH, clickAt(20, 20));
  checkf(fired == 0, "clicking away from the card is a no, not a yes (%d)", fired);
  check(!p.active(), "and dismisses it");

  // A destroyed thing cannot be destroyed twice: close() must clear the action.
  fired = 0;
  p.open("Delete Elder?", "Delete", [&fired] { ++fired; });
  p.close();
  check(!p.active(), "closing it by hand disarms it");
  checkf(fired == 0, "without running anything (%d)", fired);
}

// Loot tables. The arithmetic behind a chest, checked without a chest.
void testLoot() {
  std::printf("\n-- loot --\n");

  const game::LootTable* common = game::lootBook().find("dungeon/chest");
  const game::LootTable* rich = game::lootBook().find("dungeon/altar");
  check(common != nullptr, "the common dungeon table is registered");
  check(rich != nullptr, "and the altar table beside it");
  check(game::lootBook().find("nothing/here") == nullptr,
        "and asking for a table nobody wrote gives nothing rather than a crash");
  if (!common || !rich) return;

  // Sorted by name, which is what keeps the golden dump stable across builds — an
  // unordered_map walked directly would reshuffle it.
  const std::vector<const game::LootTable*> all = game::lootBook().sorted();
  bool ordered = true;
  for (std::size_t i = 1; i < all.size(); ++i) {
    if (all[i - 1]->id > all[i]->id) ordered = false;
  }
  check(ordered, "and the book hands them back in name order");

  // Every stack a table can produce has to be a real item, or a chest quietly
  // swallows it. A typo here is invisible until somebody opens one.
  int unknown = 0;
  for (const game::LootTable* t : all) {
    for (const game::LootPool& pool : t->pools) {
      for (const game::LootEntry& e : pool.entries) {
        if (!game::getItem(e.key)) ++unknown;
      }
    }
  }
  checkf(unknown == 0, "every item a table can give actually exists (%d unknown)", unknown);

  // Counts inside their declared range, over enough positions to catch an
  // off-by-one at either end.
  {
    int outside = 0, rolls = 0;
    for (int i = 0; i < 400; ++i) {
      for (const game::ItemStack& s : game::rollLoot(*common, 12345u, i * 7, 24, i * 13)) {
        ++rolls;
        bool ok = false;
        for (const game::LootPool& pool : common->pools) {
          for (const game::LootEntry& e : pool.entries) {
            if (e.key == s.key && s.count >= e.minCount && s.count <= e.maxCount) ok = true;
          }
        }
        if (!ok) ++outside;
      }
    }
    checkf(rolls > 0, "a table actually produces something (%d stacks over 400 chests)", rolls);
    checkf(outside == 0, "and never a count outside what it declared (%d bad)", outside);
  }

  // The same chest, asked twice, is the same chest. This is what stops a container
  // changing its mind when the chunk holding it is regenerated.
  {
    const std::vector<game::ItemStack> a = game::rollLoot(*common, 999u, 40, 30, -12);
    const std::vector<game::ItemStack> b = game::rollLoot(*common, 999u, 40, 30, -12);
    bool same = a.size() == b.size();
    for (std::size_t i = 0; same && i < a.size(); ++i) {
      same = a[i].key == b[i].key && a[i].count == b[i].count && a[i].dura == b[i].dura;
    }
    check(same, "the same chest rolls the same contents every time");

    // And a different one is genuinely different rather than the same list shifted.
    int differing = 0;
    for (int i = 1; i <= 40; ++i) {
      const std::vector<game::ItemStack> c = game::rollLoot(*common, 999u, 40 + i, 30, -12);
      if (c.size() != a.size()) {
        ++differing;
        continue;
      }
      for (std::size_t k = 0; k < c.size(); ++k) {
        if (c[k].key != a[k].key || c[k].count != a[k].count) {
          ++differing;
          break;
        }
      }
    }
    checkf(differing >= 35, "and its neighbours hold something else (%d of 40 differ)",
           differing);

    // The seed has to matter too, or every world's dungeons are the same dungeon.
    int seedDiffers = 0;
    for (std::uint32_t s = 1; s <= 20; ++s) {
      const std::vector<game::ItemStack> c = game::rollLoot(*common, s, 40, 30, -12);
      if (c.size() != a.size()) {
        ++seedDiffers;
        continue;
      }
      for (std::size_t k = 0; k < c.size(); ++k) {
        if (c[k].key != a[k].key || c[k].count != a[k].count) {
          ++seedDiffers;
          break;
        }
      }
    }
    checkf(seedDiffers >= 17, "and another world's does too (%d of 20 seeds)", seedDiffers);
  }

  // A looted tool arrives usable. Writing an ItemStack straight into a chest slot
  // skips Inventory::give, which is what normally fills durability in — so without
  // an explicit lookup a found pickaxe reads as "does not wear" and never breaks.
  {
    int tools = 0, unbreakable = 0, wrong = 0;
    for (int i = 0; i < 600; ++i) {
      for (const game::LootTable* t : all) {
        for (const game::ItemStack& s : game::rollLoot(*t, 7u, i * 3, 28, i * 11)) {
          const int want = game::maxDurability(s.key);
          if (want <= 0) continue;
          ++tools;
          if (s.dura < 0) ++unbreakable;
          else if (s.dura != want) ++wrong;
        }
      }
    }
    checkf(tools > 0, "tools do turn up in loot (%d over 600 chests)", tools);
    checkf(unbreakable == 0, "and none of them is unbreakable (%d with no durability)",
           unbreakable);
    checkf(wrong == 0, "each arriving at full durability (%d wrong)", wrong);
  }

  // Weights mean something. The heaviest entry in a pool should beat the lightest
  // by roughly their ratio — loose bounds, because this is a check that the weight
  // walk works at all, not a test of the hash's uniformity.
  {
    const game::LootPool& pool = common->pools[0];
    std::unordered_map<std::string, int> seen;
    for (int i = 0; i < 4000; ++i) {
      for (const game::ItemStack& s : game::rollLoot(*common, 55u, i, 26, i * 5)) seen[s.key]++;
    }
    const game::LootEntry* heaviest = nullptr;
    const game::LootEntry* lightest = nullptr;
    for (const game::LootEntry& e : pool.entries) {
      if (!heaviest || e.weight > heaviest->weight) heaviest = &e;
      if (!lightest || e.weight < lightest->weight) lightest = &e;
    }
    const int hi = seen[heaviest->key], lo = seen[lightest->key];
    checkf(lo > 0, "even the rarest entry in a pool turns up (%s x%d)", lightest->key.c_str(),
           lo);
    checkf(hi > lo, "and the commonest beats it (%s x%d vs %s x%d)", heaviest->key.c_str(), hi,
           lightest->key.c_str(), lo);
  }
}

// The first structure. Swept over real terrain rather than asserted about, the way
// testCaveWater and testWorldgenDepth are.
void testDungeons() {
  std::printf("\n-- dungeons --\n");

  const world::NoiseSet noise(3918175327u);
  const std::uint32_t seed = 3918175327u;
  const world::BlockId altar = world::blocks().idOf("evil_altar");
  const world::BlockId chestId = world::blocks().idOf("chest");
  const world::BlockId bricks = world::blocks().idOf("bricks");
  const world::BlockId cobbled = world::blocks().idOf("cobbled");

  // THE claim the whole release rests on: v5 adds dungeons and changes nothing
  // else, so an existing world can be moved onto it without its landscape shifting
  // under what the player built. The golden gate covers this too; it is here as
  // well because it is the property most likely to be broken by a later edit, and
  // a gate failure is a diff while this is a sentence.
  {
    int differing = 0, compared = 0;
    for (int cx = -3; cx <= 3; ++cx) {
      for (int cz = -3; cz <= 3; ++cz) {
        world::Chunk a, b;
        a.cx = b.cx = cx;
        a.cz = b.cz = cz;
        a.data = std::make_shared<world::ChunkData>();
        b.data = std::make_shared<world::ChunkData>();
        world::generate(a, noise, 4);
        world::generate(b, noise, 4);
        // Same version twice is a control: if this ever differs, the generator has
        // become nondeterministic and every other check here is meaningless.
        for (int i = 0; i < world::kCellsPerChunk; ++i) {
          ++compared;
          if (a.data->voxels.get(i) != b.data->voxels.get(i)) ++differing;
        }
      }
    }
    checkf(differing == 0, "generation is repeatable at all (%d of %d cells)", differing,
           compared);
  }

  // And that v4 has no dungeon in it.
  //
  // The control above only proves the generator is deterministic; it compares v4
  // with v4 and would happily pass with dungeons carved through both. What actually
  // has to hold is that the v5 gate keeps them out of v4 entirely, and that is
  // checkable without a stored baseline: bricks, cobbled, chests and altars are
  // crafted blocks. Terrain generation has never produced one at any version, so a
  // single cell of masonry in a v4 chunk means the gate has leaked.
  {
    int masonry = 0;
    for (int cx = -6; cx <= 6; ++cx) {
      for (int cz = -6; cz <= 6; ++cz) {
        world::Chunk c;
        c.cx = cx;
        c.cz = cz;
        c.data = std::make_shared<world::ChunkData>();
        world::generate(c, noise, 4);
        for (int i = 0; i < world::kCellsPerChunk; ++i) {
          const world::BlockId id = c.data->voxels.get(i);
          if (id == altar || id == chestId || id == bricks || id == cobbled) ++masonry;
        }
      }
    }
    checkf(masonry == 0, "and an older world stays exactly as old as it was (%d dungeon cells at v4)",
           masonry);
  }

  int dungeonChunks = 0, altars = 0, chests = 0, breached = 0, unenclosed = 0;
  int totalChunks = 0;
  constexpr int kSweep = 14;  // 29x29 chunks, about 215 blocks either way
  for (int cx = -kSweep; cx <= kSweep; ++cx) {
    for (int cz = -kSweep; cz <= kSweep; ++cz) {
      world::Chunk chunk;
      chunk.cx = cx;
      chunk.cz = cz;
      chunk.data = std::make_shared<world::ChunkData>();
      world::generate(chunk, noise, world::kGenVersion);
      ++totalChunks;

      bool any = false;
      for (int x = 0; x < world::CX; ++x) {
        for (int z = 0; z < world::CZ; ++z) {
          const int wx = cx * 16 + x, wz = cz * 16 + z;
          const int surface = world::heightAt(noise, wx, wz, world::kGenVersion);
          for (int y = 3; y < world::WH; ++y) {
            const world::BlockId id = chunk.data->voxels.get(world::localIdx(x, y, z));
            if (id == altar) {
              ++altars;
              any = true;
            } else if (id == chestId) {
              ++chests;
              any = true;
            } else if (id == bricks || id == cobbled) {
              any = true;
            } else {
              continue;
            }
            // Masonry above the ground is masonry that broke the surface. The
            // margin matches kRockAbove: a chamber lit from outside is a chamber
            // whose altar has switched itself off.
            if (y > surface - 4) ++breached;
          }
        }
      }
      if (any) ++dungeonChunks;
    }
  }

  checkf(altars > 0, "dungeons generate at all (%d altars over %d chunks)", altars,
         totalChunks);
  checkf(chests >= altars, "each with at least one chest (%d chests, %d altars)", chests,
         altars);
  // Two-sided. A generator that carpets the world in dungeons passes any check that
  // only asks whether it made one.
  const double pct = 100.0 * dungeonChunks / totalChunks;
  checkf(pct > 1.0, "spread widely enough to be met rather than hunted (%.1f%% of chunks)",
         pct);
  checkf(pct < 35.0, "and rarely enough to still be a find (%.1f%% of chunks)", pct);
  checkf(breached == 0, "and never breaking the surface (%d cells above ground)", breached);

  // A chamber has to be sealed, or it fills with cave air and daylight and stops
  // being a room. Walk the shell around the nearest altar.
  {
    world::DungeonSite site;
    check(world::findDungeon(noise, seed, world::kGenVersion, 0, 0, 6, site),
          "one can be found near the origin, for --find-dungeon to point at");

    std::unordered_map<long long, world::BlockId> around;
    const int c0x = world::World::floorDiv16(site.x - 40), c1x = world::World::floorDiv16(site.x + 40);
    const int c0z = world::World::floorDiv16(site.z - 40), c1z = world::World::floorDiv16(site.z + 40);
    for (int cx = c0x; cx <= c1x; ++cx) {
      for (int cz = c0z; cz <= c1z; ++cz) {
        world::Chunk chunk;
        chunk.cx = cx;
        chunk.cz = cz;
        chunk.data = std::make_shared<world::ChunkData>();
        world::generate(chunk, noise, world::kGenVersion);
        for (int x = 0; x < world::CX; ++x) {
          for (int z = 0; z < world::CZ; ++z) {
            for (int y = site.y - 2; y <= site.y + 7 && y < world::WH; ++y) {
              const long long k = (static_cast<long long>(cx * 16 + x) << 40) ^
                                  (static_cast<long long>(y) << 20) ^ (cz * 16 + z);
              around[k] = chunk.data->voxels.get(world::localIdx(x, y, z));
            }
          }
        }
      }
    }
    const auto at = [&](int x, int y, int z) {
      const long long k = (static_cast<long long>(x) << 40) ^ (static_cast<long long>(y) << 20) ^ z;
      const auto it = around.find(k);
      return it == around.end() ? world::kAir : it->second;
    };

    check(at(site.x, site.y, site.z) == altar, "the altar is where the finder says it is");
    check(at(site.x, site.y - 1, site.z) == cobbled, "standing on a laid floor");
    // Flood the open space from the altar. This used to assert the dungeon was
    // sealed rock to rock, which was right when it had no way in and is exactly
    // wrong now: the point of the tunnel is that the flood escapes.
    //
    // What survives from that check is the surface: the flood may reach cave, but
    // it must never reach daylight, or the altar is lit and the dungeon is off.
    std::vector<std::array<int, 3>> open {{site.x, site.y, site.z}};
    std::unordered_map<long long, bool> seen;
    std::size_t head = 0;
    int cells = 0, reachedSky = 0;
    int farthest = 0;
    while (head < open.size() && cells < 60000) {
      const std::array<int, 3> p = open[head++];
      ++cells;
      const int span = std::max(std::abs(p[0] - site.x), std::abs(p[2] - site.z));
      if (span > farthest) farthest = span;
      const int dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
      for (const auto& d : dirs) {
        const int nx = p[0] + d[0], ny = p[1] + d[1], nz = p[2] + d[2];
        if (std::abs(nx - site.x) > 34 || std::abs(nz - site.z) > 34) continue;
        if (ny >= world::heightAt(noise, nx, nz, world::kGenVersion)) {
          ++reachedSky;
          continue;
        }
        const world::BlockId id = at(nx, ny, nz);
        if (id != world::kAir) continue;
        const long long k =
            (static_cast<long long>(nx) << 40) ^ (static_cast<long long>(ny) << 20) ^ nz;
        if (seen.count(k)) continue;
        seen[k] = true;
        open.push_back({nx, ny, nz});
      }
    }
    checkf(reachedSky == 0, "the altar chamber never opens to the sky (%d cells)", reachedSky);
    // Past the dungeon's own footprint, which is 31 blocks at its very widest and
    // usually much less. Getting well outside it from the altar means the tunnel
    // reached natural cave and the two are joined.
    checkf(farthest > 20, "and the way out reaches real cave (%d blocks from the altar)",
           farthest);
    checkf(cells > 40, "with room enough inside to be a dungeon (%d open cells)", cells);

    // Sealed is not the same as connected, and only one of the two is obvious from
    // looking at a wall. The first version of this generator stamped each room and
    // corridor complete — shell and interior together — so a corridor's walls were
    // laid straight through the room it had just opened into, and the altar ended up
    // in a one-block slot with the chests behind masonry. Every enclosure check above
    // passed while that was true, because a dungeon chopped into sealed pieces is
    // still sealed.
    //
    // So: every chest has to be reachable from the altar. A chest is a solid block
    // and so is never itself in the flood, which makes the question whether the
    // flood ever came to stand next to it.
    std::vector<world::DungeonChest> mine;
    std::vector<world::DungeonChest> perChunk;
    for (int cx = c0x; cx <= c1x; ++cx) {
      for (int cz = c0z; cz <= c1z; ++cz) {
        world::dungeonChestsIn(cx, cz, noise, seed, world::kGenVersion, perChunk);
        for (const world::DungeonChest& s : perChunk) {
          if (std::abs(s.x - site.x) > 34 || std::abs(s.z - site.z) > 34) continue;
          if (s.y != site.y) continue;  // a neighbouring dungeon at another depth
          mine.push_back(s);
        }
      }
    }
    int unreachable = 0;
    for (const world::DungeonChest& s : mine) {
      const int dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
      bool touched = false;
      for (const auto& d : dirs) {
        const long long k = (static_cast<long long>(s.x + d[0]) << 40) ^
                            (static_cast<long long>(s.y + d[1]) << 20) ^ (s.z + d[2]);
        if (seen.count(k)) touched = true;
      }
      if (!touched) ++unreachable;
    }
    checkf(!mine.empty(), "this dungeon has chests to walk to (%zu)", mine.size());
    checkf(unreachable == 0, "and every one of them can be walked to from the altar (%d walled off)",
           unreachable);
  }

  // The chest positions World re-derives have to be the chests that were actually
  // stamped, or containers are offered at empty air and real ones never filled.
  {
    int found = 0, mismatched = 0;
    std::vector<world::DungeonChest> sites;
    for (int cx = -8; cx <= 8; ++cx) {
      for (int cz = -8; cz <= 8; ++cz) {
        world::Chunk chunk;
        chunk.cx = cx;
        chunk.cz = cz;
        chunk.data = std::make_shared<world::ChunkData>();
        world::generate(chunk, noise, world::kGenVersion);
        world::dungeonChestsIn(cx, cz, noise, seed, world::kGenVersion, sites);
        for (const world::DungeonChest& s : sites) {
          ++found;
          const int lx = s.x - cx * 16, lz = s.z - cz * 16;
          if (lx < 0 || lx >= world::CX || lz < 0 || lz >= world::CZ) {
            ++mismatched;  // reported a chest that is not even in this chunk
            continue;
          }
          if (chunk.data->voxels.get(world::localIdx(lx, s.y, lz)) != chestId) ++mismatched;
        }
      }
    }
    checkf(found > 0, "the chest sites can be re-derived without being stored (%d)", found);
    checkf(mismatched == 0, "and every one of them is a chest in the ground (%d wrong)",
           mismatched);
  }

  // Filling a dungeon chest once and only once.
  //
  // The pressure here is that chunks are regenerated every time the player walks
  // back into them, so the sink fires again for a chest they cleared out an hour
  // ago. What stops it refilling is that block entities are NOT unloaded with their
  // chunk — so one already being there is the record that this chest has been met.
  {
    world::DungeonSite site;
    if (world::findDungeon(noise, seed, world::kGenVersion, 0, 0, 6, site)) {
      world::World w(seed, 4);
      int filled = 0, offered = 0;
      // The site the finder returns is the ALTAR, which is not a container — the
      // chests stand in the room corners. So the position to inspect is whichever
      // one the sink actually reports, not the one we searched for.
      int chestX = 0, chestY = 0, chestZ = 0;
      bool haveChest = false;
      // The same guard App applies, kept in step with it by being written the same
      // way round: existing entity means leave it alone.
      w.setLootSink([&](int x, int y, int z, bool rich) {
        ++offered;
        if (!haveChest) {
          chestX = x;
          chestY = y;
          chestZ = z;
          haveChest = true;
        }
        if (w.getBlockEntity(x, y, z)) return;
        game::BlockEntity* be = w.getOrCreateBlockEntity(x, y, z, game::BlockEntityKind::Chest);
        if (!be) return;
        const std::vector<game::ItemStack> loot =
            game::rollLoot(rich ? "dungeon/altar" : "dungeon/chest", seed, x, y, z);
        for (std::size_t i = 0; i < loot.size() && i < be->slots.size(); ++i) {
          be->slots[i] = loot[i];
        }
        ++filled;
      });

      w.waitForIdle(static_cast<float>(site.x), static_cast<float>(site.z));
      for (int i = 0; i < 40; ++i) w.update(static_cast<float>(site.x), static_cast<float>(site.z));
      checkf(filled > 0, "a dungeon chest is filled as its chunk lands (%d)", filled);
      check(haveChest, "and the sink says which cell it was");

      // What is in it, before anything is allowed to happen to it twice.
      const game::BlockEntity* be = w.getBlockEntity(chestX, chestY, chestZ);
      int before = 0;
      if (be) {
        for (const game::ItemStack& st : be->slots) {
          if (!st.empty()) ++before;
        }
      }
      checkf(before > 0, "with something in it (%d stacks)", before);

      // Empty it, the way a player would, then walk away and come back.
      if (game::BlockEntity* mutableBe = w.getBlockEntity(chestX, chestY, chestZ)) {
        for (game::ItemStack& st : mutableBe->slots) st.clear();
      }
      const int filledBefore = filled;
      w.waitForIdle(static_cast<float>(site.x + 4000), static_cast<float>(site.z + 4000));
      for (int i = 0; i < 40; ++i) {
        w.update(static_cast<float>(site.x + 4000), static_cast<float>(site.z + 4000));
      }
      w.waitForIdle(static_cast<float>(site.x), static_cast<float>(site.z));
      for (int i = 0; i < 40; ++i) w.update(static_cast<float>(site.x), static_cast<float>(site.z));

      checkf(offered > filledBefore,
             "coming back really does offer the chest again (%d offers, %d fills)", offered,
             filled);
      checkf(filled == filledBefore, "but an emptied one stays empty (%d fills, was %d)", filled,
             filledBefore);
      const game::BlockEntity* after = w.getBlockEntity(chestX, chestY, chestZ);
      int stacks = 0;
      if (after) {
        for (const game::ItemStack& st : after->slots) {
          if (!st.empty()) ++stacks;
        }
      }
      checkf(stacks == 0, "with nothing put back into it (%d stacks)", stacks);
    }
  }
}

// Moving a world onto a newer generator, and the copy taken before it happens.
void testWorldUpgrade() {
  std::printf("\n-- old worlds --\n");

  // This is the only test that goes through the storage layer rather than encode
  // and decode, because listing worlds is the thing being checked and a listing
  // needs a directory to read. runSelfTest is reached before main.cpp calls
  // paths::init, so without this `save::list()` asks whether "" is a directory,
  // decides it is not, and truthfully reports no worlds at all — which looks
  // exactly like the bug this test was written to catch.
  //
  // A scratch directory rather than the real one: a test has no business writing
  // into somebody's saves, and both worlds below are erased at the end anyway.
  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() / "hollowreach-selftest";
  std::error_code ec;
  std::filesystem::create_directories(scratch, ec);
  paths::init(scratch.string());

  save::WorldSave s;
  s.meta.id = save::newId();
  s.meta.name = "Elder";
  s.meta.seed = 4242u;
  s.meta.genVersion = 3;
  s.meta.createdAt = s.meta.savedAt = save::nowSeconds();
  s.inventory.give("greystone", 11);
  check(save::write(s), "a world on an older generator can be written");

  const std::vector<save::WorldListing> before = save::list();
  const save::WorldListing* row = nullptr;
  for (const save::WorldListing& w : before) {
    if (w.id == s.meta.id) row = &w;
  }
  check(row != nullptr, "and it appears in the listing");
  // The number was decoded and then dropped on the floor before this, which is why
  // nothing could warn about it.
  checkf(row && row->genVersion == 3, "which now carries the generator that made it (%d)",
         row ? row->genVersion : -1);
  check(world::kGenVersion > 3, "and the current generator is newer, so there is a warning to give");

  std::string copyId;
  check(save::backup(s.meta.id, &copyId), "it can be copied");
  check(copyId != s.meta.id, "under an id of its own rather than over the original");

  save::WorldSave copy;
  check(save::read(copyId, copy), "the copy reads back");
  checkf(copy.meta.genVersion == 3, "still on the old generator (%d)", copy.meta.genVersion);
  check(copy.meta.name.find("backup") != std::string::npos, "and says what it is in its name");
  check(copy.inventory.countOf("greystone") == 11, "carrying everything the original had");

  check(save::setGenVersion(s.meta.id, world::kGenVersion), "the original can be moved forward");
  save::WorldSave moved;
  check(save::read(s.meta.id, moved), "and reads back");
  checkf(moved.meta.genVersion == world::kGenVersion, "on the current generator (%d)",
         moved.meta.genVersion);
  check(moved.meta.seed == 4242u, "with the same seed, so it is the same place");
  check(moved.inventory.countOf("greystone") == 11, "and nothing of the player's lost");

  // The copy must be untouched by what happened to the original — the entire point.
  save::WorldSave after;
  check(save::read(copyId, after), "the copy still reads");
  checkf(after.meta.genVersion == 3, "and is still the world as it was (%d)",
         after.meta.genVersion);

  save::erase(s.meta.id);
  save::erase(copyId);
}

void testCaveWater() {
  std::printf("underground water\n");
  const world::NoiseSet noise(3918175327u);

  struct Survey {
    long long carved = 0, wet = 0, deepCarved = 0, deepWet = 0;
    int floating = 0, aboveCeiling = 0, columns = 0, wetColumns = 0;
    long long joined = 0;
  };

  const auto survey = [&](int ver) {
    Survey s;
    for (int cx = -5; cx <= 5; ++cx) {
      for (int cz = -5; cz <= 5; ++cz) {
        world::Chunk chunk;
        chunk.cx = cx;
        chunk.cz = cz;
        chunk.data = std::make_shared<world::ChunkData>();
        world::generate(chunk, noise, ver);
        for (int x = 0; x < world::CX; ++x) {
          for (int z = 0; z < world::CZ; ++z) {
            const int h = world::heightAt(noise, cx * 16 + x, cz * 16 + z, ver);
            ++s.columns;
            bool colWet = false;
            for (int y = 3; y < h && y < world::WH; ++y) {
              const world::BlockId id = chunk.data->voxels.get(world::localIdx(x, y, z));
              const bool isWater = id == world::wk().water;
              if (!isWater && id != world::kAir) continue;
              ++s.carved;
              if (y <= world::deepOreCeiling(ver)) ++s.deepCarved;
              if (!isWater) continue;
              ++s.wet;
              colWet = true;
              if (y <= world::deepOreCeiling(ver)) ++s.deepWet;
              if (y > world::pocketCeiling(ver)) ++s.aboveCeiling;
              // Nothing may hang in the air: a lake is filled from its floor up,
              // so whatever is under a wet cell is either rock or more water.
              const world::BlockId under = chunk.data->voxels.get(world::localIdx(x, y - 1, z));
              if (under == world::kAir) ++s.floating;
              // Does it touch more water sideways? A lake should read as a body of
              // water rather than as a scattering of single wet cells, and noise
              // asked for a cell at a time is exactly how you get the scattering.
              // Chunk-interior only, so the borders do not count as isolated.
              if (x > 0 && x < world::CX - 1 && z > 0 && z < world::CZ - 1) {
                const bool touch =
                    chunk.data->voxels.get(world::localIdx(x + 1, y, z)) == world::wk().water ||
                    chunk.data->voxels.get(world::localIdx(x - 1, y, z)) == world::wk().water ||
                    chunk.data->voxels.get(world::localIdx(x, y, z + 1)) == world::wk().water ||
                    chunk.data->voxels.get(world::localIdx(x, y, z - 1)) == world::wk().water;
                if (touch) ++s.joined;
              }
            }
            if (colWet) ++s.wetColumns;
          }
        }
      }
    }
    return s;
  };

  const Survey v3 = survey(3);
  const Survey v4 = survey(4);
  const auto pct = [](long long a, long long b) {
    return 100.0 * static_cast<double>(a) / static_cast<double>(std::max(1LL, b));
  };

  // The complaint, measured. This is what a v3 world does and why v4 exists.
  checkf(pct(v3.deepWet, v3.deepCarved) > 99.0,
         "a v3 world floods the whole deep ore band (%.1f%% of carved cells)",
         pct(v3.deepWet, v3.deepCarved));

  checkf(pct(v4.deepWet, v4.deepCarved) < 45.0,
         "v4 leaves the deep ore band mostly dry (%.1f%% wet, was %.1f%%)",
         pct(v4.deepWet, v4.deepCarved), pct(v3.deepWet, v3.deepCarved));
  checkf(pct(v4.wet, v4.carved) < 25.0, "and the underground as a whole (%.1f%% wet, was %.1f%%)",
         pct(v4.wet, v4.carved), pct(v3.wet, v3.carved));

  // But it is still there to be found -- an underground with no water at all would
  // be its own kind of wrong, and would pass every check above.
  checkf(pct(v4.wet, v4.carved) > 1.0, "while still putting water underground at all (%.1f%%)",
         pct(v4.wet, v4.carved));
  checkf(pct(v4.wetColumns, v4.columns) > 5.0,
         "spread over enough of the world to be met rather than hunted (%.1f%% of columns)",
         pct(v4.wetColumns, v4.columns));

  // The two structural promises, which are what stop a pocket behaving oddly.
  checkf(v4.floating == 0, "every pocket rests on something rather than hanging (%d floating)",
         v4.floating);
  checkf(v4.aboveCeiling == 0, "and none of it reaches above the pocket ceiling (%d cells)",
         v4.aboveCeiling);
  checkf(pct(v4.joined, v4.wet) > 70.0,
         "and it pools into bodies rather than scattering (%.1f%% of wet cells touch another)",
         pct(v4.joined, v4.wet));
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
            const world::BlockId id = chunk.data->voxels.get(world::localIdx(x, y, z));
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
        if (legacy.data->voxels.get(y * world::CX * world::CZ + i) != world::kAir) clean = false;
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

// --- any-wood smelting and the recipe book's auto-fill -------------------------
void testRecipeConvenience() {
  std::printf("recipe convenience\n");

  // --- charcoal takes any log ---------------------------------------------------
  //
  // It read "Oak Log" in the book and meant it: the smelting table matched by
  // string, so a forge in a pine forest refused every log fed into it. The same
  // trap the wooden tools had, in the one table that had not been converted.
  {
    int burned = 0, kinds = 0;
    for (const world::BlockDef& d : world::blocks().all()) {
      if (!d.isLog) continue;
      ++kinds;
      const game::SmeltingRecipe* s = game::smeltingFor(d.key);
      if (s && s->out == "charcoal") ++burned;
    }
    checkf(kinds >= 5, "there are %d kinds of log", kinds);
    checkf(burned == kinds, "every one of them smelts to charcoal (%d/%d)", burned, kinds);
    check(game::smeltingFor("greystone") == nullptr, "and stone still does not");
  }

  // --- the Atlas needs three separate slots of paper ----------------------------
  //
  // The shapeless matcher counts occupied CELLS, not items, so three paper in one
  // square is not the recipe however much the count badge implied it was.
  {
    std::vector<game::ItemStack> grid(9);
    grid[0] = {"azurite", 1, -1};
    grid[1] = {"leather", 1, -1};
    grid[2] = {"paper", 3, -1};
    check(!game::matchGrid(grid, 3, game::CraftStation::Workbench),
          "three paper stacked in one slot does not make an Atlas");
    grid[2] = {"paper", 1, -1};
    grid[3] = {"paper", 1, -1};
    grid[4] = {"paper", 1, -1};
    const game::CraftMatch m = game::matchGrid(grid, 3, game::CraftStation::Workbench);
    check(m && m.outKey == "atlas", "three paper in three slots does");
  }

  // --- auto-fill lays a recipe out and takes it from the bag --------------------
  {
    game::Inventory inv;
    ui::InventoryUI panel;
    panel.attach(&inv, nullptr);
    panel.open(ui::InventoryMode::Workbench, nullptr);

    const game::Recipe* chest = nullptr;
    for (const game::Recipe& r : game::recipeBook().recipes()) {
      if (r.outKey == "chest") chest = &r;
    }
    check(chest != nullptr, "the chest recipe exists");
    if (!chest) return;

    // Nothing in the bag: refused, and nothing taken.
    checkf(panel.autoFill(*chest) == ui::InventoryUI::FillResult::Missing,
           "auto-fill refuses a recipe you cannot afford");

    // Eight planks is exactly a chest. Deliberately BIRCH, so the "#planks" tag
    // has to resolve against what is actually held rather than the literal key.
    inv.give("birch_planks", 8);
    checkf(panel.autoFill(*chest) == ui::InventoryUI::FillResult::Ok,
           "and lays it out once you have the wood");
    checkf(inv.countOf("birch_planks") == 0, "taking every plank it needed (%d left)",
           inv.countOf("birch_planks"));

    // Closing returns the grid, which is what makes a second click safe.
    panel.close();
    checkf(inv.countOf("birch_planks") == 8, "and closing puts them all back (%d)",
           inv.countOf("birch_planks"));

    // A bench recipe in the 2x2 is refused rather than half-placed.
    panel.open(ui::InventoryMode::Inventory, nullptr);
    checkf(panel.autoFill(*chest) == ui::InventoryUI::FillResult::TooBig,
           "a bench recipe will not fit the 2x2");
    checkf(inv.countOf("birch_planks") == 8, "and takes nothing when it refuses (%d)",
           inv.countOf("birch_planks"));
  }
}

// --- paintings ----------------------------------------------------------------
//
// The picture is the state; everything else about a painting is an ordinary wall
// block. So these check the two things that could quietly lose one: that it
// survives an encode/decode round trip, and that it goes when its block does.
// Defined with the block-support group further down, which is where it belongs;
// paintings need it because losing a wall is a support event like any other.
int settleBlocks(world::World& w, int maxTicks = 400);

void testPaintings() {
  std::printf("paintings\n");
  const world::BlockId canvas = world::wk().canvas;

  // A recognisable picture: a gradient, so a transposed or shifted decode shows up
  // rather than comparing equal by luck.
  const auto makeArt = [](int salt) {
    game::Painting art;
    art.rgb.resize(game::kPaintingBytes);
    for (int y = 0; y < game::kPaintingSize; ++y) {
      for (int x = 0; x < game::kPaintingSize; ++x) {
        const std::size_t o = (static_cast<std::size_t>(y) * game::kPaintingSize + x) * 3;
        art.rgb[o + 0] = static_cast<std::uint8_t>(x + salt);
        art.rgb[o + 1] = static_cast<std::uint8_t>(y * 2);
        art.rgb[o + 2] = static_cast<std::uint8_t>((x ^ y) + salt);
      }
    }
    art.source = "probe.png";
    return art;
  };

  // --- it goes when the wall it hangs on does -----------------------------------
  {
    auto w = makeWorld();
    w->setBlock(6, kY, 6, world::wk().greystone, 0);
    w->setBlock(5, kY, 6, canvas, 0);  // meta 0: hung on the +x wall, which is (6, ..)
    w->setPainting(5, kY, 6, makeArt(0));
    check(w->painting(5, kY, 6) != nullptr, "a hung picture is there");

    const std::uint32_t before = w->paintingRevision();
    w->setBlock(5, kY, 6, world::kAir, 0);
    check(w->painting(5, kY, 6) == nullptr, "and is gone when the canvas is broken");
    check(w->paintingRevision() != before, "which the renderer is told about");
  }

  // --- and when the wall goes, the support rules take the canvas with it ---------
  {
    auto w = makeWorld();
    w->setBlock(6, kY, 6, world::wk().greystone, 0);
    w->setBlock(5, kY, 6, canvas, 0);
    w->setPainting(5, kY, 6, makeArt(7));
    settleBlocks(*w);
    check(w->getBlock(5, kY, 6) == canvas, "a painting hangs on its wall");
    w->setBlock(6, kY, 6, world::kAir, 0);
    settleBlocks(*w);
    check(w->getBlock(5, kY, 6) == world::kAir, "mining the wall drops the painting");
    check(w->painting(5, kY, 6) == nullptr, "and the picture with it");
  }

  // --- changing the picture is a change ------------------------------------------
  //
  // The renderer caches an uploaded texture per POSITION, and a position is not a
  // picture: choosing a second screenshot for a painting already on the wall
  // leaves the key exactly where it was. Everything that lets the cache notice
  // rests on the stamp being fresh every time, so that is what is asserted here
  // rather than the cache itself, which needs a GL context to exist.
  {
    auto w = makeWorld();
    w->setBlock(6, kY, 6, world::wk().greystone, 0);
    w->setBlock(5, kY, 6, canvas, 0);

    w->setPainting(5, kY, 6, makeArt(1));
    const game::Painting* first = w->painting(5, kY, 6);
    check(first && first->stamp != 0, "a hung picture is stamped");
    const std::uint64_t firstStamp = first ? first->stamp : 0;
    const std::uint32_t firstRev = w->paintingRevision();

    w->setPainting(5, kY, 6, makeArt(2));
    const game::Painting* second = w->painting(5, kY, 6);
    check(second != nullptr, "and can be given a different one");
    if (second) {
      checkf(second->stamp != firstStamp, "which is a different picture (%llu vs %llu)",
             static_cast<unsigned long long>(firstStamp),
             static_cast<unsigned long long>(second->stamp));
    }
    check(w->paintingRevision() != firstRev, "and the renderer is told to look again");

    // The pixels really did change, checked against a fresh copy rather than the
    // dangling `first` pointer, which the second insert invalidated.
    const game::Painting expected = makeArt(2);
    check(second && second->rgb == expected.rgb, "and the picture on the wall is the new one");
  }

  // --- a loaded world's paintings are stamped too ---------------------------------
  //
  // Otherwise every painting in every world would arrive with the same stamp of
  // zero, and a renderer that outlived the last world could match one of its
  // entries against a painting hanging in the same place in this one.
  {
    auto w = makeWorld();
    std::unordered_map<game::BlockEntityKey, game::Painting> loaded;
    loaded[game::blockEntityKey(1, 2, 3)] = makeArt(4);
    loaded[game::blockEntityKey(4, 5, 6)] = makeArt(5);
    check(loaded[game::blockEntityKey(1, 2, 3)].stamp == 0, "a decoded painting has no stamp");
    w->installPaintings(loaded);
    const game::Painting* a = w->painting(1, 2, 3);
    const game::Painting* b = w->painting(4, 5, 6);
    check(a && b && a->stamp != 0 && b->stamp != 0, "installing a save stamps them");
    check(a && b && a->stamp != b->stamp, "each one distinctly");
  }

  // --- the round trip -----------------------------------------------------------
  {
    save::WorldSave out;
    out.meta.id = "paint";
    out.meta.name = "Paint";
    out.meta.seed = 1234u;
    out.meta.genVersion = 3;
    const game::Painting art = makeArt(19);
    out.paintings[game::blockEntityKey(-33, 70, 12)] = art;

    const std::vector<std::uint8_t> bytes = save::encode(out);
    save::WorldSave back;
    std::string error;
    checkf(save::decode(bytes.data(), bytes.size(), back, &error),
           "a world with a painting decodes (%s)", error.c_str());
    checkf(back.paintings.size() == 1, "and brings back exactly one (%zu)",
           back.paintings.size());
    const auto it = back.paintings.find(game::blockEntityKey(-33, 70, 12));
    if (it != back.paintings.end()) {
      check(it->second.rgb == art.rgb, "with every pixel where it was");
      check(it->second.source == art.source, "and the name of the shot it came from");
    } else {
      check(false, "at the negative-coordinate position it was stored at");
    }

    // Deterministic encoding is the property the whole round-trip test rests on.
    check(save::encode(back) == bytes, "and re-encodes to the same bytes");
  }

  // --- a truncated painting section is refused rather than half-read -------------
  {
    save::WorldSave out;
    out.meta.id = "paint";
    out.meta.seed = 1u;
    out.meta.genVersion = 3;
    out.paintings[game::blockEntityKey(0, 64, 0)] = makeArt(3);
    std::vector<std::uint8_t> bytes = save::encode(out);
    // Lop off a quarter of the picture. The header's own length and CRC catch this
    // first, which is the point: there is no path where a short file yields a
    // partly-filled painting.
    bytes.resize(bytes.size() - game::kPaintingBytes / 4);
    save::WorldSave back;
    std::string error;
    check(!save::decode(bytes.data(), bytes.size(), back, &error),
          "a truncated painting section is refused");
  }
}

// --- the held-item swing ------------------------------------------------------
//
// One property, asserted for every hold style: a swing brings the business end of
// the item DOWN the screen and FORWARD into the scene. That is what a swing means,
// and it is exactly what silently stopped being true when the tool pose was yawed
// a quarter turn — the arc was being added into the style's own Euler angles, so
// the axis it turned about was whatever the style had rotated it to. Nothing about
// the pose looked wrong at rest, which is why it took a play-test to find.
void testViewmodelSwing() {
  std::printf("held item swing\n");

  struct Case {
    const char* name;
    const render::HoldStyle& style;
  };
  const Case cases[] = {
      {"tool", render::holdStyles::tool()},   {"sword", render::holdStyles::sword()},
      {"shovel", render::holdStyles::shovel()}, {"block", render::holdStyles::block()},
      {"item", render::holdStyles::item()},   {"panel", render::holdStyles::panel()},
      {"food", render::holdStyles::food()},
  };

  // The tip of a bottom-centred unit mesh: the head of a tool, the point of a
  // sword, the far corner of a block.
  const auto tip = [](const Mat4& m) {
    return Vec3{m.m[4] + m.m[12], m.m[5] + m.m[13], m.m[6] + m.m[14]};
  };
  constexpr float kFov = 1.2217304f;  // 70 degrees
  constexpr float kAspect = 16.0f / 9.0f;

  for (const Case& c : cases) {
    render::Viewmodel rest;
    rest.setItem("probe");
    // Run the equip animation out, so what is measured is the swing alone.
    for (int i = 0; i < 40; ++i) rest.update(0.016f, 0.0f, 0.0f);
    const Vec3 before = tip(rest.modelMatrix(c.style, kFov, kAspect));

    render::Viewmodel swung = rest;
    swung.swing();
    // Just past a third of the way in, which is where the broad arc peaks.
    for (int i = 0; i < 6; ++i) swung.update(0.016f, 0.0f, 0.0f);
    const Vec3 after = tip(swung.modelMatrix(c.style, kFov, kAspect));

    const float down = before.y - after.y;
    const float across = std::fabs(after.x - before.x);
    checkf(down > 0.02f, "a %s swing brings its head down the screen (%.3f -> %.3f)", c.name,
           before.y, after.y);
    checkf(after.z < before.z - 0.01f, "and forward into the scene (%.3f -> %.3f)", before.z,
           after.z);
    // The one that matters, and the one that broke: the arc has to be mostly a
    // drop, not mostly a slew across the screen.
    checkf(down > across, "and travels further down than across (%.3f vs %.3f)", down, across);
  }

  // Not vacuous: the form this replaced — adding the arc into the style's own
  // angles, which is what the JS did — fails the very first case. Worked out here
  // rather than asserted from memory, so the check above is known to be sensitive
  // to the bug it exists for.
  {
    const render::HoldStyle& st = render::holdStyles::tool();
    const auto tipOf = [&](float rx, float ry, float rz) {
      // Column 1 of Ry*Rx*Rz, which is where a bottom-centred mesh's tip goes.
      const float cx = std::cos(rx), sinx = std::sin(rx);
      const float cy = std::cos(ry), siny = std::sin(ry);
      const float cz = std::cos(rz), sinz = std::sin(rz);
      return Vec3{-cy * sinz + siny * sinx * cz, cx * cz, siny * sinz + cy * sinx * cz};
    };
    const float g = std::sin(std::sqrt(0.37f) * 3.14159265358979323846f);
    const Vec3 restTip = tipOf(st.rot.x, st.rot.y, st.rot.z);
    const Vec3 addTip = tipOf(st.rot.x - 0.78f * g, st.rot.y, st.rot.z - 0.26f * g);
    const float addDown = restTip.y - addTip.y;
    const float addAcross = std::fabs(addTip.x - restTip.x);
    checkf(addAcross > addDown,
           "adding the arc into the pose instead slews the head across the screen further than "
           "it lowers it (%.3f across vs %.3f down), which is the sideways dig",
           addAcross, addDown);
  }

  // And the pose itself is untouched at rest: an idle item must sit where its
  // style says, or every one of the numbers in itemmodel.cpp means something
  // different from what it claims.
  {
    render::Viewmodel vm;
    vm.setItem("probe");
    for (int i = 0; i < 40; ++i) vm.update(0.0f, 0.0f, 0.0f);
    const Mat4 a = vm.modelMatrix(render::holdStyles::tool(), kFov, kAspect);
    for (int i = 0; i < 40; ++i) vm.update(0.0f, 0.0f, 0.0f);
    const Mat4 b = vm.modelMatrix(render::holdStyles::tool(), kFov, kAspect);
    bool same = true;
    for (int i = 0; i < 16; ++i) {
      if (std::fabs(a.m[i] - b.m[i]) > 1e-5f) same = false;
    }
    check(same, "and a still hand with a stopped clock holds the pose exactly");
  }
}

// --- where a new world puts you ----------------------------------------------
//
// Checked against real generated voxels rather than against the fields findSpawn
// consults, because the bug this exists to catch was precisely a check that
// consulted the wrong field: ravines are carved after the heightmap, out of ground
// heightAt still calls solid, so a spawn chosen on height alone can be the lip of a
// thirty-block drop and every heightmap-shaped test of it will pass.
void testSpawnChoice() {
  std::printf("spawn selection\n");
  constexpr int kSeeds = 24;
  constexpr int kVer = 3;
  const int sea = world::seaLevel(kVer);

  // Topmost solid cell of a real generated column. Chunks are generated once and
  // kept, since the probe grid revisits the same few.
  struct Ground {
    const world::NoiseSet& noise;
    std::unordered_map<std::uint64_t, std::shared_ptr<world::ChunkData>> chunks;

    int topSolid(int wx, int wz) {
      const int cx = world::World::floorDiv16(wx), cz = world::World::floorDiv16(wz);
      const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32) |
                                static_cast<std::uint32_t>(cz);
      auto it = chunks.find(key);
      if (it == chunks.end()) {
        world::Chunk chunk;
        chunk.cx = cx;
        chunk.cz = cz;
        chunk.data = std::make_shared<world::ChunkData>();
        world::generate(chunk, noise, kVer);
        it = chunks.emplace(key, chunk.data).first;
      }
      const int lx = wx - cx * world::CX, lz = wz - cz * world::CZ;
      for (int y = world::WH - 1; y >= 0; --y) {
        if (world::blocks().solid(it->second->voxels.get(world::localIdx(lx, y, lz)))) return y;
      }
      return 0;
    }
  };

  // A drop is "worth dying down" when it can kill outright from full health:
  // damage is floor(fallen - kFallSafe), so that is kFallSafe + kMaxHealth. Taken
  // from the survival constants rather than picked, because the point is not that
  // spawn is flat — cliffs are scenery — but that it is not a death loop.
  const int kLethal =
      static_cast<int>(game::survivalConst::kFallSafe + game::survivalConst::kMaxHealth);

  int wet = 0, holed = 0, moved = 0, originWet = 0, originCracked = 0;
  int worstDrop = 0;
  for (int i = 0; i < kSeeds; ++i) {
    // Spread out with a 32-bit odd multiplier so consecutive seeds are unrelated
    // worlds, and above INT32_MAX often enough to exercise the unsigned paths.
    const std::uint32_t seed = 1013904223u + static_cast<std::uint32_t>(i) * 2654435761u;
    world::World w(seed, 4, kVer);
    const Vec3 spot = w.findSpawn(8.5f, 8.5f);
    const int sx = static_cast<int>(std::floor(spot.x));
    const int sz = static_cast<int>(std::floor(spot.z));

    world::NoiseSet noise(seed);
    Ground ground{noise, {}};
    const int here = ground.topSolid(sx, sz);
    if (here < sea + 1) ++wet;
    if (sx != 8 || sz != 8) ++moved;

    // Nothing within eight blocks may be a long way below the ground you land on.
    int drop = 0;
    for (int dz = -8; dz <= 8; dz += 2) {
      for (int dx = -8; dx <= 8; dx += 2) {
        const int d = here - ground.topSolid(sx + dx, sz + dz);
        if (d > drop) drop = d;
      }
    }
    if (drop > worstDrop) worstDrop = drop;
    if (drop >= kLethal) ++holed;

    // And the same questions of the origin column, which is where a death used to
    // put you back however far the spawn had been moved from it.
    if (world::heightAt(noise, 8, 8, kVer) < sea + 1) ++originWet;
    bool cracked = false;
    for (int dz = -8; dz <= 8 && !cracked; dz += 2) {
      for (int dx = -8; dx <= 8 && !cracked; dx += 2) {
        cracked = world::ravineAt(noise, 8 + dx, 8 + dz, kVer);
      }
    }
    if (cracked) ++originCracked;
  }

  checkf(wet == 0, "%d seeds all start on dry land (%d in water)", kSeeds, wet);
  checkf(holed == 0, "and none beside a drop that could kill outright (%d over %d, worst %d)",
         holed, kLethal, worstDrop);
  // None of the above is evidence of anything if every seed already put you
  // somewhere good. These three say the hazards are real at the rate a player
  // meets them, and that the search is what avoids them.
  checkf(moved > 0, "and the search moves off the origin to manage it (%d/%d seeds)", moved,
         kSeeds);
  checkf(originWet > 0, "while the origin column itself is under water on %d of them", originWet);
  checkf(originCracked > 0, "and has a ravine within eight blocks on %d", originCracked);
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

int settleBlocks(world::World& w, int maxTicks) {
  for (int i = 0; i < maxTicks; ++i) {
    if (w.blockUpdates().pending() == 0) return i;
    w.tickBlockUpdates(0.0625f);
  }
  return maxTicks;
}

// --- lighting across a chunk border ------------------------------------------
//
// Lighting is per chunk: each pass rebuilds one chunk from zero, reading its eight
// neighbours' *stored* light to seed its border. That makes the whole thing a
// fixed-point iteration, and a fixed-point iteration only converges if something
// keeps kicking it — which for a long time nothing did. Installing a chunk's light
// re-meshed its neighbours but never re-lit them, so light that had to cross a seam
// crossed it only if the two chunks happened to be lit in the right order, and a
// torch a block from a border lit its own chunk and stopped dead at the line.
// --- chunk storage -----------------------------------------------------------
//
// A chunk keeps each of its four per-cell arrays a band at a time, and each band
// either as one repeated value or as a dense buffer. Everything here is about the
// two ways that can go wrong: addressing the wrong band, which quietly corrupts
// a world, and failing to stay uniform, which quietly gives back the memory the
// whole exercise was for.
void testChunkStorage() {
  std::printf("banded chunk storage\n");

  // The property every `i >> kBandShift` in the codebase rests on. There is a
  // static_assert for one case; this is all of them, because getting it wrong for
  // some y and not others is exactly the shape of bug that survives a spot check.
  {
    bool ok = true;
    for (int y = 0; y < world::WH && ok; ++y) {
      for (int z = 0; z < world::CZ && ok; ++z) {
        for (int x = 0; x < world::CX; ++x) {
          const int i = world::localIdx(x, y, z);
          if ((i >> world::kBandShift) != y / world::kBandHeight) ok = false;
        }
      }
    }
    check(ok, "every cell lands in the band its y says it should");
  }

  {
    world::Banded<std::uint8_t> a;
    check(a.bytes() == 0, "a fresh column holds no buffers at all");
    check(a.get(0) == 0 && a.get(world::kCellsPerChunk - 1) == 0, "and reads as zero");

    // Writing a band's own value back must not be what makes it dense — this is
    // the case that fires thousands of times a tick from the water simulation.
    a.set(world::localIdx(3, 40, 9), 0);
    check(a.bytes() == 0, "writing a band's own value back keeps it free");

    a.set(world::localIdx(3, 40, 9), 7);
    checkf(a.bytes() == world::kCellsPerBand, "one differing cell costs one band (%zu bytes)",
           a.bytes());
    check(a.get(world::localIdx(3, 40, 9)) == 7, "and reads back");
    check(a.get(world::localIdx(4, 40, 9)) == 0, "without disturbing its neighbour");
    check(a.get(world::localIdx(3, 8, 9)) == 0, "or the band below");

    a.set(world::localIdx(3, 40, 9), 0);
    check(a.bytes() == world::kCellsPerBand, "putting it back does not un-dense on its own");
    a.compact();
    check(a.bytes() == 0, "but compacting notices they all agree again");
    check(a.get(world::localIdx(3, 40, 9)) == 0, "and the value survives the collapse");
  }

  // Every cell, written and read back, against a plain array holding the same
  // thing. Catches an off-by-one in the band index that a sparse test would miss.
  {
    world::Banded<world::BlockId> s;
    std::vector<world::BlockId> flat(world::kCellsPerChunk);
    std::uint32_t state = 20260804u;
    const auto next = [&state] {
      state = state * 1664525u + 1013904223u;
      return state >> 16;
    };
    for (int i = 0; i < world::kCellsPerChunk; ++i) {
      // Mostly one value, so some bands stay uniform and some do not.
      const std::uint32_t r = next();
      const world::BlockId v =
          (r & 63) == 0 ? static_cast<world::BlockId>(1 + (r >> 6) % 40) : 0;
      flat[i] = v;
      s.set(i, v);
    }
    bool same = true;
    for (int i = 0; i < world::kCellsPerChunk && same; ++i) same = s.get(i) == flat[i];
    check(same, "every one of the 49152 cells reads back what was written");

    world::Banded<world::BlockId> loaded;
    loaded.loadFrom(flat.data());
    check(loaded == s, "loadFrom builds the same column that set() did");

    std::vector<world::BlockId> out(world::kCellsPerChunk, 0xFFFF);
    s.copyTo(out.data());
    check(out == flat, "and copyTo hands back exactly what went in");
  }

  // The point of the exercise, on real terrain rather than a synthetic pattern.
  {
    world::Chunk chunk;
    chunk.cx = 7;
    chunk.cz = -3;
    world::generate(chunk, world::NoiseSet(3918175327u), world::kGenVersion);
    const std::size_t flat = static_cast<std::size_t>(world::kCellsPerChunk) * 5;
    const std::size_t got = chunk.data->bytes();
    checkf(got < flat / 2, "a generated chunk costs under half of flat storage (%zu vs %zu KB)",
           got / 1024, flat / 1024);
  }
}

// Rebuilds the light of every lit chunk from zero, repeatedly, until the whole set
// stops changing — and reports the worst disagreement with what the world is
// actually holding.
//
// This is the check the incremental engine lives or dies by. computeLight is a
// whole-chunk rebuild seeded from its neighbours, so iterating it over the loaded
// set until it settles lands on the one true answer: the least assignment where
// every passable cell is at least its brightest neighbour minus one. The
// incremental add/remove passes claim to keep the world sitting exactly there
// after any sequence of edits, and nothing else in the suite would notice if they
// drifted — the golden mesh dump calls computeLight itself, so it compares the
// rebuild against the rebuild.
//
// The two directions of failure are both real bugs and they read differently:
// incremental BELOW the rebuild is light that never arrived (the torch whose glow
// stops at a chunk border), incremental ABOVE it is light that never left (the
// removed torch that keeps shining).
//
// Only lit chunks are rebuilt, and everything else is treated as absent, because
// that is precisely what the running world does: submitLight hands a worker the
// neighbour's arrays whether or not they hold anything, and an unlit chunk's are
// all zero, which contributes exactly what a missing chunk does.
int worstLightDrift(world::World& w, std::string& where) {
  std::unordered_map<world::ChunkKey, std::shared_ptr<world::ChunkData>> ref;
  for (const auto& [key, lc] : w.chunks()) {
    if (!lc->chunk.generated || !lc->chunk.lit) continue;
    ref[key] = std::make_shared<world::ChunkData>(*lc->chunk.data);
  }
  // Zeroed, so the rebuild cannot inherit any part of the answer it is checking.
  for (auto& [key, d] : ref) {
    d->skylight.fill(0);
    d->blocklight.fill(0);
  }

  std::vector<world::Emitter> scratch;
  for (int round = 0; round < 64; ++round) {
    bool changed = false;
    for (auto& [key, d] : ref) {
      world::LightNeighbourhood nb;
      nb.cx = world::keyCx(key);
      nb.cz = world::keyCz(key);
      for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
          auto it = ref.find(world::chunkKey(nb.cx + dx, nb.cz + dz));
          nb.grid[(dz + 1) * 3 + (dx + 1)] = it == ref.end() ? nullptr : it->second.get();
        }
      }
      nb.grid[4] = d.get();
      const auto beforeSky = d->skylight;
      const auto beforeBlock = d->blocklight;
      world::computeLight(*d, nb, scratch);
      if (d->skylight != beforeSky || d->blocklight != beforeBlock) changed = true;
    }
    if (!changed) break;
  }

  int worst = 0;
  for (const auto& [key, d] : ref) {
    const world::LoadedChunk* lc = w.chunkAt(world::keyCx(key), world::keyCz(key));
    const world::ChunkData& live = *lc->chunk.data;
    for (int i = 0; i < world::kCellsPerChunk; ++i) {
      const int ds = static_cast<int>(live.skylight.get(i)) - static_cast<int>(d->skylight.get(i));
      const int db = static_cast<int>(live.blocklight.get(i)) - static_cast<int>(d->blocklight.get(i));
      for (const auto& [delta, name] :
           {std::pair<int, const char*>{ds, "sky"}, std::pair<int, const char*>{db, "block"}}) {
        if (std::abs(delta) <= worst) continue;
        worst = std::abs(delta);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s at (%d,%d,%d): world %d, rebuild %d", name,
                      world::keyCx(key) * world::CX + world::idxX(i), world::idxY(i),
                      world::keyCz(key) * world::CZ + world::idxZ(i),
                      static_cast<int>(name[0] == 's' ? live.skylight.get(i) : live.blocklight.get(i)),
                      static_cast<int>(name[0] == 's' ? d->skylight.get(i) : d->blocklight.get(i)));
        where = buf;
      }
    }
  }
  return worst;
}

void testLighting() {
  std::printf("lighting across chunk borders\n");

  const world::BlockId torch = world::wk().emberlight;
  auto w = makeArena();
  w->waitForIdle(kOriginX, kOriginZ);

  // A torch in the last column of chunk 0. Its light has to reach chunk 1, which
  // begins one cell away.
  w->setBlock(15, kY, 8, torch, 0);
  w->waitForIdle(kOriginX, kOriginZ);

  checkf(w->getBlockLight(15, kY, 8) == 14, "a torch lights its own cell to 14 (%d)",
         w->getBlockLight(15, kY, 8));
  checkf(w->getBlockLight(14, kY, 8) == 13, "and 13 one cell away inside its chunk (%d)",
         w->getBlockLight(14, kY, 8));
  // The seam. Same distance from the torch as the cell above, different chunk.
  checkf(w->getBlockLight(16, kY, 8) == 13, "and 13 one cell away ACROSS the seam (%d)",
         w->getBlockLight(16, kY, 8));
  checkf(w->getBlockLight(20, kY, 8) == 9, "still falling off correctly five cells in (%d)",
         w->getBlockLight(20, kY, 8));

  // And it has to go away again the same way.
  w->setBlock(15, kY, 8, world::kAir, 0);
  w->waitForIdle(kOriginX, kOriginZ);
  checkf(w->getBlockLight(16, kY, 8) == 0, "taking the torch away unlights the far side (%d)",
         w->getBlockLight(16, kY, 8));

  // Skylight has the same problem in reverse: a sealed room straddling a seam kept
  // whatever daylight had leaked along it. This is the case the monster spawner
  // trips over, since a lit cell is a cell it will not spawn in.
  for (int x = 12; x <= 20; ++x) {
    for (int z = 4; z <= 12; ++z) {
      w->setBlock(x, kY + 2, z, world::wk().greystone, 0);
      const bool wall = x == 12 || x == 20 || z == 4 || z == 12;
      if (!wall) continue;
      w->setBlock(x, kY, z, world::wk().greystone, 0);
      w->setBlock(x, kY + 1, z, world::wk().greystone, 0);
    }
  }
  w->waitForIdle(kOriginX, kOriginZ);
  int brightest = 0;
  for (int x = 13; x <= 19; ++x) {
    for (int z = 5; z <= 11; ++z) brightest = std::max(brightest, w->getSky(x, kY, z));
  }
  checkf(brightest == 0, "a sealed room straddling a seam is dark all the way across (%d)",
         brightest);

  // Two torches, one removed. The remove pass has to tell light that came from the
  // torch being taken away apart from light that was always the other one's — get
  // that wrong and either the survivor's glow is scrubbed out with its neighbour's
  // or the dead one's is left behind.
  {
    w->setBlock(14, kY, 2, torch, 0);
    w->setBlock(18, kY, 2, torch, 0);
    w->waitForIdle(kOriginX, kOriginZ);
    const int both = w->getBlockLight(16, kY, 2);
    w->setBlock(14, kY, 2, world::kAir, 0);
    w->waitForIdle(kOriginX, kOriginZ);
    checkf(w->getBlockLight(18, kY, 2) == 14, "the torch that stayed is untouched (%d)",
           w->getBlockLight(18, kY, 2));
    checkf(w->getBlockLight(16, kY, 2) == 12, "and the cell between them keeps only its half (%d)",
           w->getBlockLight(16, kY, 2));
    checkf(both == 12, "which is what it held all along, since the two were equal (%d)", both);
    w->setBlock(18, kY, 2, world::kAir, 0);
  }

  // Skylight down a column, which is the one source that is not a block: sealing a
  // roof has to put the whole shaft below it into shadow, and opening it again has
  // to let the sun all the way back down.
  {
    for (int y = kY; y <= kY + 5; ++y) w->setBlock(3, y, 3, world::kAir, 0);
    w->waitForIdle(kOriginX, kOriginZ);
    const int open = w->getSky(3, kY, 3);
    w->setBlock(3, kY + 5, 3, world::wk().greystone, 0);
    w->waitForIdle(kOriginX, kOriginZ);
    const int capped = w->getSky(3, kY, 3);
    w->setBlock(3, kY + 5, 3, world::kAir, 0);
    w->waitForIdle(kOriginX, kOriginZ);
    checkf(capped < open, "capping a shaft puts the floor under it in shadow (%d -> %d)", open,
           capped);
    checkf(w->getSky(3, kY, 3) == open, "and taking the cap off lets the sun back down (%d)",
           w->getSky(3, kY, 3));
  }

  // The whole point, stated as one number: after all of that, every cell of every
  // lit chunk agrees with a from-scratch rebuild of the entire loaded set.
  {
    std::string where;
    const int drift = worstLightDrift(*w, where);
    checkf(drift == 0, "an edited world sits exactly where a full rebuild would put it (%s)",
           drift == 0 ? "no drift" : where.c_str());
  }

  // And the same for a world nobody has touched, which is the other half of the
  // engine: real terrain with caves and overhangs, lit chunk by chunk on eight
  // workers in whatever order they finish, then stitched across the seams. This is
  // the check the 2.4.0 relight race would have failed.
  {
    world::World fresh(3918175327u, 3);
    fresh.waitForIdle(kOriginX, kOriginZ);
    std::string where;
    const int drift = worstLightDrift(fresh, where);
    checkf(drift == 0, "and so does a freshly streamed one, seams and all (%s)",
           drift == 0 ? "no drift" : where.c_str());
  }
}

// --- what a bulk edit costs --------------------------------------------------
//
// The incremental light passes run synchronously on the main thread inside
// setBlock. One torch is bounded by the light radius and nobody need worry, but
// a player roofing over a valley or a cliff of sand collapsing is one BFS per
// block, back to back, inside a single frame — and that is the case nothing else
// here exercises.
//
// Measured in cells written rather than milliseconds, deliberately. Nothing
// streams during these loops — setBlock does its light work synchronously and
// hands nothing to a worker — so the count is identical on every machine and
// every run, which a wall clock is not. The bounds are two to three times what
// is observed: the point is to catch a change of ALGORITHM, an edit that starts
// costing a whole chunk again, not to fail on a machine having a bad day.
void testBulkEdits() {
  std::printf("what a bulk edit costs\n");

  // Leaves and glass are solidClear: solid, but not opaque. So a felled tree's
  // decay cascade changes no opacity and emits nothing, and the light passes
  // decline it outright. Worth an assertion rather than a comment, because
  // somebody making leaves opaque for a shadow effect would otherwise turn every
  // felled tree into a few hundred floods and have no idea why.
  {
    check(!world::blocks().opaque(world::wk().leaves), "leaves do not block light");
    auto w = makeArena();
    w->waitForIdle(kOriginX, kOriginZ);
    const std::uint64_t before = w->lightWrites();
    for (int x = 2; x <= 10; ++x) {
      for (int z = 2; z <= 10; ++z) {
        for (int y = kY; y <= kY + 3; ++y) w->setBlock(x, y, z, world::wk().leaves, 0);
      }
    }
    for (int x = 2; x <= 10; ++x) {
      for (int z = 2; z <= 10; ++z) {
        for (int y = kY; y <= kY + 3; ++y) w->setBlock(x, y, z, world::kAir, 0);
      }
    }
    checkf(w->lightWrites() == before, "so 648 leaves placed and cleared cost no light work (%llu)",
           static_cast<unsigned long long>(w->lightWrites() - before));
  }

  // Roofing over open ground, then taking the roof off again. Every block placed
  // shadows the column under it; every block removed lets the sun back down and
  // then sideways. This is the heaviest thing a player can do by hand.
  {
    auto w = makeArena();
    w->waitForIdle(kOriginX, kOriginZ);

    const std::uint64_t start = w->lightWrites();
    for (int x = 0; x <= 22; ++x) {
      for (int z = 0; z <= 22; ++z) w->setBlock(x, kY + 5, z, world::wk().greystone, 0);
    }
    const std::uint64_t sealing = w->lightWrites() - start;

    const std::uint64_t mid = w->lightWrites();
    for (int x = 0; x <= 22; ++x) {
      for (int z = 0; z <= 22; ++z) w->setBlock(x, kY + 5, z, world::kAir, 0);
    }
    const std::uint64_t opening = w->lightWrites() - mid;

    // 529 blocks, and about 113 cells written per block: the shadow it casts and
    // the sunlight that refills it. For scale, the scheme this replaced relit the
    // whole 3x3 for every one of them — nine chunks, 49152 cells, two channels,
    // roughly 885,000 cell writes PER BLOCK rather than for all 529 together.
    checkf(sealing < 150000, "roofing 529 cells of sky costs %llu light writes",
           static_cast<unsigned long long>(sealing));
    checkf(opening < 50000, "and taking the roof off again costs %llu",
           static_cast<unsigned long long>(opening));

    // And the world is still exactly where a full rebuild would put it, which is
    // the thing the counts must not have been bought at the expense of.
    w->waitForIdle(kOriginX, kOriginZ);
    std::string where;
    const int drift = worstLightDrift(*w, where);
    checkf(drift == 0, "with the light still correct afterwards (%s)",
           drift == 0 ? "no drift" : where.c_str());
  }

  // A column of sand losing its floor: every block is opaque, so each step of the
  // collapse is a real opacity change in both directions.
  {
    auto w = makeArena();
    for (int y = kY; y <= kY + 5; ++y) {
      for (int x = 4; x <= 12; ++x) {
        for (int z = 4; z <= 12; ++z) w->setBlock(x, y, z, world::wk().sand, 0);
      }
    }
    w->waitForIdle(kOriginX, kOriginZ);
    const std::uint64_t before = w->lightWrites();
    for (int x = 4; x <= 12; ++x) {
      for (int z = 4; z <= 12; ++z) w->setBlock(x, kY - 1, z, world::kAir, 0);
    }
    // Almost free, and for a reason worth knowing: the sand was already in the
    // dark under six of its own layers, so removing the floor under it changes
    // barely any illumination at all. The expensive direction is opening ground
    // to the SKY, not closing it.
    checkf(w->lightWrites() - before < 2000, "dropping the floor from under 81 columns costs %llu",
           static_cast<unsigned long long>(w->lightWrites() - before));
  }
}

// --- sleeping ----------------------------------------------------------------
//
// A bed used to be a button that deleted the night. It is now a clock you can read
// for free, an hour you choose, and a gate that says no until you have been up long
// enough to have earned it.
void testSleep() {
  std::printf("sleep and the Time Wheel\n");

  // --- the tiredness gate ---
  {
    render::Sky sky;
    sky.time = 0.32f;
    check(!sky.tired(), "a freshly rested player cannot go straight back to bed");
    checkf(std::fabs(sky.hoursUntilTired() - render::Sky::kRestedHours) < 0.01f,
           "and is told the whole eight hours are still to run (%.2f)", sky.hoursUntilTired());

    // Run the clock forward the eight hours the gate asks for.
    const float secondsPerHour = sky.dayLength / render::Sky::kHoursPerDay;
    for (int i = 0; i < 8; ++i) {
      for (int f = 0; f < 60; ++f) sky.update(secondsPerHour / 60.0f);
    }
    checkf(sky.tired(), "eight game hours awake makes one (%.2f h)", sky.hoursAwake());

    // Sleeping resets it, and the hours slept do not count toward the next one.
    sky.time = 0.8f;
    sky.startSleep(0.27f);
    check(sky.isSleeping(), "confirming starts the sweep");
    for (int i = 0; i < 2000 && sky.isSleeping(); ++i) sky.update(1.0f / 60.0f);
    checkf(std::fabs(sky.time - 0.27f) < 0.002f, "which lands on the hour asked for (%.3f)",
           sky.time);
    checkf(sky.hoursAwake() == 0.0f, "and you wake rested (%.2f h)", sky.hoursAwake());
    check(!sky.tired(), "so a bed will not take you twice in a row");
  }

  // --- sleeping to an arbitrary hour, not just dawn ---
  {
    render::Sky sky;
    sky.time = 0.5f;  // noon
    sky.setHoursAwake(render::Sky::kRestedHours);
    sky.startSleep(0.75f);  // an afternoon nap to dusk
    for (int i = 0; i < 2000 && sky.isSleeping(); ++i) sky.update(1.0f / 60.0f);
    checkf(std::fabs(sky.time - 0.75f) < 0.002f, "a nap ends where it was aimed (%.3f)",
           sky.time);

    // Asking for the hour it already is means a whole day round, not an instant
    // no-op — which is what the wheel shows when the handle sits on `now`.
    sky.time = 0.5f;
    sky.startSleep(0.5f);
    check(sky.isSleeping(), "asking for the current hour sleeps a full day round");
  }

  // --- the dial's geometry ---
  {
    float ux = 0, uy = 0;
    ui::TimeWheel::timeToUnit(0.0f, ux, uy);
    checkf(std::fabs(ux) < 0.001f && uy < -0.99f, "midnight is straight up (%.2f, %.2f)", ux,
           uy);
    ui::TimeWheel::timeToUnit(0.25f, ux, uy);
    checkf(ux > 0.99f && std::fabs(uy) < 0.001f, "06:00 is a quarter turn clockwise (%.2f, %.2f)",
           ux, uy);
    ui::TimeWheel::timeToUnit(0.5f, ux, uy);
    checkf(std::fabs(ux) < 0.001f && uy > 0.99f, "and noon is straight down (%.2f, %.2f)", ux,
           uy);

    // Every hour has to survive the round trip, or dragging the handle lands
    // somewhere other than where the pointer is.
    float worst = 0.0f;
    for (int i = 0; i < 24; ++i) {
      const float t = static_cast<float>(i) / 24.0f;
      ui::TimeWheel::timeToUnit(t, ux, uy);
      const float back = ui::TimeWheel::angleToTime(ux, uy);
      float delta = std::fabs(back - t);
      if (delta > 0.5f) delta = 1.0f - delta;  // across midnight
      worst = std::max(worst, delta);
    }
    checkf(worst < 0.0005f, "a position on the rim reads back as the hour it was (%.5f)", worst);
  }

  // --- what the wheel offers when you open it ---
  {
    ui::TimeWheel wheel;
    wheel.open(0.85f);  // late evening
    checkf(std::fabs(wheel.target() - render::Sky::kDawn) < 0.01f,
           "opened at night, the handle starts at dawn (%.3f)", wheel.target());
    check(render::Sky::clockStringAt(wheel.target()) == "06:30",
          "snapped to a round hour rather than the raw constant");
    check(!wheel.isVote(), "and it is yours to move");

    // Opened in the morning, dawn is a whole day away and a poor suggestion.
    wheel.open(0.4f);
    checkf(std::fabs(wheel.target() - render::Sky::kDawn) > 0.05f,
           "opened after dawn it does not offer a 23-hour sleep (%.3f)", wheel.target());

    wheel.openVote(0.85f, 0.31f, "Ada");
    check(wheel.isVote(), "a bed opened on somebody else's proposal is a vote");
    checkf(std::fabs(wheel.target() - 0.31f) < 0.001f, "showing their hour, not yours (%.3f)",
           wheel.target());
  }

  // --- how a span reads ---
  {
    check(render::Sky::clockStringAt(0.0f) == "00:00", "midnight prints as 00:00");
    check(render::Sky::clockStringAt(0.5f) == "12:00", "and noon as 12:00");
    check(render::Sky::spanString(0.5f) == "12h 0m", "half a day is twelve hours");
    check(render::Sky::spanString(1.0f / 24.0f) == "1h 0m", "an hour is an hour");
    check(render::Sky::spanString(1.0f / 48.0f) == "30m", "and half of one drops the hours");
  }
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

  // --- a torch on a WALL asks the wall, not the floor ---------------------------
  //
  // The first version of this system only ever looked down, so every wall torch in
  // the world broke the moment anything scheduled it — with nothing beneath one by
  // definition, the check could not come out any other way.
  {
    auto w = makeWorld();
    // A wall at x=6, and a torch on its -x face at x=5: meta 2 leans toward -x, so
    // the wall it hangs on is the cell at +x.
    w->setBlock(6, kY + 1, 11, stone, 0);
    w->setBlock(5, kY + 1, 11, torch, 2);
    settleBlocks(*w);
    check(w->getBlock(5, kY + 1, 11) == torch, "a wall torch stays up with nothing under it");

    // Its own floor coming and going is none of its business.
    w->setBlock(5, kY, 11, stone, 0);
    settleBlocks(*w);
    w->setBlock(5, kY, 11, world::kAir, 0);
    settleBlocks(*w);
    check(w->getBlock(5, kY + 1, 11) == torch, "and is unmoved by the floor beneath it changing");

    // The wall is what holds it.
    w->setBlock(6, kY + 1, 11, world::kAir, 0);
    settleBlocks(*w);
    check(w->getBlock(5, kY + 1, 11) == world::kAir, "but falls when its wall is mined");
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

  // --- sand settles on top of the player, never around them ---------------------
  //
  // A solid block appearing where a body already stands is the one thing the
  // physics sweep cannot resolve gracefully: it snaps the body to the nearest face
  // of the box it is inside, which is a jump rather than a slide, and the nearest
  // face can be through a wall. That is the old close-a-door-on-yourself teleport,
  // and doors refuse to close for exactly that reason. Falling sand is the other
  // way the situation arises and had no such rule.
  {
    auto w = makeWorld();
    game::Player player(12.5f, static_cast<float>(kY) - 1.0f, 12.5f);
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

    // Floor to stand on, a wall pressed against one side of the player, and a
    // block of sand in the air directly overhead. The wall is what turns a bad
    // ejection into a visible one: through it is the shortest way out.
    for (int x = 10; x <= 14; ++x) {
      for (int z = 10; z <= 14; ++z) w->setBlock(x, kY - 2, z, stone, 0);
    }
    for (int y = kY - 1; y <= kY + 1; ++y) w->setBlock(13, y, 12, stone, 0);
    w->setBlock(12, kY + 3, 12, sand, 0);
    settleBlocks(*w);

    const Vec3 before = player.pos();
    game::PlayerOptions options;
    for (int i = 0; i < 240; ++i) {
      entities.tick(1.0f / 60.0f, ctx);
      w->tickBlockUpdates(1.0f / 60.0f);
      // The player is stepped too: an ejection happens on the next sweep, so a
      // test that never sweeps could not see one.
      player.update(1.0f / 60.0f, input, *w, options, i / 60.0);
    }

    const Vec3 after = player.pos();
    checkf(std::fabs(after.x - before.x) < 0.05f && std::fabs(after.z - before.z) < 0.05f,
           "sand landing on the player does not shove them anywhere (%.2f,%.2f -> %.2f,%.2f)",
           before.x, before.z, after.x, after.z);
    check(after.x < 13.0f, "and certainly not through the wall beside them");
    check(w->getBlock(12, kY - 1, 12) != sand && w->getBlock(12, kY, 12) != sand,
          "and no sand is written into a cell they are standing in");

    // It is still there, held as an entity resting on them, rather than lost.
    int waiting = 0;
    for (const game::Entity& fb : entities.all()) {
      if (!fb.dead && fb.type == game::EntityType::FallingBlock) ++waiting;
    }
    checkf(waiting == 1, "the sand waits on top of them instead of vanishing (%d)", waiting);

    // And the moment they step aside it finishes the trip. Landing a cell higher
    // instead would have left it unsupported and falling again on the next tick.
    player.setPos(Vec3{10.5f, static_cast<float>(kY) - 1.0f, 10.5f});
    for (int i = 0; i < 240; ++i) {
      entities.tick(1.0f / 60.0f, ctx);
      w->tickBlockUpdates(1.0f / 60.0f);
      player.update(1.0f / 60.0f, input, *w, options, (240 + i) / 60.0);
    }
    check(w->getBlock(12, kY - 1, 12) == sand, "and lands as soon as they move away");
    int stuck = 0;
    for (const game::Entity& fb : entities.all()) {
      if (!fb.dead && fb.type == game::EntityType::FallingBlock) ++stuck;
    }
    checkf(stuck == 0, "leaving nothing hanging in the air (%d)", stuck);
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
    // The fall fans out from the cell that reaches the bottom, one level thinner,
    // and nothing spreads partway down.
    //
    // This used to assert the opposite, and said so: a falling column spread
    // sideways at *every* level it passed, each of those cells deepened the layer
    // below into a column of its own, and the note here called that "what makes a
    // waterfall widen at its base... exactly what the web build does". It was a
    // faithful port and it does not converge. Every cell the flood reached had
    // water above it, recompute() promotes exactly that to a full column, and a
    // full column spreads at full strength again — so the spill re-energised itself
    // for as long as there was anywhere left to go. Measured: a source poured off a
    // twenty-block wall wet ninety-six thousand cells and took seventeen hundred
    // ticks to stop, against a hundred and thirteen for the same source on flat
    // ground. Water goes down before it goes out now.
    check(w->getBlock(9, kY, 8) == water, "the fall fans out across the floor it lands on");
    checkf((w->getMeta(9, kY, 8) & world::kWaterFalling) == 0 &&
               (w->getMeta(9, kY, 8) & 7) == 1,
           "as a level-one flow rather than a column of its own (meta %d)",
           w->getMeta(9, kY, 8));
    check(w->getBlock(9, kY + 1, 8) != water,
          "and nothing creeps out sideways partway down the fall");
    checkf(w->getBlock(12, kY, 8) == water,
           "while water still reaches four cells from the impact (meta %d)",
           w->getMeta(12, kY, 8));
  }

  // --- a spill is bounded, which is the whole point of the rule above -----------
  //
  // The measurement that found it, kept as a test because "it spreads too far" is
  // not something a reader can check by eye. A source on flat ground reaches seven
  // cells; the same source dropped down a shaft has to land before it spreads, so
  // it reaches seven from the impact and no further.
  {
    auto w = makeWaterWorld();
    w->setBlock(8, kY + 12, 8, water, 0);
    const int ticks = settleWater(*w, 2000);
    int wet = 0, reach = 0, sideways = 0;
    for (int x = kPondMin; x <= kPondMax; ++x) {
      for (int z = kPondMin; z <= kPondMax; ++z) {
        for (int y = kY; y <= kY + 12; ++y) {
          if (w->getBlock(x, y, z) != water) continue;
          ++wet;
          reach = std::max(reach, std::abs(x - 8) + std::abs(z - 8));
          // Anything off the column's own axis, above the floor, is water that
          // spread while it was still falling.
          if (y > kY && (x != 8 || z != 8)) ++sideways;
        }
      }
    }
    checkf(sideways == 0, "a fall stays one cell wide the whole way down (%d cells beside it)",
           sideways);
    checkf(reach <= world::kWaterMaxLevel,
           "and reaches no further from the impact than water on flat ground (%d)", reach);
    checkf(wet < 200, "so the whole spill is %d cells, not thousands", wet);
    checkf(ticks < 100, "and settles in %d ticks", ticks);
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
// --- which networks a beacon goes out on ---------------------------------------
//
// A datagram to 255.255.255.255 from an unbound socket leaves by ONE interface,
// picked by the routing table. A machine with a Hyper-V switch, a VPN or WSL has
// several, and when the stack picks a virtual one the beacon is broadcast
// perfectly into a network with nobody on it — send() succeeds and nothing looks
// wrong. That is why hosting from one machine can work while hosting from the
// other does not.
//
// So a beacon now goes to each interface's own subnet broadcast. This checks the
// arithmetic that produces those addresses, on the machine actually running the
// test — there is no fixture, because the whole point is what the real adapters
// say.
void testNetInterfaces() {
  std::printf("which networks a beacon goes out on\n");

  const std::vector<net::Interface> found = net::localInterfaces();
  // Not an assertion that any exist: this has to pass on a build machine with no
  // network at all, where falling back to the limited broadcast is right.
  std::printf("       %zu broadcastable interface(s)\n", found.size());

  bool everyBroadcastInsideItsSubnet = true;
  bool noLoopback = true;
  bool noDuplicates = true;
  for (std::size_t i = 0; i < found.size(); ++i) {
    const net::Interface& f = found[i];
    std::printf("       %-34s %-15s -> %s\n", f.name.c_str(), f.addressText.c_str(),
                f.broadcastText.c_str());

    // A directed broadcast has to end in a run of set bits and share the rest with
    // the interface: 192.168.10.113/24 gives 192.168.10.255 and nothing else.
    const std::uint32_t a = f.address, b = f.broadcast;
    const std::uint32_t diff = a ^ b;
    // In network byte order the host part is the trailing bytes, so the xor of the
    // two must be a suffix mask once byte-swapped back. Checking the property that
    // actually matters instead: the broadcast is >= the address, and the bits they
    // differ in are exactly the ones the broadcast has set.
    if ((diff & b) != diff) everyBroadcastInsideItsSubnet = false;
    if ((a & 0xFF) == 127) noLoopback = false;  // first octet, network order
    for (std::size_t j = 0; j < i; ++j) {
      if (found[j].address == f.address) noDuplicates = false;
    }
  }
  check(everyBroadcastInsideItsSubnet, "each broadcast address differs from its interface "
                                       "only in bits it has set");
  check(noLoopback, "loopback is not something to look for games on");
  check(noDuplicates, "and no interface is listed twice");

  // The case that motivated all of this: a /32, which is what a VPN like Tailscale
  // hands out. It has no subnet to broadcast into, so it must not be offered one —
  // a "broadcast" there is just the interface talking to itself.
  bool anySlashThirtyTwo = false;
  for (const net::Interface& f : found) {
    if (f.address == f.broadcast) anySlashThirtyTwo = true;
  }
  check(!anySlashThirtyTwo, "and a point-to-point interface is left out rather than "
                            "broadcast to itself");

  // End to end, on this machine: does a host advertising actually get listed by a
  // guest looking? Both halves are real sockets on the real discovery port, so
  // this exercises the beacon, the query, the reply and the parsing together.
  //
  // Reported rather than asserted, and deliberately. A machine can legitimately
  // fail this while the code is perfect — a firewall with no inbound rule for this
  // binary drops the datagrams, which is itself one of the two things that breaks
  // LAN play and is worth SAYING rather than turning into a red build on a
  // developer's laptop.
  {
    net::Advertiser host;
    net::Listener guest;
    const bool advertising = host.start(25565, "Test World", "Alice");
    const bool listening = guest.start();
    check(advertising, "a host can open a discovery socket");
    checkf(listening, "and a guest can listen on port %d (%s)", static_cast<int>(net::kDiscoveryPort),
           guest.problem().empty() ? "ok" : guest.problem().c_str());

    bool sawIt = false;
    if (advertising && listening) {
      // Two seconds of pumping at 60 Hz, which covers a beacon period and a query
      // period several times over.
      for (int i = 0; i < 120 && !sawIt; ++i) {
        host.update(1.0 / 60.0);
        guest.update(1.0 / 60.0);
        for (const net::Beacon& b : guest.found()) {
          if (b.worldName == "Test World" && b.port == 25565) sawIt = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
      }
    }
    if (sawIt) {
      check(true, "and the guest lists the world the host is advertising");
    } else {
      std::printf("       [note] this machine did not hear its own beacon. That is the\n"
                  "              firewall or an isolated adapter, not a broken build --\n"
                  "              allow Hollowreach through for Private networks.\n");
    }
  }
}

// --- how far in the past a remote body is drawn --------------------------------
//
// The buffer a ghost holds used to be a flat 150 ms, inherited from the web build
// together with its 10 Hz snapshots. On a LAN, where the round trip is about a
// millisecond, that one constant was the largest source of visible lag in the
// game and nothing about it looked at the connection.
//
// It is now measured from the stream itself. These checks are about the two ways
// that can be wrong, and they pull in opposite directions: too small and the
// render time runs past the newest sample, so the ghost reaches the end of what
// it has and freezes until the next packet — a stutter, exactly when the link is
// already struggling. Too large and it is the old problem back again.
void testInterpDelay() {
  std::printf("how far behind a ghost is drawn\n");

  // Feeds a track at a steady rate and reports where it settles.
  const auto settle = [](double period, double wobble) {
    net::Ghosts::Track t;
    double now = 100.0;
    std::uint32_t state = 7u;
    for (int i = 0; i < 400; ++i) {
      state = state * 1664525u + 1013904223u;
      const double j = wobble * ((state >> 16) % 1000) / 1000.0;
      now += period + j;
      t.push(Vec3{static_cast<float>(i), 0, 0}, 0, 0, now);
    }
    return std::pair<net::Ghosts::Track, double>{t, now};
  };

  {
    auto [fast, now] = settle(0.050, 0.0);
    checkf(fast.delay() < 0.075, "a clean 20 Hz stream settles near its own gap (%.0f ms)",
           fast.delay() * 1000.0);
    checkf(fast.delay() >= net::kMinInterpDelay, "but never below the floor (%.0f ms)",
           fast.delay() * 1000.0);

    // The property that matters: the render time must land BETWEEN the two samples
    // held, not past the newest. Past it there is nothing to interpolate toward
    // and the body stops dead until the next packet lands.
    const double at = now - fast.delay();
    check(at <= fast.time, "and the drawn instant is not ahead of the newest sample");
    check(at >= fast.prevTime, "nor behind the older one, which would freeze it");
  }

  {
    auto [slow, unusedA] = settle(0.200, 0.0);
    auto [fast, unusedB] = settle(0.050, 0.0);
    checkf(slow.delay() > fast.delay() * 2.0,
           "a 5 Hz stream buffers far more than a 20 Hz one (%.0f vs %.0f ms)",
           slow.delay() * 1000.0, fast.delay() * 1000.0);
  }

  {
    // Same average rate, one of them erratic. The jittery one has to hold more, or
    // every longer-than-usual gap shows as a stutter.
    auto [steady, unusedC] = settle(0.050, 0.0);
    auto [jittery, unusedD] = settle(0.050, 0.060);
    checkf(jittery.delay() > steady.delay(),
           "and a jittery stream buffers more than a steady one at the same rate "
           "(%.0f vs %.0f ms)",
           jittery.delay() * 1000.0, steady.delay() * 1000.0);
    checkf(jittery.delay() <= net::kMaxInterpDelay, "without running past the ceiling (%.0f ms)",
           jittery.delay() * 1000.0);
  }

  // What this is all for, stated as the number that changed: on the rate the host
  // now sends at, a remote player is drawn a great deal closer to the present than
  // the old fixed 150 ms managed.
  {
    auto [lan, unusedE] = settle(0.050, 0.002);
    checkf(lan.delay() < 0.100, "so a LAN ghost lands well inside the old 150 ms (%.0f ms)",
           lan.delay() * 1000.0);
  }
}

// --- what arrives out of order, and what has not arrived at all ----------------
//
// Two failures that look nothing alike on the surface and are the same mistake
// underneath: treating "I have not been told" as "I have been told nothing".
//
// The fast channel is ENET_PACKET_FLAG_UNSEQUENCED, so packets arrive in whatever
// order the network hands them over. A ghost stamps every sample with its ARRIVAL
// time, so an old snapshot overtaking a new one was not ignored — it was taken as
// the newest thing known, and dragged everybody back to where they had been.
//
// And a roster entry arrives on the reliable channel while the first snapshot is
// still in flight on the fast one, so for a moment a player is known to exist and
// not known to be anywhere. Drawing that gave them a body at the default position,
// which is the world origin at bedrock: buried, at the spot most worlds call home.
void testGhostArrival() {
  std::printf("ghosts, and what has not arrived yet\n");

  // --- newer-than, across the wrap -------------------------------------------
  {
    check(net::newerSeq(2, 1), "a later sequence number is newer");
    check(!net::newerSeq(1, 2), "an earlier one is not");
    check(!net::newerSeq(5, 5), "and a repeat of the newest is not either");
    // The counter is 32 bits and a peer may send whatever it likes. A plain
    // greater-than would reject everything for the rest of the session the
    // instant it wrapped.
    check(net::newerSeq(1, 0xFFFFFFFFu), "one past the end of the range is newer, not older");
    check(!net::newerSeq(0xFFFFFFFFu, 1), "and the value it wrapped from is older, not newer");
  }

  // --- the number survives the wire ------------------------------------------
  {
    net::SnapshotMsg out;
    out.seq = 4242;
    out.time = 0.4f;
    net::SnapEntity e;
    e.id = 9;
    e.type = static_cast<std::uint8_t>(game::EntityType::Drop);
    e.pos = Vec3{1, 2, 3};
    e.a = 5;
    e.key = "greystone";
    out.entities.push_back(e);

    ByteWriter w;
    net::begin(w, net::MsgType::Snapshot);
    encode(w, out);
    ByteReader r(w.data().data(), w.data().size());
    r.skip(1);
    net::SnapshotMsg back;
    const bool ok = decode(r, back);
    check(ok && back.seq == 4242, "a snapshot carries its sequence number across the wire");
    check(ok && back.entities.size() == 1 && back.entities.front().key == "greystone",
          "and says what a dropped item is, not merely where");
  }

  // --- the snapshot budget can count ------------------------------------------
  //
  // Host::sendSnapshot fills a message up to a byte allowance so that it fits in
  // one datagram, and it works that allowance out from wireSize rather than by
  // encoding and measuring. That is only safe while the two agree exactly, and
  // nothing else in the game would ever notice them drifting apart: a field added
  // to the encoder without a matching change here makes the budget optimistic, the
  // snapshot goes back over the fragment threshold, and the symptom arrives half an
  // hour into somebody's session looking like a network fault.
  {
    net::SnapshotMsg m;
    m.time = 0.31f;
    m.seq = 77;
    // Deliberately varied: an empty key, a short one, and a long one, because the
    // length-prefixed string is the only part of the arithmetic that is not a
    // constant and so the only part that can be wrong by a different amount each
    // time.
    for (const char* key : {"", "coal", "greystone_stairs"}) {
      net::SnapEntity e;
      e.id = 3;
      e.type = static_cast<std::uint8_t>(game::EntityType::Drop);
      e.pos = Vec3{1, 2, 3};
      e.key = key;
      m.entities.push_back(std::move(e));
    }
    for (const char* id : {"phost0000001", "pguest000001"}) {
      net::SnapPlayer p;
      p.playerId = id;
      p.pos = Vec3{4, 5, 6};
      m.players.push_back(std::move(p));
    }

    std::size_t predicted = net::snapshotOverhead();
    for (const net::SnapEntity& e : m.entities) predicted += net::wireSize(e);
    for (const net::SnapPlayer& p : m.players) predicted += net::wireSize(p);

    ByteWriter w;
    net::begin(w, net::MsgType::Snapshot);
    encode(w, m);
    checkf(predicted == w.data().size(),
           "what a snapshot is budgeted at is what it actually encodes to (%zu vs %zu)",
           predicted, w.data().size());
  }

  // --- a player known about but not yet located -------------------------------
  {
    game::EntityManager entities;
    net::Ghosts ghosts;
    ghosts.attach(&entities);

    // The roster message, ahead of any snapshot. This is every join, every time.
    ghosts.addPlayer("pguest000001", "Bob");
    ghosts.update(100.0);
    int bodies = 0;
    for (const game::Entity& e : entities.all()) {
      if (e.ghost && !e.dead) ++bodies;
    }
    checkf(bodies == 0, "a player nobody has located yet is not drawn anywhere (%d bodies)",
           bodies);

    // And once a snapshot says where they are, they appear there.
    net::SnapshotMsg snap;
    net::SnapPlayer p;
    p.playerId = "pguest000001";
    p.pos = Vec3{120.0f, 70.0f, -40.0f};
    snap.players.push_back(p);
    ghosts.feedSnapshot(snap, 100.05);
    ghosts.update(100.10);

    const game::Entity* body = nullptr;
    for (const game::Entity& e : entities.all()) {
      if (e.ghost && e.type == game::EntityType::RemotePlayer && !e.dead) body = &e;
    }
    checkf(body != nullptr, "and appears as soon as one does");
    if (body) {
      const float off = std::fabs(body->pos.x - 120.0f) + std::fabs(body->pos.y - 70.0f);
      checkf(off < 0.01f, "at the place they actually are, not at the origin (%.1f %.1f)",
             body->pos.x, body->pos.y);
    }
  }
}

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

  // The host's own entity tick, run alongside the network pump below. Without it
  // nothing in the host's world ages, falls or is collected, and the half of
  // multiplayer that is about things rather than about blocks cannot be exercised
  // at all. `sharedWorld` because that is what this world is: see EntityContext.
  game::EntityContext hostCtx;
  hostCtx.world = hostWorld.get();
  hostCtx.player = &hostPlayer;
  hostCtx.inventory = &hostInventory;
  hostCtx.entities = &hostEntities;
  hostCtx.sky = &hostSky;
  hostCtx.sharedWorld = true;
  // An Input nobody is typing into, which is exactly what App gives this context
  // whenever a screen is open — and never nullptr, which is what this harness used
  // to give it. The difference is not cosmetic: the only thing that reads it is
  // the rideable boat, and leaving it null meant the boat's rider branch could not
  // run here at all. A test of who a boat carries that silently skipped the code
  // that carries anybody is a test that would have passed whatever the answer was.
  Input hostInput;
  hostCtx.input = &hostInput;

  // What App installs on a host: anything the world spills becomes a real item
  // lying in it. Needed here because a container broken by a guest is emptied by
  // the host, through this seam and nowhere else — without it the spill is a
  // silent no-op and a test of it proves nothing.
  hostWorld->setDropSink([&hostEntities](float x, float y, float z, const std::string& key,
                                         int count, int dura) {
    hostEntities.spawnDrop(Vec3{x, y, z}, key, count, dura);
  });

  // Something alive before anyone joins, so the payload has something to strip.
  hostEntities.spawn(game::EntityType::Pig,
                     Vec3{kOriginX + 1.0f, static_cast<float>(kY), kOriginZ + 1.0f});

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
    // Exactly what App::buildSave puts in one, because the question of what a
    // guest is sent is only interesting if the thing it is sent is real.
    data.entities = hostEntities.serialize();
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
  // How many entities were in the world the host sent. A guest owns none of them:
  // they belong to the host and arrive as ghosts in the snapshot stream.
  std::size_t payloadEntities = 999;

  net::Client client;
  net::SessionHooks guestHooks;
  guestHooks.notify = [](const std::string&) {};
  guestHooks.onDisconnected = [](const std::string&) {};
  // Why the host would not give us a container, if it refused one. App closes the
  // screen on this; here it is simply recorded, because "the host said nothing" is
  // the whole assertion for a container that is supposed to work.
  std::string lastDeny;
  guestHooks.onContainerDenied = [&lastDeny](int, int, int, const std::string& why) {
    lastDeny = why;
  };
  int mountDenials = 0;
  guestHooks.onMountDenied = [&mountDenials](int) { ++mountDenials; };
  guestHooks.adoptWorld = [&](const save::WorldSave& data) {
    payloadEntities = data.entities.size();
    guestWorld = std::make_unique<world::World>(data.meta.seed, 2, data.meta.genVersion);
    guestWorld->setEdits(data.edits);
    guestWorld->primeSpawn(kOriginX, kOriginZ);
    guestSky.time = data.meta.time;
    // App::startWorld does this for every world it opens, and a guest's world goes
    // through the same door. Reproduced here because the interesting question is
    // what happens to a guest that is HANDED a list of animals, and a harness that
    // quietly ignored the list could not ask it.
    guestEntities.load(data.entities);
    adopted = true;

    net::GameRefs refs;
    refs.world = guestWorld.get();
    refs.player = &guestPlayer;
    refs.inventory = &guestInventory;
    refs.entities = &guestEntities;
    refs.sky = &guestSky;
    client.attachGame(refs);
    // The sink App::adoptRemoteWorld installs, and the reason it is worth
    // duplicating here: a guest's world offers every write to the host for
    // approval. Leaving it out left the harness unable to reproduce anything that
    // goes wrong on the way back out of the world — which is most of what a guest
    // does — and a guest that only ever calls sendEdit directly is not the guest
    // the game runs.
    guestWorld->setEditSink([&client](int x, int y, int z, world::BlockId id, int meta) {
      client.sendEdit(x, y, z, static_cast<std::uint16_t>(id),
                      static_cast<std::uint8_t>(meta));
    });
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
  // The same pump with the host's world alive underneath it. Kept separate from
  // the plain one so the checks above, which are about messages rather than about
  // things, are not at the mercy of a wandering pig.
  const auto pumpLive = [&](int steps) {
    for (int i = 0; i < steps; ++i) {
      now += 0.02;
      hostEntities.tick(0.02f, hostCtx);
      host.update(0.02, now);
      client.update(0.02, now);
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

  // --- the payload carries the world, and nothing living ----------------------
  //
  // There is a pig in the host's world and the host's save has it. A guest must
  // not: it would build a full local copy, tick it with its own AI, and stand it
  // beside the ghost of the real animal for the rest of the session — a creature
  // only one player can see, that walks away on its own and cannot be killed,
  // because the thing being swung at was never the thing that existed.
  checkf(payloadEntities == 0,
         "the world a guest is sent contains no entities of its own (%zu)", payloadEntities);
  {
    int locals = 0;
    for (const game::Entity& e : guestEntities.all()) {
      if (!e.ghost && !e.dead) ++locals;
    }
    checkf(locals == 0, "so a guest's world holds nothing but ghosts (%d local)", locals);
  }

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

  // --- a picture hung on the host reaches the guest ---------------------------
  //
  // The pixels themselves cross the wire, which is the whole reason a painting
  // stores them instead of naming a file: the guest has never seen the host's
  // screenshots folder, so a filename would arrive meaning nothing.
  {
    const world::BlockId canvas = world::wk().canvas;
    hostWorld->setBlock(9, kY, 5, world::wk().greystone, 0);
    host.onLocalEdit(9, kY, 5, static_cast<std::uint16_t>(world::wk().greystone), 0);
    hostWorld->setBlock(8, kY, 5, canvas, 1);  // meta 1: the wall is at +x
    host.onLocalEdit(8, kY, 5, static_cast<std::uint16_t>(canvas), 1);
    pump(40);

    game::Painting art;
    art.rgb.resize(game::kPaintingBytes);
    for (std::size_t i = 0; i < art.rgb.size(); ++i) {
      art.rgb[i] = static_cast<std::uint8_t>((i * 7) & 0xFF);
    }
    hostWorld->setPainting(8, kY, 5, art);
    host.broadcastPainting(8, kY, 5, art);
    pump(60);

    const game::Painting* seen = guestWorld->painting(8, kY, 5);
    check(seen != nullptr, "a picture the host hangs arrives at the guest");
    if (seen) {
      check(seen->rgb == art.rgb, "with every pixel intact across the wire");
    }
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

    // And it is still accepted once the host has been busy. A guest applies what
    // the host sends it, and for a while each of those went back out again as a
    // request of its own: the host accepted it, relayed it to everyone, and round
    // it went. This does not watch the loop — it watches what the loop cost, which
    // was the host running out of patience with the guest and refusing the edits
    // the player had actually made.
    for (int i = 0; i < 80; ++i) {
      const int bx = 20 + (i % 8), bz = 20 + (i / 8);
      hostWorld->setBlock(bx, kY, bz, stone, 0);
      host.onLocalEdit(bx, kY, bz, static_cast<std::uint16_t>(stone), 0);
    }
    pump(60);
    client.sendEdit(6, kY, 7, static_cast<std::uint16_t>(stone), 0);
    pump(40);
    check(hostWorld->getBlock(6, kY, 7) == stone,
          "and a guest's edit still lands after the host has sent a burst of its own");
  }

  // --- the host's body arrives as a ghost -------------------------------------
  {
    hostPlayer.setPos(Vec3{kOriginX + 4.0f, static_cast<float>(kY), kOriginZ});
    // Facing +x, so the body's heading is something the check below can name.
    hostPlayer.setLook(kYawPlusX, 0.0f);
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

      // Every model faces +z; a player's yaw 0 looks down -z. The ghost carries
      // the model's convention, so it is half a turn from the pose that fed it —
      // and when it was not, two players facing each other saw one another's back.
      constexpr float kPi = 3.14159265358979f;
      float turned = body->yaw - (kYawPlusX + kPi);
      while (turned > kPi) turned -= 2.0f * kPi;
      while (turned < -kPi) turned += 2.0f * kPi;
      checkf(std::fabs(turned) < 0.01f,
             "and faces the way the host is facing, not away from it (%.3f rad off)",
             turned);
    }
    check(!client.ghosts().nameplates().empty() &&
              client.ghosts().nameplates().front().name == "Alice",
          "with a nameplate carrying the host's name");
  }

  // --- and the guest's body arrives back at the host ---------------------------
  //
  // The other direction, which nothing checked before and which is the one the
  // person hosting actually looks at. A guest that can see the host while the
  // host cannot see the guest passes every test above.
  {
    guestPlayer.setPos(Vec3{kOriginX - 4.0f, static_cast<float>(kY), kOriginZ});
    pump(60);
    const game::Entity* body = nullptr;
    for (const game::Entity& e : hostEntities.all()) {
      if (e.ghost && e.type == game::EntityType::RemotePlayer && !e.dead) body = &e;
    }
    checkf(body != nullptr, "the guest's player appears in the host's world as a ghost");
    if (body) {
      const float dx = std::fabs(body->pos.x - (kOriginX - 4.0f));
      checkf(dx < 4.0f, "and follows where the guest actually is (%.2f blocks behind)", dx);
    }
    check(!host.ghosts().nameplates().empty() &&
              host.ghosts().nameplates().front().name == "Bob",
          "with a nameplate carrying the guest's name");
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
    hostEntities.tick(0.02f, hostCtx);
    pump(40);
    cows = 0;
    for (const game::Entity& e : guestEntities.all()) {
      if (e.ghost && e.type == game::EntityType::Cow && !e.dead) ++cows;
    }
    check(cows == 0, "and vanishes from the guest when the host's copy dies");
  }

  // --- what a dropped item is, and who is allowed to have it -------------------
  //
  // A snapshot entity used to be a position, a type and two floats, which is the
  // whole of a sheep and not nearly the whole of a dropped item. A guest was told
  // where the thing was and how many of it there were, and never what it was, so
  // the renderer fell through to its last-resort grey cube: every ore, every tool,
  // every loaf in the world, the same anonymous block.
  //
  // And it could not be picked up. Drops the host owns are ghosts on a guest, and
  // a ghost is never ticked, so the code that collects one never ran on the side
  // that was standing on it. That included a guest's own death scatter.
  {
    // Something for it to land on. makeWorld carves its pocket out of open air a
    // long way above the terrain — which is exactly what the other checks want,
    // and means anything with gravity in it falls out of the bottom.
    for (int x = 6; x <= 11; ++x) {
      for (int z = 6; z <= 11; ++z) {
        hostWorld->setBlock(x, kY - 3, z, world::wk().greystone, 0);
      }
    }
    const float floorY = static_cast<float>(kY) - 2.0f;
    hostPlayer.setPos(Vec3{kOriginX + 6.0f, floorY, kOriginZ});
    guestPlayer.setPos(Vec3{kOriginX + 4.0f, floorY, kOriginZ});
    pumpLive(40);

    hostEntities.spawnDrop(Vec3{kOriginX, static_cast<float>(kY), kOriginZ}, "greystone", 3, -1);
    pumpLive(90);

    const game::Entity* ghost = nullptr;
    for (const game::Entity& e : guestEntities.all()) {
      if (e.ghost && e.type == game::EntityType::Drop && !e.dead) ghost = &e;
    }
    checkf(ghost != nullptr, "a dropped item the host owns is mirrored to the guest");
    if (ghost) {
      checkf(ghost->data.key == "greystone",
             "and the guest is told what it is, not only where (\"%s\")",
             ghost->data.key.c_str());
      checkf(ghost->data.count == 3, "and how many of it there are (%d)", ghost->data.count);
    }
    // Six blocks away and it has stayed put. An instant drop is vacuumed with no
    // distance check at all in a world of one, and that quietly became "the host
    // harvests everything anyone mines, from anywhere" the moment a guest arrived.
    checkf(hostInventory.countOf("greystone") == 0,
           "and the host has not taken it from across the world (%d)",
           hostInventory.countOf("greystone"));

    const int before = guestInventory.countOf("greystone");
    guestPlayer.setPos(Vec3{kOriginX, floorY, kOriginZ});
    pumpLive(90);
    checkf(guestInventory.countOf("greystone") == before + 3,
           "a guest standing on a drop picks it up (%d -> %d)", before,
           guestInventory.countOf("greystone"));
    int leftover = 0;
    for (const game::Entity& e : hostEntities.all()) {
      if (!e.dead && e.type == game::EntityType::Drop) ++leftover;
    }
    checkf(leftover == 0, "and it leaves the host's world with them (%d still there)", leftover);
  }

  // --- a container a guest puts down is a real container -----------------------
  //
  // Interact::tryPlace creates a station's block entity the moment the block
  // exists, because a forge should smelt and a chest should hold things whether or
  // not anyone has opened them. Nothing did that for a block that arrived over the
  // wire, so a chest a guest placed was a block on the host and a container
  // nowhere. Every consequence pointed somewhere else: the request was refused
  // with "Nothing there", the refusal meant no lock, no lock meant the contents
  // the guest sent on closing were discarded without a word, and breaking it
  // spilled nothing because there was nothing to spill. Meanwhile the guest's own
  // screen worked perfectly the whole time — it was reading the copy in its own
  // world — which is why this presented as a chest that worked and then stopped.
  {
    const world::BlockId chest = world::blocks().idOf("chest");
    const float floorY = static_cast<float>(kY) - 2.0f;
    const int cx = 9, cy = kY - 2, cz = 9;
    guestPlayer.setPos(Vec3{kOriginX + 4.0f, floorY, kOriginZ + 4.0f});
    pump(30);

    // Exactly what tryPlace does on the guest: write the block, which the edit
    // sink offers to the host, and make the container locally.
    guestWorld->setBlock(cx, cy, cz, chest, 0);
    guestWorld->getOrCreateBlockEntity(cx, cy, cz, game::BlockEntityKind::Chest);
    pump(40);

    check(hostWorld->getBlock(cx, cy, cz) == chest, "a chest a guest places reaches the host");
    game::BlockEntity* hostBe = hostWorld->getBlockEntity(cx, cy, cz);
    checkf(hostBe != nullptr, "and is a container there, not merely a block");

    lastDeny.clear();
    client.sendBlockEntityRequest(cx, cy, cz, static_cast<std::uint8_t>(game::BlockEntityKind::Chest));
    pump(40);
    checkf(lastDeny.empty(), "so opening it is not refused (\"%s\")", lastDeny.c_str());

    // Put something in, and close it the way App does — one message carrying the
    // result of the whole rummage, with `final` to release the lock.
    if (game::BlockEntity* mine = guestWorld->getBlockEntity(cx, cy, cz)) {
      mine->slots[0] = game::ItemStack{"greystone", 12, -1};
      net::BeStateMsg state;
      state.x = cx;
      state.y = cy;
      state.z = cz;
      state.kind = static_cast<std::uint8_t>(game::BlockEntityKind::Chest);
      for (const game::ItemStack& s : mine->slots) {
        net::WireSlot out;
        if (!s.empty()) {
          out.key = s.key;
          out.count = s.count;
          out.dura = s.dura;
        }
        state.slots.push_back(std::move(out));
      }
      state.final = true;
      client.sendBlockEntityState(state);
    }
    pump(40);
    hostBe = hostWorld->getBlockEntity(cx, cy, cz);
    const bool stored = hostBe != nullptr && !hostBe->slots.empty() &&
                        hostBe->slots[0].key == "greystone" && hostBe->slots[0].count == 12;
    check(stored, "and what the guest puts in it is held by the host, not by the guest");

    // Break it. The host is the only one that can spill it, because the host is
    // the only one that has it.
    const int stoneBefore = hostInventory.countOf("greystone");
    (void)stoneBefore;
    guestWorld->setBlock(cx, cy, cz, world::kAir, 0);
    pump(40);
    int spilled = 0;
    for (const game::Entity& e : hostEntities.all()) {
      if (!e.dead && e.type == game::EntityType::Drop && e.data.key == "greystone" &&
          e.data.count == 12) {
        ++spilled;
      }
    }
    checkf(spilled == 1, "and breaking it drops what was inside rather than losing it (%d)",
           spilled);
    checkf(hostWorld->getBlockEntity(cx, cy, cz) == nullptr,
           "with nothing left behind at that position");

    // A chest already standing in the world with nothing behind it, which is what
    // every save written by an older build holds: the block was relayed and
    // stored, the container never was. Reloading could not fix it and neither
    // could rejoining — it is in the file — so opening one has to be what makes it
    // real, or those worlds stay broken forever.
    {
      const int ox = 7, oy = kY - 2, oz = 10;
      hostWorld->applyRemoteEdit(ox, oy, oz, chest, 0);
      hostWorld->removeBlockEntity(ox, oy, oz);  // the state an old save is in
      guestPlayer.setPos(Vec3{static_cast<float>(ox), floorY, static_cast<float>(oz)});
      pump(30);
      lastDeny.clear();
      client.sendBlockEntityRequest(ox, oy, oz,
                                    static_cast<std::uint8_t>(game::BlockEntityKind::Chest));
      pump(40);
      checkf(lastDeny.empty(), "a chest left containerless by an older build opens (\"%s\")",
             lastDeny.c_str());
      checkf(hostWorld->getBlockEntity(ox, oy, oz) != nullptr,
             "and is a real container from then on");
    }

    // And the other direction, which is where the contents were actually going
    // missing. A guest learns that a station was destroyed only as a relayed edit,
    // and there is no path on that side that cleans up after one — so the
    // container stayed in the guest's world at that position, invisible, holding
    // what it held, and handed it all to the next block placed there.
    const int hx = 11, hy = kY - 2, hz = 9;
    hostWorld->setBlock(hx, hy, hz, chest, 0);
    hostWorld->getOrCreateBlockEntity(hx, hy, hz, game::BlockEntityKind::Chest);
    host.onLocalEdit(hx, hy, hz, static_cast<std::uint16_t>(chest), 0);
    pump(40);
    // What the guest would be holding after opening it once.
    if (game::BlockEntity* seen =
            guestWorld->getOrCreateBlockEntity(hx, hy, hz, game::BlockEntityKind::Chest)) {
      seen->slots[0] = game::ItemStack{"coal", 5, -1};
    }
    hostWorld->setBlock(hx, hy, hz, world::kAir, 0);
    host.onLocalEdit(hx, hy, hz, static_cast<std::uint16_t>(world::kAir), 0);
    pump(40);
    checkf(guestWorld->getBlockEntity(hx, hy, hz) == nullptr,
           "and a container the host destroys does not linger in the guest's world");

    // Swept up before the sections below walk past it. It is a real item lying on
    // a real floor now, which is the point of the check — and which means the
    // guest would collect it the moment the boat test moves them over it, and the
    // inventory the save check counts would be twelve stone heavier than the
    // arithmetic there says.
    for (game::Entity& e : hostEntities.all()) {
      if (!e.dead && e.type == game::EntityType::Drop) e.dead = true;
    }
    hostEntities.tick(0.02f, hostCtx);
    pump(20);
  }

  // --- a boat a guest is sitting in --------------------------------------------
  //
  // Two faults, and the first hid the second. App asked to mount by the entity's
  // LOCAL id, and on a guest every entity is a ghost whose id is -netId - 1 — so
  // the host looked up a negative id, found nothing, and refused every mount
  // anybody ever asked for. Behind that, a boat is steered from ctx.input and
  // seats ctx.player, both of which mean "whoever is playing on THIS machine", so
  // a mount that did succeed would have dragged the host into the boat.
  {
    const float floorY = static_cast<float>(kY) - 2.0f;
    guestPlayer.setPos(Vec3{kOriginX + 1.0f, floorY, kOriginZ});
    hostPlayer.setPos(Vec3{kOriginX + 9.0f, floorY, kOriginZ});
    pumpLive(30);

    game::Entity* boat = hostEntities.spawn(game::EntityType::Boat,
                                            Vec3{kOriginX + 1.0f, floorY, kOriginZ});
    const int boatId = boat ? boat->id : 0;
    checkf(boatId > 0, "the host has a boat to be asked about");
    pumpLive(30);

    // The old call, exactly: a ghost's id rather than the host's. It is worth
    // asking for explicitly, because the check below is only meaningful if this
    // one is genuinely refused.
    mountDenials = 0;
    client.sendBoatMount(-boatId - 1, true);
    pumpLive(30);
    checkf(mountDenials == 1, "a mount asked for by the ghost's own id is refused (%d)",
           mountDenials);

    // Where the host is standing BEFORE anyone asks for the boat. Taken here and
    // not after, because what is being watched for happens on the very first tick
    // of a successful mount: measuring from a position already inside the boat
    // would compare the wrong thing against itself and pass either way.
    const Vec3 stood = hostPlayer.pos();

    mountDenials = 0;
    client.sendBoatMount(boatId, true);
    pumpLive(30);
    boat = hostEntities.byId(boatId);
    checkf(mountDenials == 0 && boat != nullptr && boat->data.rider,
           "and the host's id for the same boat is accepted");

    // The host is standing eight blocks away and stays there. Without the
    // remoteRider guard the boat's update hook reads the host's keyboard and puts
    // the host's body in the seat, so the person who was not in the boat is the
    // one who ends up in it.
    pumpLive(40);
    const Vec3 after = hostPlayer.pos();
    const float moved = std::fabs(after.x - stood.x) + std::fabs(after.y - stood.y) +
                        std::fabs(after.z - stood.z);
    checkf(moved < 0.5f, "while the host, who is not in it, is left where they stood (%.1f)",
           moved);

    // The rider owns the hull: it goes where they go, and the host learns that
    // from the pose they already send rather than from a message of its own.
    guestPlayer.setPos(Vec3{kOriginX + 5.0f, floorY, kOriginZ + 2.0f});
    pumpLive(40);
    boat = hostEntities.byId(boatId);
    const float lag = boat ? std::fabs(boat->pos.x - (kOriginX + 5.0f)) : 99.0f;
    checkf(lag < 0.5f, "and the boat follows the guest who is steering it (x %.1f)",
           boat ? boat->pos.x : 0.0f);

    client.sendBoatMount(boatId, false);
    pumpLive(30);
    boat = hostEntities.byId(boatId);
    checkf(boat != nullptr && !boat->data.rider,
           "standing up hands it back to the world");
    if (boat) boat->dead = true;
    pumpLive(20);
  }

  // --- a busy world does not crowd out what is next to you ----------------------
  //
  // The snapshot was a census of everything, capped at 512 and filled in the order
  // the entity vector happened to hold. That order is oldest first, and a drop in
  // an unloaded chunk never ages — the tick skips it before `age += dt` — so a
  // long session accumulated a queue of frozen items that filled the allowance
  // before it ever reached the mob standing beside the player. A guest treats
  // anything a snapshot does not mention as gone, so those mobs did not merely
  // stop updating: they were deleted, over and over, as fast as they appeared.
  {
    const float floorY = static_cast<float>(kY) - 2.0f;
    guestPlayer.setPos(Vec3{kOriginX, floorY, kOriginZ});
    pump(30);

    // Six hundred of them, comfortably past the old 512 and all of them older
    // than the animal below. Far enough away to be irrelevant, near enough to be
    // inside the range filter, so this tests the allowance rather than the radius.
    for (int i = 0; i < 600; ++i) {
      hostEntities.spawnDrop(Vec3{kOriginX + 40.0f + static_cast<float>(i % 8),
                                  static_cast<float>(kY),
                                  kOriginZ + 40.0f + static_cast<float>(i / 8)},
                             "coal", 1, -1);
    }
    game::Entity* neighbour = hostEntities.spawn(
        game::EntityType::Sheep, Vec3{kOriginX + 2.0f, floorY, kOriginZ});
    const int sheepId = neighbour ? neighbour->id : 0;
    pump(60);

    int sheep = 0;
    std::size_t arrived = net::snapshotOverhead();
    for (const game::Entity& e : guestEntities.all()) {
      if (!e.ghost || e.dead) continue;
      if (e.type == game::EntityType::RemotePlayer) {
        net::SnapPlayer p;
        p.playerId = "phost0000001";
        arrived += net::wireSize(p);
        continue;
      }
      net::SnapEntity out;
      out.key = e.data.key;
      arrived += net::wireSize(out);
      if (e.type == game::EntityType::Sheep) ++sheep;
    }
    checkf(sheep == 1, "an animal at arm's length survives six hundred distant drops (%d)",
           sheep);

    // And what arrived fits in one datagram, which is the whole point. ENet
    // fragments a packet above mtu - sizeof(ENetProtocolHeader) -
    // sizeof(ENetProtocolSendFragment) — 1364 bytes at the default MTU — and
    // fragmenting on this channel used to mean far worse than fragmenting: the
    // packet was silently promoted to reliable, ordered and retransmitted, twenty
    // times a second, until the reliable window stalled and never recovered. Six
    // hundred entities is about 20 KB, so this is a world that would have wedged.
    constexpr std::size_t kEnetFragmentThreshold = 1392 - 4 - 24;
    checkf(arrived <= kEnetFragmentThreshold,
           "and what it was told fits in one datagram (%zu bytes, limit %zu)", arrived,
           kEnetFragmentThreshold);

    // Tidy up, so the sections after this are not looking at a world full of coal.
    for (game::Entity& e : hostEntities.all()) {
      if (e.type == game::EntityType::Drop || e.id == sheepId) e.dead = true;
    }
    hostEntities.tick(0.02f, hostCtx);
    pump(40);
  }

  // --- waking up somewhere else ------------------------------------------------
  //
  // A respawn moves a body further in one step than any speed allows, and the
  // host's movement check is right to notice. What it did about it was teleport
  // the guest back to where the pose before the jump had them — which, for a
  // respawn, is the spot they had just died on. Anyone who died more than about
  // twelve blocks from their spawn woke up standing in their own dropped things.
  //
  // The two halves of this check are each other's control: the same leap, once
  // unannounced and once announced, and the difference is the whole fix.
  {
    const float floorY = static_cast<float>(kY) - 2.0f;
    const Vec3 home{kOriginX, floorY, kOriginZ};
    const Vec3 away{kOriginX + 200.0f, floorY, kOriginZ + 200.0f};

    guestPlayer.setPos(home);
    pump(40);
    guestPlayer.setPos(away);
    pump(60);
    const float dragged = std::fabs(guestPlayer.pos().x - away.x);
    checkf(dragged > 1.0f, "an unannounced leap across the map is put back (%.0f blocks)",
           dragged);

    guestPlayer.setPos(home);
    pump(40);
    client.sendWarp();
    guestPlayer.setPos(away);
    // Short enough to stay inside net::kMoveGrace, which is measured in seconds of
    // the same clock this pump advances by 20 ms a step.
    pump(20);
    checkf(std::fabs(guestPlayer.pos().x - away.x) < 0.01f,
           "and the same leap, announced first, is left alone (x %.1f)", guestPlayer.pos().x);

    // A second one, still inside the window the warning opened. The exemption
    // cannot be a single free pose: the warning goes reliably on channel 0 and
    // the poses that need it go unsequenced on channel 1, and the two channels do
    // not wait for each other — so the pose the exemption was meant for is not
    // reliably the first one to arrive after it.
    const Vec3 further{away.x + 60.0f, floorY, away.z + 60.0f};
    guestPlayer.setPos(further);
    pump(20);
    checkf(std::fabs(guestPlayer.pos().x - further.x) < 0.01f,
           "and so is the next one, while the warning still stands (x %.1f)",
           guestPlayer.pos().x);

    guestPlayer.setPos(home);
    pump(40);
  }

  // Bound now, checked after the goodbye below carries the last state across. The
  // fields have been on the wire since the first multiplayer build and nothing
  // ever filled them in, so the host stored "no spawn bound" for every guest it
  // had ever met: a Soul Anchor lasted exactly until you left the world.
  const Vec3 guestAnchor{kOriginX + 3.0f, static_cast<float>(kY), kOriginZ + 3.0f};
  client.setSpawn(true, guestAnchor);

  // --- sleep is a vote on somebody's hour --------------------------------------
  {
    // The proposer has to have earned it. Nobody in this session has been awake
    // for anything, so the first attempt is refused outright.
    hostSky.setHoursAwake(0.0f);
    host.onLocalSleep(true, 0.5f);
    pump(10);
    checkf(host.proposedSleep() < 0.0f, "a host who is not tired cannot open the question (%.2f)",
           host.proposedSleep());

    hostSky.setHoursAwake(render::Sky::kRestedHours);
    const float before = hostSky.time;
    host.onLocalSleep(true, 0.5f);
    pump(10);
    check(hostSky.time == before, "one of two players in bed does not skip the night");
    checkf(std::fabs(host.proposedSleep() - 0.5f) < 0.001f,
           "the proposal is on the table at the hour they asked for (%.3f)",
           host.proposedSleep());
    checkf(std::fabs(client.proposedSleep() - 0.5f) < 0.001f,
           "and the guest is told about it (%.3f)", client.proposedSleep());

    // The guest is NOT tired, and that is deliberately not a problem: only the
    // person who opens the question has to be.
    guestSky.setHoursAwake(0.0f);
    client.sendSleep(true, 0.9f);
    pump(30);
    check(hostSky.isSleeping(), "and an untired second vote still carries it");
    checkf(std::fabs(hostSky.sleepTarget() - 0.5f) < 0.001f,
           "at the proposer's hour, not the voter's (%.3f)", hostSky.sleepTarget());

    // Run the sweep out and everyone is rested again.
    for (int i = 0; i < 400 && hostSky.isSleeping(); ++i) hostSky.update(1.0f / 60.0f);
    checkf(hostSky.hoursAwake() == 0.0f, "waking resets the clock (%.2f)",
           hostSky.hoursAwake());
    check(host.proposedSleep() < 0.0f, "and the question is closed again");
  }

  // --- guest progress survives the host's save ---------------------------------
  {
    // 17 handed straight into the bag, on top of the 3 they picked up off the
    // floor earlier in this session.
    constexpr int kGuestStone = 17 + 3;
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
                after.front().inventory.countOf("greystone") == kGuestStone,
            "with the inventory they left with");
      checkf(after.front().hasSpawn &&
                 std::fabs(after.front().spawn.x - guestAnchor.x) < 0.01f,
             "and the Soul Anchor they had bound (%s, x %.1f)",
             after.front().hasSpawn ? "bound" : "unbound", after.front().spawn.x);

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
                back.guests.front().inventory.countOf("greystone") == kGuestStone,
            "and it survives a save and a reload");
    }
  }

  // --- leaving ----------------------------------------------------------------
  checkf(host.guestCount() == 0, "the host drops a guest that says goodbye (%d left)",
         host.guestCount());
  host.stop();
}

// The seam that stops a guest and its host arguing about the same block. A guest's
// world carries a sink that offers everything written to it to the host for
// approval, so the host's own edits have to arrive through a door that does not
// ring the bell — otherwise they go straight back where they came from, the host
// accepts and relays them, and the two ends spend the session telling each other
// about a block neither of them changed.
void testRemoteEditSink() {
  std::printf("remote edits do not echo\n");
  auto world = makeWorld();
  const world::BlockId stone = world::wk().greystone;
  int offered = 0;
  world->setEditSink([&offered](int, int, int, world::BlockId, int) { ++offered; });

  world->setBlock(4, kY, 4, stone, 0);
  checkf(offered == 1, "an edit made here is offered to the sink (%d)", offered);

  world->applyRemoteEdit(5, kY, 5, stone, 0);
  checkf(offered == 1, "one that came from the network is not sent back out (%d)", offered);
  check(world->getBlock(5, kY, 5) == stone, "and is applied to the world all the same");

  // Writing the value a cell already holds still reaches the sink. That is what
  // left the echo nothing to settle on: every lap round the loop looked exactly
  // as new as the first one.
  world->setBlock(4, kY, 4, stone, 0);
  checkf(offered == 2, "a local write that changes nothing still counts (%d)", offered);
}

// A world too big to fit in one ordinary message. The session test above shares a
// world a few edits deep, which encodes to a few hundred bytes — so it could never
// have caught the guest silently dropping anything larger, and a world about a
// minute old already is. Everything here exists to make the payload realistic in
// the one dimension that matters: its size.
void testNetBigWorld() {
  std::printf("multiplayer: a world larger than one message\n");

  auto hostWorld = makeWorld();
  game::Player hostPlayer(kOriginX, kY, kOriginZ);
  game::Inventory hostInventory;
  game::EntityManager hostEntities;
  render::Sky hostSky;
  hostSky.time = 0.55f;

  // A slab of edits, well above the pocket floor so nothing else is standing
  // there. The only thing being bought is bytes.
  const world::BlockId fill = world::wk().greystone;
  for (int y = kY + 6; y < kY + 18; ++y) {
    for (int x = 0; x < 40; ++x) {
      for (int z = 0; z < 40; ++z) hostWorld->setBlock(x, y, z, fill, 0);
    }
  }

  net::GameRefs hostRefs;
  hostRefs.world = hostWorld.get();
  hostRefs.player = &hostPlayer;
  hostRefs.inventory = &hostInventory;
  hostRefs.entities = &hostEntities;
  hostRefs.sky = &hostSky;

  net::SessionHooks hostHooks;
  hostHooks.buildSave = [&] {
    save::WorldSave data;
    data.meta.id = "wbigworld00";
    data.meta.name = "Big";
    data.meta.seed = hostWorld->seed();
    data.meta.genVersion = hostWorld->genVersion();
    data.meta.time = hostSky.time;
    data.edits = hostWorld->edits();
    data.player = hostPlayer.state();
    return data;
  };
  hostHooks.notify = [](const std::string&) {};

  // Measured rather than assumed: if the save format ever gets dense enough that
  // this payload slips back under the cap, the test is no longer covering the
  // thing it was written for and should say so here rather than passing quietly.
  const std::vector<std::uint8_t> payload = save::encode(hostHooks.buildSave());
  checkf(payload.size() > net::kMaxMessage,
         "the shared world is larger than one ordinary message (%zu bytes, cap %zu)",
         payload.size(), net::kMaxMessage);

  net::Host host;
  std::string error;
  if (!host.start(0, "phostbig0001", "Alice", hostRefs, hostHooks, &error)) {
    checkf(false, "the host binds a port (%s)", error.c_str());
    return;
  }

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
    guestWorld->setEditSink([&client](int x, int y, int z, world::BlockId id, int meta) {
      client.sendEdit(x, y, z, static_cast<std::uint16_t>(id),
                      static_cast<std::uint8_t>(meta));
    });
  };

  if (!client.start("127.0.0.1", host.port(), "pguestbig001", "Bob", guestHooks, &error)) {
    checkf(false, "the guest connects (%s)", error.c_str());
    host.stop();
    return;
  }

  double now = 0.0;
  for (int i = 0; i < 250; ++i) {
    now += 0.02;
    host.update(0.02, now);
    client.update(0.02, now);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (adopted && client.state() == net::Client::State::Playing) break;
  }

  checkf(adopted && client.state() == net::Client::State::Playing,
         "a guest receives a world that does not fit in one message");
  if (adopted && guestWorld) {
    check(guestWorld->getBlock(20, kY + 10, 20) == fill,
          "and the far side of the payload arrived intact");
  }

  client.stop(false);
  host.stop();
}

// Two guests in one world. What one guest knows about another arrives only through
// the host's snapshot, which is a different path from the poses the host itself
// draws from — so the host can be watching both of them behave perfectly while the
// two of them see each other standing frozen, facing north. With one guest nothing
// ever travels that path, which is why it took a third person to notice.
void testNetGuestToGuest() {
  std::printf("multiplayer: two guests\n");

  auto hostWorld = makeWorld();
  game::Player hostPlayer(kOriginX, static_cast<float>(kY), kOriginZ);
  game::Inventory hostInventory;
  game::EntityManager hostEntities;
  render::Sky hostSky;

  net::GameRefs hostRefs;
  hostRefs.world = hostWorld.get();
  hostRefs.player = &hostPlayer;
  hostRefs.inventory = &hostInventory;
  hostRefs.entities = &hostEntities;
  hostRefs.sky = &hostSky;

  net::SessionHooks hostHooks;
  hostHooks.notify = [](const std::string&) {};
  hostHooks.buildSave = [&] {
    save::WorldSave data;
    data.meta.id = "wtwoguests0";
    data.meta.seed = hostWorld->seed();
    data.meta.genVersion = hostWorld->genVersion();
    data.edits = hostWorld->edits();
    data.player = hostPlayer.state();
    return data;
  };

  net::Host host;
  std::string error;
  if (!host.start(0, "phost0000002", "Alice", hostRefs, hostHooks, &error)) {
    checkf(false, "the host binds a port (%s)", error.c_str());
    return;
  }

  struct Guest {
    std::unique_ptr<world::World> world;
    game::Player player{kOriginX, static_cast<float>(kY), kOriginZ};
    game::Inventory inventory;
    game::EntityManager entities;
    render::Sky sky;
    net::Client client;
    bool adopted = false;
  };
  Guest bob, carol;

  const auto join = [&](Guest& g, const char* id, const char* name) {
    net::SessionHooks hooks;
    hooks.notify = [](const std::string&) {};
    hooks.onDisconnected = [](const std::string&) {};
    hooks.adoptWorld = [&g](const save::WorldSave& data) {
      g.world = std::make_unique<world::World>(data.meta.seed, 2, data.meta.genVersion);
      g.world->setEdits(data.edits);
      g.world->primeSpawn(kOriginX, kOriginZ);
      g.adopted = true;
      net::GameRefs refs;
      refs.world = g.world.get();
      refs.player = &g.player;
      refs.inventory = &g.inventory;
      refs.entities = &g.entities;
      refs.sky = &g.sky;
      g.client.attachGame(refs);
    };
    std::string why;
    return g.client.start("127.0.0.1", host.port(), id, name, hooks, &why);
  };

  if (!join(bob, "pguestbob001", "Bob") || !join(carol, "pguestcar002", "Carol")) {
    check(false, "two guests connect to the same host");
    host.stop();
    return;
  }

  double now = 0.0;
  const auto pump = [&](int steps) {
    for (int i = 0; i < steps; ++i) {
      now += 0.02;
      host.update(0.02, now);
      bob.client.update(0.02, now);
      carol.client.update(0.02, now);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  };

  pump(200);
  checkf(bob.adopted && carol.adopted && host.guestCount() == 2,
         "two guests join one world (%d in the roster)", host.guestCount());

  // They stand on opposite sides of the host, and Carol faces +x.
  bob.player.setPos(Vec3{kOriginX + 5.0f, static_cast<float>(kY), kOriginZ});
  carol.player.setPos(Vec3{kOriginX - 5.0f, static_cast<float>(kY), kOriginZ});
  carol.player.setLook(kYawPlusX, 0.0f);
  pump(120);

  // In Bob's world the host stands at the origin and Carol five blocks past it.
  const game::Entity* seen = nullptr;
  for (const game::Entity& e : bob.entities.all()) {
    if (e.ghost && e.type == game::EntityType::RemotePlayer && !e.dead &&
        e.pos.x < kOriginX - 1.0f) {
      seen = &e;
    }
  }
  checkf(seen != nullptr, "one guest can see the other's body");
  if (seen) {
    constexpr float kPi = 3.14159265358979f;
    float turned = seen->yaw - (kYawPlusX + kPi);
    while (turned > kPi) turned -= 2.0f * kPi;
    while (turned < -kPi) turned += 2.0f * kPi;
    checkf(std::fabs(turned) < 0.2f,
           "and which way they are facing, which only the snapshot carries (%.3f rad off)",
           turned);
  }

  bob.client.stop(false);
  carol.client.stop(false);
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
  // Expanded flat before hashing, deliberately. The hash has to describe the
  // CONTENTS of a chunk and nothing else: whether a band happens to be stored
  // uniform or dense is a storage decision that can legitimately differ between
  // two worlds holding identical cells, and hashing the representation would make
  // this fingerprint fire on that.
  std::vector<world::BlockId> voxels(world::kCellsPerChunk);
  std::vector<std::uint8_t> bytes(world::kCellsPerChunk);
  for (const world::ChunkKey key : keys) {
    mix(&key, sizeof key);
    const world::ChunkData& d = *w.chunkAt(world::keyCx(key), world::keyCz(key))->chunk.data;
    d.voxels.copyTo(voxels.data());
    mix(voxels.data(), voxels.size() * sizeof(world::BlockId));
    for (const world::Banded<std::uint8_t>* array : {&d.meta, &d.skylight, &d.blocklight}) {
      array->copyTo(bytes.data());
      mix(bytes.data(), bytes.size());
    }
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
      check(held->voxels.get(world::localIdx(6, kY, 6)) == world::kAir,
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

    // Repeated, because the interesting failures here are races and a race that
    // shows up once in twenty-five runs will pass a single comparison all day.
    // One did: a light install skipped a neighbour whose own first light job was
    // still out, so that job installed values computed from a snapshot taken
    // before the install and nothing ever corrected them. Inline jobs complete at
    // submit and cannot be in flight, which is why only the threaded builds
    // disagreed, and why one sample of each was not enough to see it.
    int mismatches = 0;
    for (int i = 0; i < 12; ++i) {
      const WorldFingerprintTimed again = buildWorldWith(8);
      if (again.print.hash != inline0.print.hash ||
          again.print.chunks != inline0.print.chunks) {
        ++mismatches;
      }
    }
    jobs::system().stop();
    checkf(mismatches == 0, "and does so every time over twelve more builds (%d differed)",
           mismatches);
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
  testSettingScope();
  testDropping();
  testAutoStep();
  testCrouch();
  testSurvival();
  testLayout();
  testEntities();
  testSaves();
  testWorldgenDepth();
  testViewmodelSwing();
  testSpawnChoice();
  testPaintings();
  testRecipeConvenience();
  testWater();
  testChunkStorage();
  testLighting();
  testBulkEdits();
  testSleep();
  testBlockSupport();
  testCaveWater();
  testConfirmPrompt();
  testCreative();
  testLoot();
  testDungeons();
  testWorldUpgrade();
  testNet();
  testRemoteEditSink();
  testNetInterfaces();
  testInterpDelay();
  testGhostArrival();
  testNetSession();
  testNetBigWorld();
  testNetGuestToGuest();
  testThreading();
  testAudio();
  std::printf("\n%d checks, %d failure(s)\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}

}  // namespace hr::dev
