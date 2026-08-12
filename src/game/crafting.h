// Recipe matching for the crafting grid, plus the forge's smelting and fuel
// lookups. Ported from js/game/crafting.js.
//
// A grid is a flat array of size*size slots, empty or holding a stack, exactly as
// the UI lays it out.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "game/inventory.h"
#include "game/recipes.h"

namespace hr::game {

struct CraftMatch {
  const Recipe* recipe = nullptr;
  std::string outKey;
  int outCount = 0;

  explicit operator bool() const { return recipe != nullptr; }
};

// Finds the first recipe the grid satisfies. Shaped recipes match on the grid's
// trimmed bounding box, so a pattern can sit anywhere in a 3x3.
CraftMatch matchGrid(const std::vector<ItemStack>& grid, int size, CraftStation station);

// Removes one craft's worth of ingredients (one from each used cell).
void consumeGrid(std::vector<ItemStack>& grid, int size, const Recipe& recipe);

// The forge recipe consuming `key`, or null.
const SmeltingRecipe* smeltingFor(std::string_view key);

// Burn time in seconds, 0 for anything that is not fuel.
//
// An item is fuel if SOME recipe makes it entirely out of fuel ingredients, and
// its burn time is the summed ingredient fuel divided by the output count. So
// wooden tools, chests, boats, wooden stairs and doors, and torches (coal + stick)
// all burn without an explicit entry, while anything containing stone, metal or
// wool does not.
float fuelValue(std::string_view key);

// --- the kitchen -------------------------------------------------------------

// Matching a cooking station's contents against the cooking table.
//
// Separate from the crafting matcher because it is a different question. A crafting
// grid asks "is this shape this recipe"; a pot asks "do the things in these six
// slots satisfy this bag of requirements", with tags and counts and no positions at
// all. Expressing either in terms of the other would make both worse.
struct CookMatch {
  int recipe = -1;   // index into RecipeBook::cooking(), or -1 for no match
  int score = 0;     // summed ItemDef::quality of what actually went in
  std::string out;   // the output tier the score selected
  int outCount = 1;
  float seconds = 0.0f;
};

// The food tags — #grain, #vegetable, #fruit, #protein, #dairy — live HERE rather
// than beside #planks in the block registry. `world/` sits below `game/`, so the
// block table cannot walk the item registry to find out which items are vegetables;
// putting them there would mean inverting the layering or hand-listing eighteen
// crops in a second place where they could drift.
bool cookTagMatches(std::string_view want, std::string_view have);

// `slots` is everything offered as an ingredient (the pot's six, or the single
// input of the board and the stove); `container` is the bowl slot. Returns the first
// satisfied recipe in table order, matching how the crafting matcher breaks ties, so
// a recipe added later can never shadow one already there.
CookMatch matchCooking(Kitchen station, const std::vector<ItemStack>& slots,
                       const ItemStack& container);

// Removes exactly what a match consumed. Split from matchCooking so a station can
// look without touching — which is what lets it show a progress bar before it has
// taken anything.
void consumeCooking(const CookingRecipe& recipe, std::vector<ItemStack>& slots,
                    ItemStack& container);

}  // namespace hr::game
