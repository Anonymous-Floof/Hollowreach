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

  // Fertiliser enriches soil that has already been tilled. Only plain farmland, so
  // a second dose is refused rather than silently eaten — the tile is already rich
  // and there is nothing more to buy.
  if (item.key == "fertiliser") {
    // Tilled but not already enriched — either dampness, since the wet block is the
    // same soil with a different face.
    if (world::isFarmland(target) && !world::isRichFarmland(target)) {
      plan.action = FarmAction::Enrich;
    }
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

  // Anything that is not tilled soil is not a farming attempt, and this must fall
  // straight through so the caller can eat the thing instead.
  //
  // There WAS a "till the soil first" hint here, gated on turf and loam so it only
  // fired when somebody was "plainly trying to farm". That reasoning was wrong:
  // grass and dirt are what a player is looking at almost all the time, so the hint
  // fired on nearly every attempt to eat a carrot. A message that appears when you
  // did not ask a question is not help. Where to plant lives on the item's tooltip
  // now, which is there whenever it is wanted and silent when it is not.
  // Either soil sows. Fertilised farmland is still farmland — forgetting that here
  // would mean the reward for making the good soil was being unable to plant in it.
  if (!world::isFarmland(target)) return plan;
  if (!clearAbove) return plan;

  plan.action = FarmAction::Sow;
  plan.crop = crop;
  return plan;
}

}  // namespace hr::game
