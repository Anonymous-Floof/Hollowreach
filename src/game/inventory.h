// Player inventory, ported from js/game/inventory.js.
//
// 9 hotbar slots + 27 main slots + 4 armour slots. A slot is empty or holds a
// key, a count, and — for tools and armour, which never stack — a remaining
// durability.

#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace hr::game {

inline constexpr int kHotbarSlots = 9;
inline constexpr int kMainSlots = 27;
inline constexpr int kInventorySlots = kHotbarSlots + kMainSlots;
inline constexpr int kArmorSlots = 4;

// The JS used `null` for an empty slot and `undefined` for "no durability". Here
// an empty key is the empty slot and a negative durability means the item does
// not wear — checked rather than assumed, because a stack of 64 planks and a
// pickaxe both live in this type.
struct ItemStack {
  std::string key;
  int count = 0;
  int dura = -1;
  // 0xRRGGBB, or -1 for undyed. THE ONLY PER-INSTANCE STATE IN THE GAME, and it is
  // deliberately a whole 24 bits rather than an index into a palette: the Dye update
  // promises any colour, and a palette would cap the world at however many entries
  // the index could hold.
  //
  // It is a field of its own rather than a reuse of `dura`, which is signed and free
  // on everything that does not wear: armour is dyeable AND wears, so the two would
  // have collided on exactly the item the feature was asked for.
  std::int32_t tint = -1;

  bool empty() const { return key.empty() || count <= 0; }
  void clear() {
    key.clear();
    count = 0;
    dura = -1;
    tint = -1;
  }
  bool wears() const { return dura >= 0; }
  bool dyed() const { return tint >= 0; }
};

// Whether two stacks may occupy one slot. Same item is no longer sufficient — a red
// wool and a blue wool are not interchangeable, and merging them silently destroys
// one of the two colours the player paid a dye for.
//
// Written once and called from every merge site on purpose. The sites are
// Inventory::give, the inventory screen's drop-onto-a-slot and its shift-click
// sweep, and the pickup path; four copies of `&& a.tint == b.tint` is four chances
// to leave one out, and the one left out is a colour quietly going missing rather
// than anything that looks like a bug.
inline bool canMerge(const ItemStack& a, const ItemStack& b) {
  return a.key == b.key && a.tint == b.tint;
}

class Inventory {
 public:
  Inventory() = default;

  std::array<ItemStack, kInventorySlots>& slots() { return slots_; }
  const std::array<ItemStack, kInventorySlots>& slots() const { return slots_; }
  std::array<ItemStack, kArmorSlots>& armor() { return armor_; }
  const std::array<ItemStack, kArmorSlots>& armor() const { return armor_; }

  int selected() const { return selected_; }
  void setSelected(int i) { selected_ = ((i % kHotbarSlots) + kHotbarSlots) % kHotbarSlots; }
  // Wheel / hotbar cycling, wrapping in both directions.
  void cycleSelected(int delta) { setSelected(selected_ + delta); }

  ItemStack& selectedSlot() { return slots_[selected_]; }
  const ItemStack& selectedSlot() const { return slots_[selected_]; }

  int stackMax(const std::string& key) const;
  bool stackable(const std::string& key) const { return stackMax(key) > 1; }

  // Adds up to `count`. Returns the number that did NOT fit, so the caller can
  // spill the remainder as a drop. `dura` < 0 means "use the item's maximum",
  // which is what a freshly crafted or mined tool wants. `tint` < 0 is undyed.
  //
  // `tint` is defaulted rather than threaded through the thirty existing call sites:
  // every one of them hands over something the player just crafted, mined or was
  // given by a command, and none of those is dyed. The callers that DO carry a
  // colour — picking a dyed drop back up, emptying the palette — take the overload
  // below, which cannot forget the field.
  int give(const std::string& key, int count = 1, int dura = -1, std::int32_t tint = -1);
  int give(const ItemStack& s) { return give(s.key, s.count, s.dura, s.tint); }

  int firstEmpty() const;
  int countOf(const std::string& key) const;
  bool hasItems(const std::unordered_map<std::string, int>& need) const;
  // Removes per key. Assumes hasItems() already passed.
  void removeItems(const std::unordered_map<std::string, int>& take);

  // Consumes one of the selected stack (placing a block, eating).
  void consumeSelected();

  // Wears the selected tool. Returns true if it broke.
  bool damageSelectedTool(int amount = 1);

  int totalDefense() const;
  // Wears every worn piece; a piece at 0 durability breaks.
  void damageArmor(int amount = 1);

  void clear();

 private:
  std::array<ItemStack, kInventorySlots> slots_ {};
  std::array<ItemStack, kArmorSlots> armor_ {};
  int selected_ = 0;
};

}  // namespace hr::game
