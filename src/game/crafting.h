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

}  // namespace hr::game
