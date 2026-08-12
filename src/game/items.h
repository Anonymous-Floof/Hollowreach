// The item registry, ported from js/game/items.js.
//
// Three kinds of entry, built in the JS's registration order so indices are
// stable: block items derived from the block table, hand-authored materials /
// foods / usables, and the generated tool and armour matrices.
//
// There are no item image files either. Every non-block item is a hand-placed
// 16x16 pixel sprite, and those same sprites are the *only* source of its
// appearance anywhere in the game: the inventory icon is the sprite blitted at
// 2x, and the dropped and held 3D models are the sprite extruded one texel thick
// (render/itemmesh.h). Icon and model can therefore never disagree.
//
// Resource-pack forward (enabler 6): each sprite is registered as a *provider* of
// `hollowreach:item/<key>` in the PackStack, exactly like a block painter. A pack
// dropping in `item/pick_copper.png` overrides the icon, the dropped model and the
// held model together, with no code change.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "game/nutrition.h"
#include "resource/identifier.h"
#include "resource/image.h"
#include "resource/packstack.h"
#include "world/blocks.h"

namespace hr::game {

// What an item *is*, which decides how a click on it behaves.
enum class ItemType : std::uint8_t {
  Block,     // placeable; blockId is set
  Material,  // crafting input only
  Food,      // right-click to eat
  Boat,      // right-click spawns a rideable entity
  Bucket,    // scoop / pour / milk
  Atlas,     // carrying it unlocks the map
  Warp,      // wayshard: right-click to surface
  Tool,
  Armor,
};

// Tool families. Distinct from world::ToolType because a sword mines nothing —
// it is a weapon that happens to share the tier and durability machinery.
// A hoe mines nothing, exactly as a sword does: it is a tool that borrows the tier
// and durability machinery to do something else entirely — in its case, tilling.
enum class ToolKind : std::uint8_t { None = 0, Pick, Axe, Shovel, Sword, Hoe };

// Which sprite painter draws an item. `Block` means "no sprite": the icon is an
// isometric projection of the block's display shape instead.
enum class IconKind : std::uint8_t {
  Block = 0,
  Stick,
  Lump,
  Nugget,
  Ingot,
  Gem,
  Shard,
  Leather,
  Paper,
  Boat,
  Bucket,
  Atlas,
  Wayshard,
  Meat,
  Steak,
  Flesh,
  // Produce and meals. Nine families cover every crop and every dish, because each
  // shades from ItemDef::color — a new crop is a table row, not a hand-drawn sprite.
  Grain,    // a tied bundle: wheat, barley, rice, maize
  Root,     // tapered, leafy-topped: carrot, potato, onion, beetroot, garlic
  Produce,  // round with a stem: pumpkin, melon, tomato
  Pod,      // long and curved: chili, soybean
  Berry,    // a small cluster: strawberry, blueberry, grapes
  Leafy,    // a veined head: cabbage
  Seed,     // the wild bootstrap
  Hoe,      // tills soil; mines nothing
  Bowl,     // soups and stews; contents take the colour, the bowl never does
  Plate,    // roasts and pies
  Pick,
  Axe,
  Shovel,
  Sword,
  ArmorHelmet,
  ArmorChest,
  ArmorLegs,
  ArmorBoots,
  Fallback,  // an unpainted kind draws a plain square, as the JS did
};

struct ItemDef {
  // Registration order, and the icon atlas cell. NOT the wire id, whatever this
  // comment used to say: WireSlot carries a `std::string key` (protocol.h) and
  // ItemStack stores a key too, so nothing on disk or on the wire remembers an
  // index. Inserting an item mid-list therefore costs a re-ordered icon sheet and
  // nothing else — unlike a block id, which every save contains.
  int index = 0;
  std::string key;
  std::string name;

  ItemType type = ItemType::Material;
  IconKind icon = IconKind::Fallback;
  int maxStack = 64;

  // The material colour every sprite painter shades from.
  std::uint32_t color = 0xccccccu;

  world::BlockId blockId = 0;  // type == Block

  // Food.
  int food = 0;      // hunger POINTS on the 20-point bar (1 pip = 2 points)
  bool risky = false;  // eating gambles: feeds or sickens
  // Saturation: the hidden buffer that drains before the visible bar does, and so
  // the real measure of how long a food lasts. This used to be derived as
  // `food * 0.6` for every item alike (player.cpp), which is exactly why a raw
  // carrot and a cooked meal could not be told apart by anything but size — a meal
  // that fills the same bar twice as fast is not a meal, it is a bigger carrot.
  // Authoring it per item is what lets a stew be *worth cooking*.
  float sat = 0.0f;
  // Which diet group eating this feeds. None for everything that is not food, and
  // also for the ones that genuinely feed nothing: rotten flesh is calories, not
  // nutrition.
  NutritionGroup group = NutritionGroup::None;
  // What this contributes as a cooking INGREDIENT, which is not the same question as
  // what it is worth to eat. Bread feeds you well and improves a stew barely at all;
  // a chili is the reverse. Summed over a pot's contents to pick which tier of a
  // recipe's output comes out.
  int quality = 0;

