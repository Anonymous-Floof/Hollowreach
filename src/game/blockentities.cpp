#include "game/blockentities.h"

#include "game/crafting.h"

namespace hr::game {

BlockEntity makeForge() {
  BlockEntity be;
  be.kind = BlockEntityKind::Forge;
  return be;
}

BlockEntity makeChest() {
  BlockEntity be;
  be.kind = BlockEntityKind::Chest;
  be.slots.resize(kChestSlots);
  return be;
}

BlockEntityKind entityKindFor(std::string_view blockKey) {
  if (blockKey == "forge") return BlockEntityKind::Forge;
  if (blockKey == "chest") return BlockEntityKind::Chest;
  return BlockEntityKind::None;
}

BlockEntity makeEntity(BlockEntityKind kind) {
  switch (kind) {
    case BlockEntityKind::Forge: return makeForge();
    case BlockEntityKind::Chest: return makeChest();
    case BlockEntityKind::None: break;
  }
  return {};
}

void tickForge(BlockEntity& f, float dt) {
  const SmeltingRecipe* smelt = f.input.empty() ? nullptr : smeltingFor(f.input.key);
  const bool canOut =
      smelt != nullptr && (f.output.empty() || (f.output.key == smelt->out && f.output.count < 64));
  if (!canOut) {
    f.progress = 0;
    if (f.fuelLeft <= 0) f.fuelMax = 0;
    return;
  }

  if (f.fuelLeft <= 0 && !f.fuel.empty()) {
    const float v = fuelValue(f.fuel.key);
    if (v > 0) {
      f.fuelLeft += v;
      f.fuelMax = v;
      f.fuel.count--;
      if (f.fuel.count <= 0) f.fuel.clear();
    }
  }

  if (f.fuelLeft > 0) {
    f.fuelLeft -= dt;
    f.progress += dt;
    if (f.progress >= smelt->seconds) {
      f.progress = 0;
      if (f.output.empty()) {
        f.output = ItemStack {smelt->out, 1, -1};
      } else {
        f.output.count++;
      }
      f.input.count--;
      if (f.input.count <= 0) f.input.clear();
    }
  } else {
    f.progress = 0;
  }
}

std::vector<ItemStack> entityContents(const BlockEntity& be) {
  std::vector<ItemStack> out;
  if (be.kind == BlockEntityKind::Forge) {
    for (const ItemStack* s : {&be.input, &be.fuel, &be.output}) {
      if (!s->empty()) out.push_back(*s);
    }
  } else if (be.kind == BlockEntityKind::Chest) {
    for (const ItemStack& s : be.slots) {
      if (!s.empty()) out.push_back(s);
    }
  }
  return out;
}

}  // namespace hr::game
