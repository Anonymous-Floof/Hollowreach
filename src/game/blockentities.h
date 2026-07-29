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

enum class BlockEntityKind : std::uint8_t { None = 0, Forge, Chest };

struct BlockEntity {
  BlockEntityKind kind = BlockEntityKind::None;

  // Forge.
  ItemStack input, fuel, output;
  float fuelLeft = 0.0f;  // seconds of burn remaining
  float fuelMax = 0.0f;   // what the current fuel item was worth, for the gauge
  float progress = 0.0f;  // seconds into the current smelt

  // Chest.
  std::vector<ItemStack> slots;
};

BlockEntity makeForge();
BlockEntity makeChest();

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