  // Bucket.
  std::string holds;            // "water" | "milk"; empty = the empty bucket
  bool hasFill = false;         // tints the icon's contents
  std::uint32_t fill = 0;

  // Tool / armour.
  ToolKind toolType = ToolKind::None;
  int tier = 0;
  float speed = 1.0f;   // mining multiplier, only with the matching block tool
  int durability = 0;   // 0 = does not wear
  int armorSlot = -1;   // 0 head, 1 chest, 2 legs, 3 feet
  int defense = 0;

  // Optional explicit viewmodel hold style; empty means "derive from the type".
  std::string hold;

  bool stackable() const { return maxStack > 1; }
};

// The tool and armour material tables. Exported because the recipe generator
// walks them, exactly as js/game/recipes.js imports TOOL_MATS / ARMOR_MATS.
struct ToolMaterial {
  const char* id;
  const char* name;
  int tier;
  const char* item;  // the crafting ingredient it is built from
  std::uint32_t color;
  float speed;
  int durability;
};
struct ArmorMaterial {
  const char* id;
  const char* name;
  std::uint32_t color;
  int defense;
  int durability;
};
struct ArmorPiece {
  const char* piece;
  const char* name;
  int slot;
  float mult;
};

const std::array<ToolMaterial, 6>& toolMaterials();
const std::array<ArmorMaterial, 4>& armorMaterials();
const std::array<ArmorPiece, 4>& armorPieces();

class ItemRegistry {
 public:
  static const ItemRegistry& get();

  const std::vector<ItemDef>& all() const { return items_; }
  std::size_t count() const { return items_.size(); }

  // Null for an unknown key, matching `ITEMS[key] || null`.
  const ItemDef* find(std::string_view key) const;
  const ItemDef* byIndex(int index) const {
    return index >= 0 && index < static_cast<int>(items_.size()) ? &items_[index] : nullptr;
  }
  int indexOf(std::string_view key) const;

  // The item a block drops as, for the mining path.
  const ItemDef* forBlock(world::BlockId id) const;

 private:
  ItemRegistry();

  std::vector<ItemDef> items_;
  std::unordered_map<std::string, int> byKey_;
  std::vector<int> byBlock_;  // block id -> item index, -1 for none
};

inline const ItemRegistry& items() { return ItemRegistry::get(); }
inline const ItemDef* getItem(std::string_view key) { return items().find(key); }

int maxDurability(std::string_view key);
bool isTool(std::string_view key);
bool isArmor(std::string_view key);

// Shared hover text: name plus the lines the inventory and the recipe book both
// show, so a tool's tier, an armour piece's defense and a stack's remaining
// durability read the same everywhere. `durabilityOverride` < 0 means "show the
// maximum"; `fuelSeconds` > 0 appends the forge-fuel line (callers pass it from
// crafting::fuelValue, which would otherwise be an include cycle).
struct Tooltip {
  std::string name;
  std::vector<std::string> lines;
};
Tooltip itemTooltip(std::string_view key, int durabilityOverride = -1,
                    float fuelSeconds = 0.0f);

// --- sprites ---------------------------------------------------------------

inline constexpr int kSpriteSize = 16;
// A texel counts as filled above this alpha. Shared with the mesh extruder so the
// model silhouette is exactly the icon silhouette.
inline constexpr std::uint8_t kSpriteAlphaCutoff = 128;

// A cell with alpha 0 is empty. No painter emits a transparent colour, so this is
// an exact stand-in for the JS grid's `null`.
using SpriteGrid = std::array<Rgba, kSpriteSize * kSpriteSize>;

// The raw, un-outlined sprite — the pixels the painter drew.
SpriteGrid spriteGridFor(const ItemDef& item);

// One pixel of dark rim in every empty 4-neighbour of a filled cell, so a pale
// sprite (iron tools, paper) still reads against a bright slot or a snowy world.
SpriteGrid outlined(const SpriteGrid& grid);

// The outlined sprite as an image, which is what goes into the atlas.
Image spriteImage(const ItemDef& item);

// `hollowreach:item/<key>` for every item that has a sprite. Block items are
// excluded: their appearance already comes from their block tiles.
std::vector<ResourceId> collectItemTextureIds();

// A PackStack provider serving those ids. Pushed below the built-in painters at
// startup; a user pack would go above both.
std::unique_ptr<resource::Provider> makeItemSpriteProvider();

}  // namespace hr::game
