// The rules of the palette, with no interface attached.
//
// Split out from ui/paletteui.cpp deliberately. Everything here is a question with a
// right answer — which dye does this colour cost, can the player afford it, what does
// the stack look like afterwards — and every one of those is testable headlessly. The
// screen's job is to draw a wheel and report where the mouse is; it should not also be
// the only place that knows what a dye costs.

#pragma once

#include <cstdint>
#include <string>

#include "game/inventory.h"

namespace hr::game {

// How many items one dye colours. The palette's slot is capped at this, which is the
// enforcement: you cannot present it with more than one dye's worth.
//
// Sixteen rather than a whole stack because of what the alternative costs a builder.
// At one dye per item a wall of two hundred wool wants two hundred flowers of one
// species, and nightcap only grows in snow — that is a scavenger hunt, not a build.
// At one dye per stack, dye stops being a cost at all after the first flower.
inline constexpr int kDyePerApplication = 16;

// The eight dyes, in the order they were registered. Held here rather than looked up
// by scanning the item table for IconKind::Dye, so "which colours can be mixed from"
// is a list somebody chose rather than a side effect of an enum.
struct DyeAnchor {
  const char* key;
  std::uint32_t rgb;
};
const DyeAnchor* dyeAnchors();
int dyeAnchorCount();

// The dye whose colour is nearest `rgb`, by squared distance in RGB.
//
// RGB rather than a perceptual space, and that is a real choice rather than laziness:
// the player is picking on an RGB wheel and reading an RGB hex code, so "nearest" has
// to mean nearest in the space they are looking at. A Lab metric would be more
// correct about human vision and would sometimes charge them blue for a colour that
// is plainly sitting on top of the green dab.
const DyeAnchor& nearestDye(std::uint32_t rgb);

// What one application would cost and whether it can be paid.
struct DyeCost {
  std::string dyeKey;      // the dye that would be spent
  std::uint32_t dyeRgb = 0;
  int have = 0;            // how many of it the player holds
  bool affordable = false;
};
DyeCost dyeCostFor(const Inventory& inv, std::uint32_t rgb);

// Applies `rgb` to `slot`, spending one dye. Returns false and changes nothing when
// the slot is empty, holds something undyeable, holds more than kDyePerApplication,
// or the dye cannot be paid for.
//
// One function so that "did it cost a dye" and "did the colour change" can never
// disagree — the bug where a refused application still recoloured the item, or a
// successful one was free, is not expressible.
bool applyDye(Inventory& inv, ItemStack& slot, std::uint32_t rgb);

// Whether the palette will accept this item at all.
bool isDyeable(const ItemStack& stack);

}  // namespace hr::game
