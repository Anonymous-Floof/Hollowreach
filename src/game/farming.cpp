#include "game/farming.h"

namespace hr::game {
namespace {

// Deliberately the plainest crops in the game. Wild seed is the bootstrap for a
// player who has not found a stand yet, so it has to be able to start a farm — and
// it must not be a shortcut to the ones worth travelling for.
const char* const kWildSeedCrops[] = {"wheat", "barley", "carrot", "onion", "potato"};

}  // namespace

const char* const* wildSeedCrops(int& count) {
  count = static_cast<int>(sizeof kWildSeedCrops / sizeof kWildSeedCrops[0]);
  return kWildSeedCrops;
}

FarmPlan planFarmUse(const ItemDef& item, world::BlockId target, bool clearAbove,
                     int wildSeedPick) {
  const world::BlockRegistry& reg = world::blocks();
  const world::WellKnownBlocks& w = world::wk();
  FarmPlan plan;

  if (item.toolType == ToolKind::Hoe) {
    if ((target == w.turf || target == w.loam) && clearAbove) plan.action = FarmAction::Till;
    return plan;
  }

  // The produce IS the seed, so sowing is keyed off whatever the crop drops.
  world::BlockId crop = reg.cropForProduce(item.key);
  if (crop == 0 && item.key == "wild_seeds") {
    int n = 0;
    const char* const* list = wildSeedCrops(n);
    const int i = n > 0 ? ((wildSeedPick % n) + n) % n : 0;
    crop = n > 0 ? reg.cropForProduce(list[i]) : 0;
  }
  if (crop == 0) return plan;  // not a seed at all

  if (target != w.farmland) {
    // Only worth a message when they were plainly trying to farm. Saying "till the
    // soil first" every time somebody right-clicks holding a carrot would be noise.
    if (target == w.turf || target == w.loam) plan.action = FarmAction::NeedsTilling;
    return plan;
  }
  if (!clearAbove) return plan;

  plan.action = FarmAction::Sow;
  plan.crop = crop;
  return plan;
}

}  // namespace hr::game
