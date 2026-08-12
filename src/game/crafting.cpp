#include "game/crafting.h"

#include <algorithm>
#include <unordered_map>

#include "game/items.h"
#include "world/blocks.h"

namespace hr::game {
namespace {

// Hand recipes work anywhere; a workbench recipe needs the 3x3 grid.
bool stationAllows(CraftStation recipeStation, CraftStation station) {
  return recipeStation == CraftStation::Hand || recipeStation == station;
}

// The explicit raws, plus anything flagged as a plank or a log — so every wood
// type, current and future, is a base fuel with no extra entry.
float baseFuel(const std::string& key) {
  const auto& fuels = recipeBook().baseFuels();
  auto it = fuels.find(key);
  if (it != fuels.end()) return it->second;

  const ItemDef* item = getItem(key);
  if (item && item->type == ItemType::Block) {
    const world::BlockDef& b = world::blocks().def(item->blockId);
    if (b.isPlank) return 9;
    if (b.isLog) return 12;
  }
  return 0;
}

std::vector<std::pair<std::string, int>> recipeIngredients(const Recipe& r) {
  if (r.type == RecipeType::Shapeless) return r.ingredients;
  std::vector<std::pair<std::string, int>> counts;
  for (const RecipeCell& cell : r.cells) {
    auto it = std::find_if(counts.begin(), counts.end(),
                           [&](const auto& e) { return e.first == cell.key; });
    if (it != counts.end()) {
      ++it->second;
    } else {
      counts.emplace_back(cell.key, 1);
    }
  }
  return counts;
}

// Burn times for every item, resolved once.
//
// The web build memoised this lazily, which made the result depend on the order
// the interface happened to ask: the recursion writes a 0 guard before recursing,
// so whichever side of the slab <-> vertical-slab pair was queried first came out
// as fuel and the other was pinned at 0 forever. The algorithm is kept as-is —
// including that guard, because it is also what stops the recursion — but it is
// driven here in item-registration order, so the answer is at least the same in
// every session and on every machine.
class FuelTable {
 public:
  static const FuelTable& get() {
    static const FuelTable instance;
    return instance;
  }

  float value(const std::string& key) const {
    auto it = values_.find(key);
    return it == values_.end() ? 0.0f : it->second;
  }

 private:
  FuelTable() {
    for (const ItemDef& item : items().all()) resolve(item.key);
  }

  float resolve(const std::string& key) {
    if (auto it = values_.find(key); it != values_.end()) return it->second;

    const float base = baseFuel(key);
    if (base > 0) {
      values_[key] = base;
      return base;
    }
    values_[key] = 0;  // guard against recipe cycles

    float best = 0;
    for (const Recipe& r : recipeBook().recipes()) {
      if (r.outKey != key) continue;
      const auto ings = recipeIngredients(r);
      if (ings.empty()) continue;
      float total = 0;
      bool ok = true;
      for (const auto& [ingKey, count] : ings) {
        const float fv = resolve(tagged(ingKey));
        if (fv <= 0) {
          ok = false;
          break;
        }
        total += fv * count;
      }
      if (ok) {
        const float per = total / static_cast<float>(std::max(1, r.outCount));
        if (per > best) best = per;
      }
    }
    values_[key] = best;
    return best;
  }

  // An ingredient tag stands in for a representative member.
  static std::string tagged(const std::string& key) {
    if (!key.empty() && key.front() == '#') return world::blocks().tagRepresentative(key);
    return key;
  }

