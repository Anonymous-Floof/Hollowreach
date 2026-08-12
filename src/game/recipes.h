// Recipe data, ported from js/game/recipes.js.
//
// Crafting recipes are either shapeless (a bag of ingredients) or shaped (a
// pattern of rows). `station` is Hand — which works in the 2x2 player grid *and*
// the workbench — or Workbench, which needs the 3x3. Smelting recipes run in the
// forge.
//
// Most of the table is generated rather than written out: every wood gets doors
// and trapdoors, every building material gets stairs and slabs, and every tool
// and armour tier is stamped from a shared pattern. Adding a material to the block
// table therefore adds its recipes automatically, exactly as in the web build.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hr::game {

enum class RecipeType : std::uint8_t { Shaped, Shapeless };

// Where a recipe can be crafted. Hand recipes also work at a workbench.
enum class CraftStation : std::uint8_t { Hand, Workbench };

// Where a COOKING recipe runs. Separate from CraftStation because none of these
// has a grid: a pot takes a bag of ingredients and a stove takes one thing, and
// neither cares where in the window you put it.
enum class Kitchen : std::uint8_t {
  Cutting,  // prep: slicing, milling, butchering. No fuel, quick.
  Stove,    // one item at a time, with fuel. Took food off the forge.
  Pot,      // the real meals: several ingredients, fuel, and a bowl to serve into.
};

// One occupied cell of a shaped pattern, pre-trimmed to the pattern's own bounding
// box so matching is a translation compare.
struct RecipeCell {
  int row = 0;
  int col = 0;
  std::string key;  // may be a "#tag"
};

struct Recipe {
  RecipeType type = RecipeType::Shaped;
  CraftStation station = CraftStation::Hand;

  // Shaped.
  int width = 0;
  int height = 0;
  std::vector<RecipeCell> cells;

  // Shapeless: ingredient key -> exact count. Order-independent by definition.
  std::vector<std::pair<std::string, int>> ingredients;

  std::string outKey;
  int outCount = 1;
};

struct SmeltingRecipe {
  std::string in;
  std::string out;
  float seconds = 0.0f;
};

// A cooking recipe, and the one piece of machinery that makes cooking worth doing
// rather than merely possible.
//
// THE TIERS ARE HOW "BETTER INGREDIENTS, BETTER MEAL" WORKS WITHOUT ITEM METADATA.
// ItemStack is {key, count, dura} and there is no NBT anywhere in the game, so a
// stew cannot carry "how good am I" on itself. Instead the pot adds up the `quality`
// of what actually went in and picks the best tier that score clears — the same
// recipe, a different output item. A pot of three vegetables makes Vegetable Soup;
// add meat and a chili and the same recipe makes Hearty Stew.
//
// Tiers are authored best-first and the FIRST one whose minScore is met wins, so
// adding a better tier later never silently downgrades an existing one.
struct CookingRecipe {
  Kitchen station = Kitchen::Pot;
  // Ingredient key -> count. A key may be a "#tag" (#vegetable, #grain, ...), which
  // is what lets one soup recipe accept whatever the player actually grew.
  std::vector<std::pair<std::string, int>> ingredients;
  // Consumed and NOT returned: you get it back by eating the meal. Empty for the
  // cutting board and the stove, which serve nothing.
  std::string container;
  float seconds = 0.0f;

  struct Tier {
    int minScore = 0;
    std::string out;
    int count = 1;
  };
  std::vector<Tier> tiers;

  // The output for a given ingredient score, or nullptr when nothing qualifies.
  const Tier* tierFor(int score) const {
    for (const Tier& t : tiers) {
      if (score >= t.minScore) return &t;
    }
    return tiers.empty() ? nullptr : &tiers.back();
  }
};

// Built once at startup and read-only thereafter.
class RecipeBook {
 public:
  static const RecipeBook& get();

  const std::vector<Recipe>& recipes() const { return recipes_; }
  const std::vector<SmeltingRecipe>& smelting() const { return smelting_; }
  const std::vector<CookingRecipe>& cooking() const { return cooking_; }

  // The explicit base fuels. Everything made *from* them gets its burn time
  // derived — see crafting::fuelValue.
  const std::unordered_map<std::string, float>& baseFuels() const { return fuels_; }

 private:
  RecipeBook();

  std::vector<Recipe> recipes_;
  std::vector<SmeltingRecipe> smelting_;
  std::vector<CookingRecipe> cooking_;
  std::unordered_map<std::string, float> fuels_;
};

inline const RecipeBook& recipeBook() { return RecipeBook::get(); }

}  // namespace hr::game
