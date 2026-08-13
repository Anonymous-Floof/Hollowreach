#include "game/dyeing.h"

#include <algorithm>

#include "game/items.h"

namespace hr::game {
namespace {

// The same eight colours the item table gives these keys. Restated here rather than
// read out of ItemDef::color because they mean something different in each place: in
// items.cpp they are what the sprite is painted in, and here they are the anchors the
// whole 24-bit space is measured against. A test asserts the two agree, which is the
// honest way to keep a duplicated constant honest.
constexpr DyeAnchor kAnchors[] = {
    {"dye_red", 0xd23a34u},   {"dye_orange", 0xe8862au}, {"dye_yellow", 0xf2c53au},
    {"dye_green", 0x4fae53u}, {"dye_blue", 0x4a6fe0u},   {"dye_purple", 0x9a5ac2u},
    {"dye_white", 0xf0f0eau}, {"dye_black", 0x2a2333u},
};

int distance2(std::uint32_t a, std::uint32_t b) {
  const int dr = static_cast<int>((a >> 16) & 0xFF) - static_cast<int>((b >> 16) & 0xFF);
  const int dg = static_cast<int>((a >> 8) & 0xFF) - static_cast<int>((b >> 8) & 0xFF);
  const int db = static_cast<int>(a & 0xFF) - static_cast<int>(b & 0xFF);
  return dr * dr + dg * dg + db * db;
}

}  // namespace

const DyeAnchor* dyeAnchors() { return kAnchors; }
int dyeAnchorCount() { return static_cast<int>(std::size(kAnchors)); }

const DyeAnchor& nearestDye(std::uint32_t rgb) {
  rgb &= 0x00FFFFFFu;
  const DyeAnchor* best = &kAnchors[0];
  int bestD = distance2(rgb, kAnchors[0].rgb);
  for (int i = 1; i < dyeAnchorCount(); ++i) {
    const int d = distance2(rgb, kAnchors[i].rgb);
    // Strictly less, so a tie keeps the earlier anchor and the choice is stable
    // rather than depending on table order changing under it.
    if (d < bestD) {
      bestD = d;
      best = &kAnchors[i];
    }
  }
  return *best;
}

bool isDyeable(const ItemStack& stack) {
  if (stack.empty()) return false;
  const ItemDef* def = getItem(stack.key);
  return def != nullptr && def->dyeable;
}

DyeCost dyeCostFor(const Inventory& inv, std::uint32_t rgb) {
  const DyeAnchor& anchor = nearestDye(rgb);
  DyeCost cost;
  cost.dyeKey = anchor.key;
  cost.dyeRgb = anchor.rgb;
  cost.have = inv.countOf(anchor.key);
  cost.affordable = cost.have > 0;
  return cost;
}

std::array<std::int32_t, 4> armourTints(const Inventory& inv) {
  std::array<std::int32_t, 4> out {-1, -1, -1, -1};
  const auto& worn = inv.armor();
  for (std::size_t i = 0; i < worn.size() && i < out.size(); ++i) {
    // Read through the item definition rather than trusting the slot index, because
    // the two are not the same thing: armor()[i] is where the player put it, and
    // armorSlot is where the piece belongs. They agree today because the UI enforces
    // it, and a renderer that assumed it would draw a helmet on the feet the first
    // time anything else fills these slots.
    if (worn[i].empty() || !worn[i].dyed()) continue;
    const ItemDef* def = getItem(worn[i].key);
    if (def == nullptr || def->armorSlot < 0 ||
        def->armorSlot >= static_cast<int>(out.size())) {
      continue;
    }
    out[static_cast<std::size_t>(def->armorSlot)] = worn[i].tint;
  }
  return out;
}

bool applyDye(Inventory& inv, ItemStack& slot, std::uint32_t rgb) {
  rgb &= 0x00FFFFFFu;
  if (!isDyeable(slot)) return false;
  // The cap is the whole enforcement of "one dye colours sixteen". The slot is
  // *also* limited to this in the interface, but checking here too is what makes the
  // rule true rather than merely displayed — a shift-click that overfilled the slot
  // would otherwise dye sixty-four for one dye.
  if (slot.count > kDyePerApplication) return false;

  const DyeCost cost = dyeCostFor(inv, rgb);
  if (!cost.affordable) return false;

  // Spend first, then colour. If taking the dye somehow fails there must be no way
  // to end up with a coloured item and a full bag of dye.
  inv.removeItems({{cost.dyeKey, 1}});
  slot.tint = static_cast<std::int32_t>(rgb);
  return true;
}

}  // namespace hr::game
