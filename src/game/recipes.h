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

// Built once at startup and read-only thereafter.
class RecipeBook {
 public:
  static const RecipeBook& get();

  const std::vector<Recipe>& recipes() const { return recipes_; }
  const std::vector<SmeltingRecipe>& smelting() const { return smelting_; }

  // The explicit base fuels. Everything made *from* them gets its burn time
  // derived — see crafting::fuelValue.
  const std::unordered_map<std::string, float>& baseFuels() const { return fuels_; }

 private:
  RecipeBook();

  std::vector<Recipe> recipes_;
  std::vector<SmeltingRecipe> smelting_;
  std::unordered_map<std::string, float> fuels_;
};

inline const RecipeBook& recipeBook() { return RecipeBook::get(); }

}  // namespace hr::game
