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

BlockEntity makeCutting() {
  BlockEntity be;
  be.kind = BlockEntityKind::Cutting;
  return be;
}

BlockEntity makeStove() {
  BlockEntity be;
  be.kind = BlockEntityKind::Stove;
  be.slots.resize(kStoveSlots);
  return be;
}

BlockEntity makePot() {
  BlockEntity be;
  be.kind = BlockEntityKind::Pot;
  be.slots.resize(kPotSlots);
  return be;
}

BlockEntityKind entityKindFor(std::string_view blockKey) {
  if (blockKey == "forge") return BlockEntityKind::Forge;
  if (blockKey == "chest") return BlockEntityKind::Chest;
  if (blockKey == "cutting_board") return BlockEntityKind::Cutting;
  if (blockKey == "stove") return BlockEntityKind::Stove;
  if (blockKey == "cooking_pot") return BlockEntityKind::Pot;
  return BlockEntityKind::None;
}

BlockEntity makeEntity(BlockEntityKind kind) {
  switch (kind) {
    case BlockEntityKind::Forge: return makeForge();
    case BlockEntityKind::Chest: return makeChest();
    case BlockEntityKind::Cutting: return makeCutting();
    case BlockEntityKind::Stove: return makeStove();
    case BlockEntityKind::Pot: return makePot();
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

namespace {

Kitchen kitchenFor(BlockEntityKind kind) {
  switch (kind) {
    case BlockEntityKind::Cutting: return Kitchen::Cutting;
    case BlockEntityKind::Stove: return Kitchen::Stove;
    default: break;
  }
  return Kitchen::Pot;
}

}  // namespace

void tickKitchen(BlockEntity& s, float dt) {
  // The pot and the stove cook out of `slots`; the board out of `input`. Building one
  // ingredient view here rather than branching three ways below is what lets the state
  // machine itself be identical for all three — which is the point of them sharing a
  // struct at all.
  //
  // Asking whether `slots` is empty rather than naming the kinds is deliberate: it is
  // the same question the save and the wire ask, so a station cannot end up with a
  // tray the cooker refuses to read. That mismatch is how the stove lost five recipes.
  std::vector<ItemStack> offered;
  if (s.slots.empty()) {
    offered.push_back(s.input);
  } else {
    offered = s.slots;
  }

  const CookMatch m = matchCooking(kitchenFor(s.kind), offered, s.container);
  const bool canOut =
      m.recipe >= 0 &&
      (s.output.empty() || (s.output.key == m.out && s.output.count + m.outCount <= 64));
  if (!canOut) {
    s.progress = 0;
    s.recipe = -1;
    if (s.fuelLeft <= 0) s.fuelMax = 0;
    return;
  }
  s.recipe = m.recipe;

  // The cutting board burns nothing. It is prep rather than cooking, and needing
  // fuel to slice a cabbage would make the first station in the chain the most
  // tedious one.
  if (s.kind != BlockEntityKind::Cutting) {
    if (s.fuelLeft <= 0 && !s.fuel.empty()) {
      const float v = fuelValue(s.fuel.key);
      if (v > 0) {
        s.fuelLeft += v;
        s.fuelMax = v;
        s.fuel.count--;
        if (s.fuel.count <= 0) s.fuel.clear();
      }
    }
    if (s.fuelLeft <= 0) {
      s.progress = 0;
      return;
    }
    s.fuelLeft -= dt;
  }

  s.progress += dt;
  if (s.progress < m.seconds) return;
  s.progress = 0;

  // Consume out of whichever view was offered, and by the same test, so the station
  // can never take from a place it did not look at.
  const CookingRecipe& r = recipeBook().cooking()[static_cast<std::size_t>(m.recipe)];
  if (s.slots.empty()) {
    std::vector<ItemStack> one {s.input};
    consumeCooking(r, one, s.container);
    s.input = one[0];
  } else {
    consumeCooking(r, s.slots, s.container);
  }

  if (s.output.empty()) {
    s.output = ItemStack {m.out, m.outCount, -1};
  } else {
    s.output.count += m.outCount;
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
  } else if (isKitchen(be.kind)) {
    // The pot keeps its ingredients in `slots` and everything keeps its container,
    // so a station broken mid-cook gives back everything that went into it. Losing a
    // bowl and six vegetables to a misplaced pickaxe would be a bad way to learn
    // where the station was.
    for (const ItemStack* s : {&be.input, &be.fuel, &be.output, &be.container}) {
      if (!s->empty()) out.push_back(*s);
    }
    for (const ItemStack& s : be.slots) {
      if (!s.empty()) out.push_back(s);
    }
  }
  return out;
}

}  // namespace hr::game
