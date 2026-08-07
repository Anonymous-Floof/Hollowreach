// Loot tables: what a generated container is holding when you first open it.
//
// The only reward table the game had before this was `BlockDef::drop` — one item
// key and a fixed count, which is all a mined block ever needs. A chest in a
// dungeon needs the other thing: several rolls over a weighted list, counts in a
// range, and the same answer every time for a given chest.
//
// ---------------------------------------------------------------------------
// Tables are looked up by NAME, and that is a deliberate departure.
//
// Recipes are addressed by index, and `game/recipes.cpp` carries a warning about
// what that costs: the golden vectors compare recipes by position, so inserting one
// in the middle renumbers a hundred and nineteen recipes that did not change, and
// the diff becomes unreadable. Blocks have the same problem for the same reason.
// Both are stuck with it, because a block id is in every save and a recipe's
// position is its match priority.
//
// A loot table has neither excuse. Nothing persists a table's index and nothing
// resolves ties by order, so keying them by name costs nothing and means a new
// table can be written anywhere in the list, in whatever order reads best, without
// moving anything else. The dump is sorted by name for the same reason.
//
// ---------------------------------------------------------------------------
// Rolling is a pure function of the world seed and the container's position.
//
// Two consequences worth stating, because both are easy to break later:
//
//  * `randomUnit()` must never appear here. Terrain is regenerated locally by every
//    machine in a multiplayer world, so anything derived from it has to come out of
//    the same arithmetic everywhere — see the note on randomUnit in core/prng.h.
//    Positional hashes are the whole reason worldgen can run on any thread in any
//    order and still agree.
//  * Secondary rolls must move the coordinates, not just the salt. `hash2i` and
//    `hash3i` barely avalanche a single flipped seed bit, so two rolls at one
//    position with different salts come out correlated — which would tie "how many"
//    to "which item" and make the tail of every table unreachable. Every roll below
//    offsets its coordinates by a distinct prime-ish step.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "game/inventory.h"

namespace hr::game {

// One thing a pool can produce, and how likely it is relative to its siblings.
struct LootEntry {
  std::string key;
  int minCount = 1;
  int maxCount = 1;
  // Relative, not a percentage: a pool's chances are each entry's weight over the
  // sum of the pool's weights, so entries can be added without rebalancing the rest.
  int weight = 1;
};

// A pool rolls somewhere between minRolls and maxRolls times, each roll picking one
// entry by weight. Separate pools let a table say "always some stone, and then one
// treasure" without either half being able to crowd the other out.
struct LootPool {
  int minRolls = 1;
  int maxRolls = 1;
  std::vector<LootEntry> entries;

  int totalWeight() const;
};

struct LootTable {
  std::string id;
  std::vector<LootPool> pools;
};

// Built once at startup and read-only thereafter, exactly like RecipeBook.
class LootBook {
 public:
  static const LootBook& get();

  // Null for a name nobody registered. Callers are expected to check: a container
  // asking for a table that does not exist should end up empty rather than crash,
  // and an empty chest in a dungeon is a disappointment, not a corruption.
  const LootTable* find(std::string_view id) const;

  // Every table, sorted by name. For the golden dump, and so the order cannot
  // depend on how an unordered_map happened to bucket things this build.
  std::vector<const LootTable*> sorted() const;

 private:
  LootBook();
  std::unordered_map<std::string, LootTable> tables_;
};

inline const LootBook& lootBook() { return LootBook::get(); }

// Rolls a table for a container at a world position. Same seed and same position
// always give the same result; adjacent positions give unrelated ones.
//
// Stacks come back ready to store: a tool or a piece of armour arrives with its
// full durability already set, which writing straight into BlockEntity::slots would
// otherwise skip — Inventory::give is what normally fills that in, and a generated
// chest does not go through it.
std::vector<ItemStack> rollLoot(const LootTable& table, std::uint32_t seed, int x, int y, int z);

// Convenience: looks the table up and rolls it, or returns nothing.
std::vector<ItemStack> rollLoot(std::string_view tableId, std::uint32_t seed, int x, int y, int z);

}  // namespace hr::game
