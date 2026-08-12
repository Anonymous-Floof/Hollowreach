// Block entities, ported from js/game/blockentities.js.
//
// Per-position state that lives in the World rather than the voxel array, so it
// survives a chunk unload and reload and is saved separately. Forges keep smelting
// while their interface is closed; chests store items. Both spill their contents
// when the block is mined.
//
// This header sits under game/ and is included by world/world.h, which is the same
// direction the web build's import went (js/world/world.js:12). The dependency is
// one-way and header-light — only ItemStack crosses — so the layering stays honest.

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "game/inventory.h"

namespace hr::game {

inline constexpr int kChestSlots = 27;

// APPEND ONLY. The raw value is on disk and on the wire, so inserting a kind would
// turn every saved chest into whatever now sits at its number.
//
// The three kitchen kinds are saved in a section of their OWN rather than alongside
// Forge and Chest — see save/format.h. decodeBlockEntities cannot skip a kind it
// does not know, because entity payloads carry no length, so adding one to that
// section would make an older build lose every entity sorted after the first cooker.
enum class BlockEntityKind : std::uint8_t {
  None = 0,
  Forge,
  Chest,
  Cutting,  // prep: one in, one out, no fuel
  Stove,    // one in, fuel, one out
  Pot,      // several in, a bowl, fuel, one out
};

// How many ingredient slots a cooking pot holds. Six is enough for the largest
// recipe with room to spare, and few enough that the window stays one row.
inline constexpr int kPotSlots = 6;

// And how many the stove holds. It shipped in the 2.14.0 draft with ONE, which is
// what "cooks one thing at a time" was taken to mean — but five of its thirteen
// recipes name two or three different ingredients (bread and garlic, flour and
// vegetables, a stuffed pumpkin), and a station with one slot cannot hold two things
// however long you stare at it. Those five could never be cooked.
//
// Three is the largest stove recipe, and it keeps the distinction that actually
// matters: the stove bakes dry and serves nothing, the pot simmers six things into a
// bowl. "One at a time" is about throughput, not about how many things go into a pie.
inline constexpr int kStoveSlots = 3;

// Ingredient slots per station. Zero for the cutting board, which takes a single
// item through `input` the way the forge does.
inline constexpr int cookSlotsFor(BlockEntityKind kind) {
  return kind == BlockEntityKind::Pot     ? kPotSlots
         : kind == BlockEntityKind::Stove ? kStoveSlots
                                          : 0;
}

// Is this kind one of the kitchen stations? Used by the save layer to decide which
// section an entity belongs in, so the test is written once.
inline bool isKitchen(BlockEntityKind kind) {
  return kind == BlockEntityKind::Cutting || kind == BlockEntityKind::Stove ||
         kind == BlockEntityKind::Pot;
}

struct BlockEntity {
  BlockEntityKind kind = BlockEntityKind::None;

  // Forge.
  ItemStack input, fuel, output;
  float fuelLeft = 0.0f;  // seconds of burn remaining
  float fuelMax = 0.0f;   // what the current fuel item was worth, for the gauge
  float progress = 0.0f;  // seconds into the current smelt

  // Chest.
  std::vector<ItemStack> slots;

  // --- kitchen --------------------------------------------------------------
  //
  // The pot's and the stove's ingredients live in `slots` (sized by cookSlotsFor) so
  // they reuse the same container plumbing a chest already has; the cutting board
  // uses `input` and `output` exactly as the forge does, and both cookers reuse
  // `fuel`, `fuelLeft` and `fuelMax` too. Only the bowl needs a field of its own.
  ItemStack container;
  // Which cooking recipe is in progress, or -1. Held as an index rather than
  // re-matched every tick: a pot holding six ingredients would otherwise re-run the
  // whole match on every frame of a ten-second cook.
  int recipe = -1;
};

BlockEntity makeForge();
BlockEntity makeChest();
BlockEntity makeCutting();
BlockEntity makeStove();
BlockEntity makePot();

// Advances one kitchen station by dt seconds. Like tickForge, a pure state machine
// over its own slots that runs whether or not anyone is looking at it.
void tickKitchen(BlockEntity& station, float dt);

// Which block keys carry which block entity.
BlockEntityKind entityKindFor(std::string_view blockKey);
BlockEntity makeEntity(BlockEntityKind kind);

// Advances one forge by dt seconds. A pure state machine over its three slots,
// run every frame whether or not anyone is looking at it.
void tickForge(BlockEntity& forge, float dt);

// Every stack a block entity holds, for spilling on break.
std::vector<ItemStack> entityContents(const BlockEntity& be);

// A world position packed into one key. 28 bits each of x and z (±134M blocks)
// and 8 of y, which is comfortably past the 128-block world height.
using BlockEntityKey = std::uint64_t;
inline BlockEntityKey blockEntityKey(int x, int y, int z) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x) & 0xFFFFFFFu) << 36) |
         (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & 0xFFFFFFFu) << 8) |
         static_cast<std::uint64_t>(static_cast<std::uint32_t>(y) & 0xFFu);
}

// The inverse. The web build could iterate its Map's `"x,y,z"` keys and split them
// back apart; packing means unpacking has to be written down. x and z are two's
// complement in 28 bits, so their sign bit has to be extended by hand.
inline void unpackBlockEntityKey(BlockEntityKey key, int& x, int& y, int& z) {
  auto sx = static_cast<std::int32_t>((key >> 36) & 0xFFFFFFFu);
  auto sz = static_cast<std::int32_t>((key >> 8) & 0xFFFFFFFu);
  if (sx & 0x8000000) sx -= 0x10000000;
  if (sz & 0x8000000) sz -= 0x10000000;
  x = sx;
  z = sz;
  y = static_cast<int>(key & 0xFFu);
}

}  // namespace hr::game
