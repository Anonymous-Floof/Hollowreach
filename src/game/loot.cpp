#include "game/loot.h"

#include <algorithm>

#include "core/prng.h"
#include "game/items.h"

namespace hr::game {
namespace {

// Coordinate steps for the successive rolls at one container. Distinct and
// irregular on purpose: see the note in loot.h about hash3i's weak avalanche, which
// makes "same coordinates, different salt" the one thing that does not work.
constexpr int kStepRollsX = 17, kStepRollsY = 5, kStepRollsZ = 83;
constexpr int kStepPickX = 53, kStepPickY = 11, kStepPickZ = 199;
constexpr int kStepCountX = 131, kStepCountY = 23, kStepCountZ = 7;

// Salts. Only a starting point — the coordinate steps above are what actually
// separates these.
constexpr std::uint32_t kSaltRolls = 0x100du;
constexpr std::uint32_t kSaltPick = 0x1007u;
constexpr std::uint32_t kSaltCount = 0x1013u;

// A roll in [lo, hi]. Inclusive at both ends, so a fixed count is min == max.
int rollRange(double r, int lo, int hi) {
  if (hi <= lo) return lo;
  const int span = hi - lo + 1;
  int v = lo + static_cast<int>(r * span);
  if (v > hi) v = hi;  // r is [0,1) so this only fires on a rounding edge
  return v;
}

}  // namespace

int LootPool::totalWeight() const {
  int sum = 0;
  for (const LootEntry& e : entries) {
    if (e.weight > 0) sum += e.weight;
  }
  return sum;
}

// ---------------------------------------------------------------------------
// The tables
// ---------------------------------------------------------------------------
//
// Written against what a player can actually reach. A dungeon sits well below the
// surface, so whoever is standing in front of one has stone tools at least and is
// probably after ferralite — the common chest is therefore a leg-up rather than a
// jump: fuel, food, the metal they are already mining, and occasionally a tool one
// tier above what got them there.
//
// The altar chest is the one worth the walk. It is the only place aetherite turns
// up without mining for it, and it is deliberately thin: one good thing, not a kit.
LootBook::LootBook() {
  auto add = [this](LootTable t) { tables_.emplace(t.id, std::move(t)); };

  add(LootTable{"dungeon/chest",
                {
                    // Something to burn and something to eat, always. A chest that
                    // can roll entirely empty reads as a bug rather than bad luck.
                    LootPool{2, 4,
                             {
                                 {"embercoal", 2, 6, 30},
                                 {"charcoal", 1, 4, 14},
                                 {"raw_copper", 2, 5, 22},
                                 {"raw_ferralite", 1, 3, 14},
                                 {"stick", 2, 6, 12},
                                 {"leather", 1, 3, 10},
                                 {"paper", 1, 4, 8},
                             }},
                    LootPool{1, 2,
                             {
                                 {"pork_cooked", 1, 3, 26},
                                 {"beef_cooked", 1, 3, 22},
                                 {"rotten_flesh", 1, 4, 18},
                                 {"azurite", 1, 2, 12},
                                 {"sparkstone", 1, 2, 8},
                             }},
                    // The occasional tool. Weighted so most chests have none.
                    LootPool{0, 1,
                             {
                                 {"pick_stone", 1, 1, 20},
                                 {"sword_stone", 1, 1, 16},
                                 {"pick_copper", 1, 1, 10},
                                 {"sword_copper", 1, 1, 8},
                                 {"helmet_copper", 1, 1, 6},
                                 {"boots_copper", 1, 1, 6},
                             }},
                }});

  add(LootTable{"dungeon/altar",
                {
                    LootPool{1, 2,
                             {
                                 {"aetherite", 1, 3, 20},
                                 {"sparkstone", 2, 5, 22},
                                 {"gloamite", 1, 2, 14},
                                 {"verdanite", 1, 2, 14},
                                 {"ferralite_ingot", 1, 3, 18},
                                 {"sunbrass_ingot", 1, 2, 12},
                             }},
                    LootPool{0, 1,
                             {
                                 {"pick_ferralite", 1, 1, 18},
                                 {"sword_ferralite", 1, 1, 16},
                                 {"chest_ferralite", 1, 1, 10},
                                 {"pick_sunbrass", 1, 1, 8},
                                 {"pick_aetherite", 1, 1, 3},
                             }},
                }});
}

const LootBook& LootBook::get() {
  static const LootBook instance;
  return instance;
}

const LootTable* LootBook::find(std::string_view id) const {
  const auto it = tables_.find(std::string(id));
  return it == tables_.end() ? nullptr : &it->second;
}

std::vector<const LootTable*> LootBook::sorted() const {
  std::vector<const LootTable*> out;
  out.reserve(tables_.size());
  for (const auto& [key, table] : tables_) out.push_back(&table);
  std::sort(out.begin(), out.end(),
            [](const LootTable* a, const LootTable* b) { return a->id < b->id; });
  return out;
}

std::vector<ItemStack> rollLoot(const LootTable& table, std::uint32_t seed, int x, int y, int z) {
  std::vector<ItemStack> out;

  // Every roll in the whole table walks one shared coordinate cursor, so no two
  // rolls anywhere ever land on the same hash input. Pool index and roll index both
  // advance it, which is what keeps pool 2's first roll independent of pool 1's.
  int step = 0;
  const auto at = [&](std::uint32_t salt, int sx, int sy, int sz) {
    ++step;
    return hash3i(seed ^ salt, x + sx * step, y + sy * step, z + sz * step);
  };

  for (const LootPool& pool : table.pools) {
    const int weight = pool.totalWeight();
    if (weight <= 0 || pool.entries.empty()) continue;

    const int rolls =
        rollRange(at(kSaltRolls, kStepRollsX, kStepRollsY, kStepRollsZ), pool.minRolls,
                  pool.maxRolls);

    for (int i = 0; i < rolls; ++i) {
      // Pick by weight: walk the entries subtracting as we go, so an entry's share
      // is exactly its weight over the total and adding one does not disturb the
      // others' relative odds.
      int ticket = static_cast<int>(at(kSaltPick, kStepPickX, kStepPickY, kStepPickZ) * weight);
      if (ticket >= weight) ticket = weight - 1;

      const LootEntry* chosen = nullptr;
      for (const LootEntry& e : pool.entries) {
        if (e.weight <= 0) continue;
        ticket -= e.weight;
        if (ticket < 0) {
          chosen = &e;
          break;
        }
      }
      if (!chosen) continue;

      const int count = rollRange(at(kSaltCount, kStepCountX, kStepCountY, kStepCountZ),
                                  chosen->minCount, chosen->maxCount);
      if (count <= 0) continue;

      ItemStack s;
      s.key = chosen->key;
      s.count = count;
      // The thing Inventory::give would have done for us. A generated chest is
      // written to directly, so without this a looted pickaxe arrives with dura -1
      // — which reads as "does not wear" and hands out an unbreakable tool.
      const int dura = maxDurability(chosen->key);
      s.dura = dura > 0 ? dura : -1;
      out.push_back(std::move(s));
    }
  }

  return out;
}

std::vector<ItemStack> rollLoot(std::string_view tableId, std::uint32_t seed, int x, int y,
                                int z) {
  const LootTable* table = lootBook().find(tableId);
  if (!table) return {};
  return rollLoot(*table, seed, x, y, z);
}

}  // namespace hr::game
