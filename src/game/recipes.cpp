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

  // A stretched canvas in a wooden frame: planks around the edge, wool for the
  // cloth, azurite ground into the pigment that fixes an image onto it.
  //
  // Registered LAST for the same reason the block is: matching is first-match-wins
  // over an ordered list, so the golden vectors compare recipes by index, and
  // slotting a new one in beside the bed renumbered a hundred and nineteen
  // recipes that had not changed at all.
  shaped({"PPP", "PWP", "PAP"}, {{'P', "#planks"}, {'W', "wool"}, {'A', "azurite"}}, "canvas", 1,
         kBench);

  // --- The Farming update, all appended -----------------------------------
  //
  // The hoes deliberately do NOT join kToolPatterns above. Adding a fifth pattern
  // to that loop would interleave six hoes through the tool matrix and renumber
  // roughly a hundred recipes that had not changed — the exact accident the canvas
  // comment above records. A separate loop at the end costs one loop and keeps the
  // golden diff to six new lines.
  for (const ToolMaterial& m : toolMaterials()) {
    shaped({"XX", "S ", "S "}, {{'X', m.item}, {'S', "stick"}}, "hoe_" + std::string(m.id), 1,
           kBench);
  }

  // The three kitchen stations, and the bowls every pot meal is served into.
  //
  // The bowl is a shallow wooden V — the bucket's shape in wood rather than iron,
  // which is exactly how it reads. It was authored as the BOAT's hull, and the boat
  // is registered a hundred and thirty lines above out of the same #planks, so the
  // bowl could not be crafted and every pot meal needs one. The whole Cooking Pot
  // was unusable because of one repeated pattern.
  shaped({"P P", " P "}, {{'P', "#planks"}}, "bowl", 4, kBench);
  shaped({"PPP", "L L"}, {{'P', "#planks"}, {'L', "log"}}, "cutting_board", 1, kBench);
  // A stone shell with a firebox burnt into the bottom of it. The embercoal is not
  // decoration: a ring of plain cobbled is ALREADY the forge, registered two hundred
  // lines above, and matching is first-match-wins — so the stove shipped in the
  // 2.14.0 draft could not be crafted at all. The grid handed back a forge every
  // time. `testRecipesReachable` now proves every recipe can be made, which is the
  // general form of that bug and the only reason it will not happen again.
  shaped({"CCC", "C C", "CEC"}, {{'C', "cobbled"}, {'E', "embercoal"}}, "stove", 1, kBench);
  shaped({"I I", "I I", "III"}, {{'I', "ferralite_ingot"}}, "cooking_pot", 1, kBench);
  shapeless({{"verdanite", 1}, {"rotten_flesh", 2}}, "fertiliser", 3, kBench);

  // --- smelting (forge) ---
  //
  // MEAT IS GONE FROM HERE. Cooking food at a forge was the whole of the kitchen
  // before this update, and leaving it would mean the stove had to compete with a
  // block every player already owns. They were the last two entries, so removing
  // them renumbers nothing.
  smelting_ = {
      {"#logs", "charcoal", 8},
      {"raw_copper", "copper_ingot", 6},
      {"raw_ferralite", "ferralite_ingot", 8},
      {"raw_sunbrass", "sunbrass_ingot", 8},
      {"sand", "glass", 5},
      {"cobbled", "greystone", 6},
  };

  // --- the kitchen ----------------------------------------------------------
  //
  // Three stations and a deliberate progression: the board needs no fuel and turns
  // raw things into better raw things, the stove cooks one item at a time, and the
  // pot is where several ingredients become a meal worth having grown them for.
  //
  // Tiers are authored BEST FIRST — tierFor returns the first tier whose minScore is
  // met — so a better tier added later can never silently downgrade one already here.
  {
    auto cut = [this](std::vector<std::pair<std::string, int>> in, std::string out,
                      int count, float secs) {
      CookingRecipe r;
      r.station = Kitchen::Cutting;
      r.ingredients = std::move(in);
      r.seconds = secs;
      r.tiers.push_back({0, std::move(out), count});
      cooking_.push_back(std::move(r));
    };
    auto stove = [this](std::vector<std::pair<std::string, int>> in, std::string out,
                        int count, float secs) {
      CookingRecipe r;
      r.station = Kitchen::Stove;
      r.ingredients = std::move(in);
      r.seconds = secs;
      r.tiers.push_back({0, std::move(out), count});
      cooking_.push_back(std::move(r));
    };
    auto pot = [this](std::vector<std::pair<std::string, int>> in,
                      std::vector<CookingRecipe::Tier> tiers, float secs) {
      CookingRecipe r;
      r.station = Kitchen::Pot;
      r.ingredients = std::move(in);
      r.container = "bowl";
      r.seconds = secs;
      r.tiers = std::move(tiers);
      cooking_.push_back(std::move(r));
    };

    // Prep. Butchering is the important one: one raw chop becomes two strips, so
    // meat goes further when it is worked rather than eaten.
    cut({{"wheat", 2}}, "flour", 1, 2.0f);
    cut({{"barley", 2}}, "flour", 1, 2.0f);
    cut({{"beef_raw", 1}}, "meat_strips", 2, 2.5f);
    cut({{"pork_raw", 1}}, "meat_strips", 2, 2.5f);

    // The stove. This is where meat cooking lives now that the forge does not do it.
    stove({{"pork_raw", 1}}, "pork_cooked", 1, 6.0f);
    stove({{"beef_raw", 1}}, "beef_cooked", 1, 6.0f);
    stove({{"meat_strips", 1}}, "cooked_strips", 1, 4.0f);
    stove({{"flour", 1}}, "bread", 1, 8.0f);
    stove({{"potato", 1}}, "baked_potato", 1, 6.0f);
    stove({{"maize", 1}}, "grilled_maize", 1, 5.0f);
    stove({{"milk_bucket", 1}}, "cheese", 2, 10.0f);
    stove({{"bread", 1}, {"garlic", 1}}, "garlic_bread", 1, 5.0f);
    stove({{"bread", 1}, {"cheese", 1}}, "cheese_toastie", 1, 6.0f);
    stove({{"#vegetable", 2}}, "roast_vegetables", 1, 8.0f);
    stove({{"flour", 2}, {"#vegetable", 2}}, "veg_pie", 1, 12.0f);
    stove({{"flour", 2}, {"meat_strips", 2}}, "meat_pie", 1, 12.0f);
    stove({{"pumpkin", 1}, {"#grain", 1}, {"#vegetable", 1}}, "stuffed_pumpkin", 1, 14.0f);

    // The pot. Every one of these takes a bowl, and the bowl comes back when the
    // meal is eaten rather than being consumed outright.
    //
    // The tiered pair is the whole "better ingredients, better meal" claim: the same
    // three-vegetable soup becomes a hearty stew once the score clears the
    // threshold, which in practice means meat or a seasoning went in with them.
    // The thresholds are worked out from the actual ingredient qualities rather
    // than guessed, because a tier nobody can reach is indistinguishable from a
    // tier that does not exist. Plain vegetables are 3 each (9 for three); the
    // seasonings are 5 and tomato is 4, so 12 is "you put something interesting in".
    // The first pass used 14 here and 16 below — and 16 is ABOVE the maximum this
    // recipe can score, so that tier could never once have fired.
    pot({{"#vegetable", 3}}, {{12, "hearty_stew", 1}, {0, "vegetable_soup", 1}}, 10.0f);
    // Anything with real protein in it is hearty by definition, so this one only has
    // to clear the plain-vegetable floor.
    pot({{"#vegetable", 2}, {"#protein", 1}},
        {{10, "hearty_stew", 1}, {0, "vegetable_soup", 1}}, 12.0f);
    pot({{"pumpkin", 2}, {"garlic", 1}}, {{0, "pumpkin_soup", 1}}, 10.0f);
    pot({{"tomato", 3}}, {{0, "tomato_soup", 1}}, 10.0f);
    pot({{"soybean", 3}}, {{0, "bean_stew", 1}}, 11.0f);
    pot({{"rice", 2}, {"#vegetable", 1}}, {{0, "rice_bowl", 1}}, 9.0f);
    pot({{"#grain", 2}, {"#dairy", 1}}, {{0, "porridge", 1}}, 9.0f);
    pot({{"#fruit", 3}}, {{0, "fruit_salad", 1}}, 6.0f);
    pot({{"#fruit", 2}, {"#grain", 1}}, {{0, "berry_compote", 1}}, 7.0f);
    pot({{"cabbage", 1}, {"tomato", 1}, {"onion", 1}}, {{0, "garden_salad", 1}}, 6.0f);
  }

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