  std::unordered_map<std::string, float> values_;
};

}  // namespace

CraftMatch matchGrid(const std::vector<ItemStack>& grid, int size, CraftStation station) {
  // Bounding box of the filled cells.
  int minR = 99, maxR = -1, minC = 99, maxC = -1, filled = 0;
  for (std::size_t i = 0; i < grid.size(); ++i) {
    if (grid[i].empty()) continue;
    const int r = static_cast<int>(i) / size;
    const int c = static_cast<int>(i) % size;
    minR = std::min(minR, r);
    maxR = std::max(maxR, r);
    minC = std::min(minC, c);
    maxC = std::max(maxC, c);
    ++filled;
  }
  if (filled == 0) return {};

  const world::BlockRegistry& reg = world::blocks();

  for (const Recipe& r : recipeBook().recipes()) {
    if (!stationAllows(r.station, station)) continue;

    if (r.type == RecipeType::Shapeless) {
      // The bag has to match exactly: same set of keys, same counts. Shapeless
      // ingredients are concrete keys, never tags.
      std::vector<std::pair<std::string, int>> counts;
      for (const ItemStack& cell : grid) {
        if (cell.empty()) continue;
        auto it = std::find_if(counts.begin(), counts.end(),
                               [&](const auto& e) { return e.first == cell.key; });
        if (it != counts.end()) {
          ++it->second;
        } else {
          counts.emplace_back(cell.key, 1);
        }
      }
      if (counts.size() != r.ingredients.size()) continue;
      bool ok = true;
      for (const auto& [key, need] : r.ingredients) {
        auto it = std::find_if(counts.begin(), counts.end(),
                               [&](const auto& e) { return e.first == key; });
        if (it == counts.end() || it->second != need) {
          ok = false;
          break;
        }
      }
      if (ok) return {&r, r.outKey, r.outCount};
      continue;
    }

    if (maxC - minC + 1 != r.width || maxR - minR + 1 != r.height) continue;
    if (filled != static_cast<int>(r.cells.size())) continue;
    bool ok = true;
    for (const RecipeCell& cell : r.cells) {
      const int gi = (minR + cell.row) * size + (minC + cell.col);
      if (gi < 0 || gi >= static_cast<int>(grid.size())) {
        ok = false;
        break;
      }
      const ItemStack& g = grid[gi];
      if (g.empty() || !reg.ingredientMatches(cell.key, g.key)) {
        ok = false;
        break;
      }
    }
    if (ok) return {&r, r.outKey, r.outCount};
  }
  return {};
}

void consumeGrid(std::vector<ItemStack>& grid, int size, const Recipe& recipe) {
  if (recipe.type == RecipeType::Shapeless) {
    std::vector<std::pair<std::string, int>> remaining = recipe.ingredients;
    for (ItemStack& g : grid) {
      if (g.empty()) continue;
      auto it = std::find_if(remaining.begin(), remaining.end(),
                             [&](const auto& e) { return e.first == g.key; });
      if (it == remaining.end() || it->second <= 0) continue;
      g.count -= 1;
      it->second -= 1;
      if (g.count <= 0) g.clear();
    }
    return;
  }

  // Re-find the bounding-box origin; the caller may have matched a pattern that
  // sits anywhere in the grid.
  int minR = 99, minC = 99;
  for (std::size_t i = 0; i < grid.size(); ++i) {
    if (grid[i].empty()) continue;
    minR = std::min(minR, static_cast<int>(i) / size);
    minC = std::min(minC, static_cast<int>(i) % size);
  }
  for (const RecipeCell& cell : recipe.cells) {
    const int gi = (minR + cell.row) * size + (minC + cell.col);
    if (gi < 0 || gi >= static_cast<int>(grid.size())) continue;
    ItemStack& g = grid[gi];
    if (g.empty()) continue;
    g.count -= 1;
    if (g.count <= 0) g.clear();
  }
}

const SmeltingRecipe* smeltingFor(std::string_view key) {
  for (const SmeltingRecipe& s : recipeBook().smelting()) {
    // Through the tag matcher, not by string equality: the charcoal entry is
    // "#logs", and comparing keys made it oak and nothing else — so a forge in a
    // pine forest refused every log fed into it while the book cheerfully said
    // logs make charcoal. The same trap the wooden tool recipes had.
    if (world::blocks().ingredientMatches(s.in, key)) return &s;
  }
  return nullptr;
}

float fuelValue(std::string_view key) {
  if (!key.empty() && key.front() == '#') {
    return FuelTable::get().value(world::blocks().tagRepresentative(key));
  }
  return FuelTable::get().value(std::string(key));
}

// --- the kitchen -------------------------------------------------------------

bool cookTagMatches(std::string_view want, std::string_view have) {
  if (want.empty() || have.empty()) return false;
  if (want[0] != '#') return want == have;

  // Food tags are derived from the item's own NutritionGroup rather than listed, so
  // a crop added later joins its group by existing and cannot be left out of one.
  const ItemDef* def = getItem(have);
  if (def != nullptr && def->group != NutritionGroup::None) {
    const char* tag = nullptr;
    switch (def->group) {
      case NutritionGroup::Grain: tag = "#grain"; break;
      case NutritionGroup::Vegetable: tag = "#vegetable"; break;
      case NutritionGroup::Fruit: tag = "#fruit"; break;
      case NutritionGroup::Protein: tag = "#protein"; break;
      case NutritionGroup::Dairy: tag = "#dairy"; break;
      case NutritionGroup::None: break;
    }
    if (tag != nullptr && want == tag) return true;
  }
  // Fall through to the block tags, so #planks and #logs keep working here too.
  return world::blocks().ingredientMatches(want, have);
}

CookMatch matchCooking(Kitchen station, const std::vector<ItemStack>& slots,
                       const ItemStack& container) {
  CookMatch best;
  int bestDemand = -1;
  const std::vector<CookingRecipe>& table = recipeBook().cooking();

  for (std::size_t ri = 0; ri < table.size(); ++ri) {
    const CookingRecipe& r = table[ri];
    if (r.station != station) continue;
    if (!r.container.empty() && (container.empty() || container.key != r.container)) continue;

    // Greedy allocation over a scratch tally of the counts. A slot may only be spent
    // once, which is the whole reason this is not "test each requirement against the
    // slots independently": two #vegetable requirements must need two vegetables,
    // not one vegetable counted twice.
    std::vector<int> left;
    left.reserve(slots.size());
    for (const ItemStack& s : slots) left.push_back(s.empty() ? 0 : s.count);

    int score = 0;
    bool ok = true;
    for (const auto& [want, need] : r.ingredients) {
      int remaining = need;
      for (std::size_t si = 0; si < slots.size() && remaining > 0; ++si) {
        if (left[si] <= 0 || !cookTagMatches(want, slots[si].key)) continue;
        const int take = left[si] < remaining ? left[si] : remaining;
        left[si] -= take;
        remaining -= take;
        if (const ItemDef* def = getItem(slots[si].key)) score += def->quality * take;
      }
      if (remaining > 0) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;

    const CookingRecipe::Tier* tier = r.tierFor(score);
    if (tier == nullptr) continue;

    // THE MOST DEMANDING SATISFIED RECIPE WINS, not the first one found.
    //
    // Crafting can take the first match because shapes are distinct. Cooking cannot:
    // its requirements are tags, so a general recipe matches everything a specific
    // one does. "Any two vegetables -> Roast Vegetables" is satisfied by the pumpkin
    // and carrot sitting in a Stuffed Pumpkin, and first-match-wins would hand the
    // player the lesser dish every single time while the better recipe sat there
    // looking correct.
    //
    // Demand is the total ingredient count, so a three-ingredient recipe beats a
    // two-ingredient one. Ties keep table order, which preserves the old contract
    // that a recipe added later cannot shadow one already there.
    int demand = 0;
    for (const auto& [want, need] : r.ingredients) {
      (void)want;
      demand += need;
    }
    if (demand <= bestDemand) continue;
    bestDemand = demand;
    best.recipe = static_cast<int>(ri);
    best.score = score;
    best.out = tier->out;
    best.outCount = tier->count;
    best.seconds = r.seconds;
  }
  return best;
}

void consumeCooking(const CookingRecipe& r, std::vector<ItemStack>& slots,
                    ItemStack& container) {
  for (const auto& [want, need] : r.ingredients) {
    int remaining = need;
    for (std::size_t si = 0; si < slots.size() && remaining > 0; ++si) {
      if (slots[si].empty() || !cookTagMatches(want, slots[si].key)) continue;
      const int take = slots[si].count < remaining ? slots[si].count : remaining;
      slots[si].count -= take;
      remaining -= take;
      if (slots[si].count <= 0) slots[si].clear();
    }
  }
  if (!r.container.empty() && !container.empty()) {
    container.count--;
    if (container.count <= 0) container.clear();
  }
}

}  // namespace hr::game
