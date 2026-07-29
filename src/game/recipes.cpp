#include "game/recipes.h"

#include <algorithm>

#include "game/items.h"
#include "world/blocks.h"

namespace hr::game {
namespace {

// Trims a pattern to its own bounding box the way js/game/crafting.js:10-21 does
// at module load, so matching only ever has to translate.
void parseShaped(Recipe& r, const std::vector<std::string>& pattern,
                 const std::vector<std::pair<char, std::string>>& legend) {
  std::size_t w = 0;
  for (const std::string& row : pattern) w = std::max(w, row.size());
  r.width = static_cast<int>(w);
  r.height = static_cast<int>(pattern.size());
  for (int row = 0; row < r.height; ++row) {
    for (int col = 0; col < r.width; ++col) {
      const char ch = col < static_cast<int>(pattern[row].size()) ? pattern[row][col] : ' ';
      if (ch == ' ' || ch == '.') continue;
      for (const auto& [letter, key] : legend) {
        if (letter == ch) {
          r.cells.push_back({row, col, key});
          break;
        }
      }
    }
  }
}

}  // namespace

RecipeBook::RecipeBook() {
  auto shaped = [this](std::vector<std::string> pattern,
                       std::vector<std::pair<char, std::string>> legend, std::string outKey,
                       int outCount, CraftStation station) {
    Recipe r;
    r.type = RecipeType::Shaped;
    r.station = station;
    r.outKey = std::move(outKey);
    r.outCount = outCount;
    parseShaped(r, pattern, legend);
    recipes_.push_back(std::move(r));
  };
  auto shapeless = [this](std::vector<std::pair<std::string, int>> in, std::string outKey,
                          int outCount, CraftStation station) {
    Recipe r;
    r.type = RecipeType::Shapeless;
    r.station = station;
    r.ingredients = std::move(in);
    r.outKey = std::move(outKey);
    r.outCount = outCount;
    recipes_.push_back(std::move(r));
  };

  constexpr CraftStation kHand = CraftStation::Hand;
  constexpr CraftStation kBench = CraftStation::Workbench;

  // "#planks" is an ingredient tag (see world/blocks.h): any wood's planks work,
  // so sticks, workbench, chest, bed and boat can be built from any wood, even
  // mixed.

  // --- bootstrap (hand) ---
  shapeless({{"log", 1}}, "planks", 4, kHand);
  shaped({"X", "X"}, {{'X', "#planks"}}, "stick", 4, kHand);
  shaped({"XX", "XX"}, {{'X', "#planks"}}, "workbench", 1, kHand);
  shaped({"E", "S"}, {{'E', "embercoal"}, {'S', "stick"}}, "emberlight", 4, kHand);
  shaped({"E", "S"}, {{'E', "charcoal"}, {'S', "stick"}}, "emberlight", 4, kHand);

  // --- stations & building (workbench) ---
  shaped({"XXX", "X X", "XXX"}, {{'X', "cobbled"}}, "forge", 1, kBench);
  shaped({"XXX", "X X", "XXX"}, {{'X', "#planks"}}, "chest", 1, kBench);
  shaped({"XX", "XX"}, {{'X', "greystone"}}, "bricks", 4, kBench);
  shaped({"XX", "XX"}, {{'X', "cobbled"}}, "polished", 4, kBench);
  shaped({"XX", "XX"}, {{'X', "sand"}}, "sandstone", 4, kBench);

  shaped({"X X", "XXX", "X X"}, {{'X', "stick"}}, "ladder", 3, kBench);
  shaped({"WWW", "PPP"}, {{'W', "wool"}, {'P', "#planks"}}, "bed", 1, kBench);

  // boat: a hull of planks
  shaped({"P P", "PPP"}, {{'P', "#planks"}}, "boat", 1, kBench);
  // bucket: a folded V of iron
  shaped({"X X", " X "}, {{'X', "ferralite_ingot"}}, "bucket", 1, kBench);
  // paper from a row of shore papyrus
  shaped({"XXX"}, {{'X', "papyrus"}}, "paper", 3, kBench);
  // the Atlas: pages, a leather binding, an azurite compass jewel
  shapeless({{"paper", 3}, {"leather", 1}, {"azurite", 1}}, "atlas", 1, kBench);

  // Soul Anchor: one of every ore ringing a sparkstone heart
  shaped({"ECF", "ZSG", "VDB"},
         {{'E', "embercoal"},
          {'C', "raw_copper"},
          {'F', "raw_ferralite"},
          {'Z', "azurite"},
          {'S', "sparkstone"},
          {'G', "raw_sunbrass"},
          {'V', "verdanite"},
          {'D', "aetherite"},
          {'B', "gloamite"}},
         "soul_anchor", 1, kBench);

  // Wayshard: gloamite keyed to a sparkstone spark
  shapeless({{"gloamite", 1}, {"sparkstone", 1}}, "wayshard", 2, kHand);

  // Per-wood doors (3 per 2x3) and trapdoors (2 per 2x3). Alderwood keeps its
  // original "door"/"trapdoor" keys; the newer woods get their own.
  struct WoodDoor {
    const char* plank;
    const char* door;
    const char* trap;
  };
  static constexpr WoodDoor kWoodDoors[] = {
      {"planks", "door", "trapdoor"},
      {"pine_planks", "pine_door", "pine_trapdoor"},
      {"dusk_planks", "dusk_door", "dusk_trapdoor"},
      {"birch_planks", "birch_door", "birch_trapdoor"},
      {"palm_planks", "palm_door", "palm_trapdoor"},
  };
  for (const WoodDoor& w : kWoodDoors) {
    shaped({"XX", "XX", "XX"}, {{'X', w.plank}}, w.door, 3, kBench);
    shaped({"XXX", "XXX"}, {{'X', w.plank}}, w.trap, 2, kBench);
  }

  const world::BlockRegistry& reg = world::blocks();

  // Vertical slabs: slab <-> vertical slab, both ways (1:1, shapeless).
  for (const std::string& base : reg.buildMaterialKeys()) {
    auto slabIt = reg.slabOf().find(base);
    if (slabIt == reg.slabOf().end()) continue;
    auto vslabIt = reg.vslabOf().find(slabIt->second);
    if (vslabIt == reg.vslabOf().end()) continue;
    shapeless({{slabIt->second, 1}}, vslabIt->second, 1, kHand);
    shapeless({{vslabIt->second, 1}}, slabIt->second, 1, kHand);
  }

  // --- new material families (mirroring the originals) ---
  for (const char* w : {"pine", "dusk", "birch", "palm"}) {
    shapeless({{std::string(w) + "_log", 1}}, std::string(w) + "_planks", 4, kHand);
  }
  for (const char* s : {"umber", "slate"}) {
    const std::string id = s;
    shaped({"XX", "XX"}, {{'X', id + "stone"}}, "polished_" + id, 4, kBench);
    shaped({"XX", "XX"}, {{'X', "polished_" + id}}, "bricks_" + id, 4, kBench);
  }

  // --- stairs (6 per 3-2-1) and slabs (6 per row of 3) for every material ---
  for (const std::string& base : reg.buildMaterialKeys()) {
    auto it = reg.stairOf().find(base);
    if (it != reg.stairOf().end()) {
      shaped({"X  ", "XX ", "XXX"}, {{'X', base}}, it->second, 6, kBench);
    }
  }
  for (const std::string& base : reg.buildMaterialKeys()) {
    auto it = reg.slabOf().find(base);
    if (it != reg.slabOf().end()) shaped({"XXX"}, {{'X', base}}, it->second, 6, kBench);
  }

  // --- tools & armour, generated per material tier (workbench) ---
  struct PatternSpec {
    const char* name;
    std::vector<std::string> rows;
  };
  const PatternSpec kToolPatterns[] = {
      {"pick", {"XXX", " S ", " S "}},
      {"axe", {"XX", "XS", " S"}},
      {"shovel", {"X", "S", "S"}},
      {"sword", {"X", "X", "S"}},
  };
  for (const ToolMaterial& m : toolMaterials()) {
    for (const PatternSpec& p : kToolPatterns) {
      shaped(p.rows, {{'X', m.item}, {'S', "stick"}}, std::string(p.name) + "_" + m.id, 1,
             kBench);
    }
  }

  const PatternSpec kArmorPatterns[] = {
      {"helmet", {"XXX", "X X"}},
      {"chest", {"X X", "XXX", "XXX"}},
      {"legs", {"XXX", "X X", "X X"}},
      {"boots", {"X X", "X X"}},
  };
  for (const ArmorMaterial& m : armorMaterials()) {
    const std::string id = m.id;
    const std::string ingredient = id == "aetherite" ? "aetherite" : id + "_ingot";
    for (const PatternSpec& p : kArmorPatterns) {
      shaped(p.rows, {{'X', ingredient}}, std::string(p.name) + "_" + id, 1, kBench);
    }
  }

  // --- smelting (forge) ---
  smelting_ = {
      {"log", "charcoal", 8},
      {"raw_copper", "copper_ingot", 6},
      {"raw_ferralite", "ferralite_ingot", 8},
      {"raw_sunbrass", "sunbrass_ingot", 8},
      {"sand", "glass", 5},
      {"cobbled", "greystone", 6},
      {"pork_raw", "pork_cooked", 6},
      {"beef_raw", "beef_cooked", 6},
  };

  // Base burn time in seconds for the *raw* fuels only. Everything else made
  // from these — wooden tools, chests, boats, wooden stairs and doors, torches
  // (coal + stick), any wood type — gets its burn time auto-computed from its
  // recipe in crafting::fuelValue, so a new wooden item needs no entry here.
  fuels_ = {
      {"embercoal", 48}, {"charcoal", 48}, {"log", 12}, {"planks", 9}, {"stick", 3},
  };
}

const RecipeBook& RecipeBook::get() {
  static const RecipeBook instance;
  return instance;
}

}  // namespace hr::game
