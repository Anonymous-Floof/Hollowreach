#include "ui/inventoryui.h"

#include <algorithm>
#include <cmath>

#include "audio/sfx.h"
#include "game/crafting.h"
#include "game/items.h"

namespace hr::ui {
namespace {

// Node tags. The DOM only needs to say "this is a slot"; which slot comes from the
// container and index it carries.
constexpr int kTagSlot = 1;
// Well clear of the container tags, which occupy kTagSlot*100 .. +100.
constexpr int kTagBook = 9001;

// One tag value per container, so a node's tag/index pair round-trips back to a SlotId.
int containerTag(Container c) { return kTagSlot * 100 + static_cast<int>(c); }
Container tagContainer(int tag) { return static_cast<Container>(tag - kTagSlot * 100); }
bool isSlotTag(int tag) { return tag >= kTagSlot * 100 && tag < kTagSlot * 100 + 100; }

std::vector<int> range(int a, int b) {
  std::vector<int> out;
  out.reserve(static_cast<std::size_t>(std::max(0, b - a)));
  for (int i = a; i < b; ++i) out.push_back(i);
  return out;
}

}  // namespace

void InventoryUI::attach(game::Inventory* inventory, const render::IconAtlas* icons) {
  inv_ = inventory;
  icons_ = icons;
}

void InventoryUI::open(InventoryMode mode, game::BlockEntity* station) {
  mode_ = mode;
  station_ = station;
  if (mode == InventoryMode::Inventory || mode == InventoryMode::Workbench) {
    craftSize_ = mode == InventoryMode::Workbench ? 3 : 2;
    craft_.assign(static_cast<std::size_t>(craftSize_ * craftSize_), game::ItemStack {});
  }
  dragging_ = false;
  drag_.cells.clear();
}

void InventoryUI::close() {
  if (!inv_) {
    mode_ = InventoryMode::Closed;
    return;
  }
  if (!cursor_.empty()) {
    inv_->give(cursor_.key, cursor_.count, cursor_.dura);
    cursor_.clear();
  }
  if (mode_ == InventoryMode::Inventory || mode_ == InventoryMode::Workbench) {
    for (game::ItemStack& s : craft_) {
      if (!s.empty()) inv_->give(s.key, s.count, s.dura);
    }
    craft_.clear();
  }
  mode_ = InventoryMode::Closed;
  station_ = nullptr;
  dragging_ = false;
  drag_.cells.clear();
  haveHover_ = false;
}

InventoryUI::FillResult InventoryUI::autoFill(const game::Recipe& recipe) {
  if (!inv_ || (mode_ != InventoryMode::Inventory && mode_ != InventoryMode::Workbench)) {
    return FillResult::NoGrid;
  }
  // A hand recipe works at a bench too; a bench recipe does not work in the 2x2.
  if (recipe.station == game::CraftStation::Workbench &&
      stationKind() != game::CraftStation::Workbench) {
    return FillResult::TooBig;
  }
  if (recipe.type == game::RecipeType::Shaped &&
      (recipe.width > craftSize_ || recipe.height > craftSize_)) {
    return FillResult::TooBig;
  }

  // Back into the bag first, so a second click replaces the first rather than
  // finding every cell occupied. Done before anything is taken, which also means
  // ingredients already laid out are available to the new recipe.
  for (game::ItemStack& s : craft_) {
    if (!s.empty()) inv_->give(s.key, s.count, s.dura);
    s.clear();
  }

  // A tag ingredient resolves against what the player actually has. Least-stocked
  // first, so laying out a plank recipe spends the odd single birch plank rather
  // than opening the stack of sixty oak you were saving.
  const auto pickFor = [&](const std::string& required) -> std::string {
    if (required.empty() || required.front() != '#') {
      return inv_->countOf(required) > 0 ? required : std::string {};
    }
    const std::vector<std::string>* members = world::blocks().tag(required.substr(1));
    if (!members) return {};
    std::string best;
    int bestCount = 0;
    for (const std::string& m : *members) {
      const int have = inv_->countOf(m);
      if (have > 0 && (best.empty() || have < bestCount)) {
        best = m;
        bestCount = have;
      }
    }
    return best;
  };

  // What each cell wants, in grid order.
  std::vector<std::pair<int, std::string>> want;  // cell index -> required key
  if (recipe.type == game::RecipeType::Shaped) {
    for (const game::RecipeCell& c : recipe.cells) {
      want.push_back({c.row * craftSize_ + c.col, c.key});
    }
  } else {
    int cell = 0;
    for (const auto& [key, count] : recipe.ingredients) {
      for (int n = 0; n < std::max(1, count); ++n) {
        if (cell >= craftSize_ * craftSize_) return FillResult::TooBig;
        want.push_back({cell++, key});
      }
    }
  }

  // Take everything before placing anything, so a shortfall discovered on the last
  // cell does not leave the first five sitting in the grid.
  std::vector<std::pair<int, game::ItemStack>> taken;
  bool ok = true;
  for (const auto& [cell, required] : want) {
    const std::string key = pickFor(required);
    if (key.empty()) {
      ok = false;
      break;
    }
    std::unordered_map<std::string, int> one {{key, 1}};
    if (!inv_->hasItems(one)) {
      ok = false;
      break;
    }
    inv_->removeItems(one);
    taken.push_back({cell, game::ItemStack{key, 1, -1}});
  }

  if (!ok) {
    for (auto& [cell, stack] : taken) inv_->give(stack.key, stack.count, stack.dura);
    return FillResult::Missing;
  }
  for (auto& [cell, stack] : taken) {
    if (cell >= 0 && cell < static_cast<int>(craft_.size())) craft_[cell] = std::move(stack);
  }
  return FillResult::Ok;
}

game::CraftStation InventoryUI::stationKind() const {
  return mode_ == InventoryMode::Workbench ? game::CraftStation::Workbench
                                           : game::CraftStation::Hand;
}

// ---------------------------------------------------------------------------
// Container access
// ---------------------------------------------------------------------------

game::ItemStack* InventoryUI::slotAt(SlotId id) {
  switch (id.container) {
    case Container::Inv:
      if (!inv_ || id.index < 0 || id.index >= game::kInventorySlots) return nullptr;
      return &inv_->slots()[static_cast<std::size_t>(id.index)];
    case Container::Armor:
      if (!inv_ || id.index < 0 || id.index >= game::kArmorSlots) return nullptr;
      return &inv_->armor()[static_cast<std::size_t>(id.index)];
    case Container::Craft:
      if (id.index < 0 || id.index >= static_cast<int>(craft_.size())) return nullptr;
      return &craft_[static_cast<std::size_t>(id.index)];
    case Container::Chest:
      if (!station_ || id.index < 0 || id.index >= static_cast<int>(station_->slots.size())) {
        return nullptr;
      }
      return &station_->slots[static_cast<std::size_t>(id.index)];
    case Container::ForgeIn:
      return station_ ? &station_->input : nullptr;
    case Container::ForgeFuel:
      return station_ ? &station_->fuel : nullptr;
    case Container::ForgeOut:
      return station_ ? &station_->output : nullptr;
    case Container::CookIn:
      return station_ ? &station_->input : nullptr;
    case Container::CookSlots:
      if (!station_ || id.index < 0 || id.index >= static_cast<int>(station_->slots.size())) {
        return nullptr;
      }
      return &station_->slots[static_cast<std::size_t>(id.index)];
    case Container::CookContainer:
      return station_ ? &station_->container : nullptr;
    case Container::CookFuel:
      return station_ ? &station_->fuel : nullptr;
    case Container::CookOut:
      return station_ ? &station_->output : nullptr;
    case Container::Result:
    case Container::None:
      return nullptr;
  }
  return nullptr;
}

const game::ItemStack* InventoryUI::slotAt(SlotId id) const {
  return const_cast<InventoryUI*>(this)->slotAt(id);
}

bool InventoryUI::accepts(SlotId id, const std::string& key) const {
  if (id.container != Container::Armor) return true;
  const game::ItemDef* item = game::getItem(key);
  return item && item->type == game::ItemType::Armor && item->armorSlot == id.index;
}

int InventoryUI::stackMax(const std::string& key) const {
  return inv_ ? inv_->stackMax(key) : 1;
}

bool InventoryUI::stackable(const std::string& key) const { return stackMax(key) > 1; }

// ---------------------------------------------------------------------------
// Click handling (js/ui/inventoryui.js:155-206)
// ---------------------------------------------------------------------------

void InventoryUI::clickSlot(SlotId id, int button) {
  if (id.container == Container::Result) {
    takeResult();
    return;
  }
  game::ItemStack* slot = slotAt(id);
  if (!slot) return;

  // Tactile tick when a stack moves. Clicking an empty slot with an empty cursor
  // is silent, which is what stops a drag across the bag from rattling.
  if (!slot->empty() || !cursor_.empty()) audio::sfx::uiSlot();

  if (isOutput(id.container)) {
    // An output slot is take-only: nothing can be placed in it, and a partial stack
    // on the cursor can only top itself up from it.
    if (!slot->empty() && (cursor_.empty() || (cursor_.key == slot->key &&
                                               stackable(cursor_.key)))) {
      if (cursor_.empty()) {
        cursor_ = *slot;
        slot->clear();
      } else {
        const int add = std::min(stackMax(cursor_.key) - cursor_.count, slot->count);
        cursor_.count += add;
        slot->count -= add;
        if (slot->count <= 0) slot->clear();
      }
    }
    return;
  }

  if (button == 1) {
    // Right click: take half, or place one.
    if (cursor_.empty()) {
      if (!slot->empty()) {
        const int half = (slot->count + 1) / 2;  // Math.ceil(count / 2)
        cursor_ = {slot->key, half, slot->dura};
        slot->count -= half;
        if (slot->count <= 0) slot->clear();
      }
    } else {
      if (!accepts(id, cursor_.key)) return;
      if (slot->empty()) {
        *slot = {cursor_.key, 1, cursor_.dura};
        if (--cursor_.count <= 0) cursor_.clear();
      } else if (slot->key == cursor_.key && stackable(cursor_.key) &&
                 slot->count < stackMax(cursor_.key)) {
        ++slot->count;
        if (--cursor_.count <= 0) cursor_.clear();
      }
    }
    return;
  }

  // Left click: pick up, place, merge or swap. The branch order matters — merging
  // must be tested before swapping, or two matching stacks would swap instead.
  if (cursor_.empty()) {
    if (!slot->empty()) {
      cursor_ = *slot;
      slot->clear();
    }
  } else if (!accepts(id, cursor_.key)) {
    // Cannot place here; the cursor keeps its stack.
  } else if (slot->empty()) {
    *slot = cursor_;
    cursor_.clear();
  } else if (slot->key == cursor_.key && stackable(cursor_.key)) {
    const int add = std::min(stackMax(cursor_.key) - slot->count, cursor_.count);
    slot->count += add;
    cursor_.count -= add;
    if (cursor_.count <= 0) cursor_.clear();
  } else {
    std::swap(*slot, cursor_);
  }
}

void InventoryUI::takeResult() {
  const game::CraftMatch match = game::matchGrid(craft_, craftSize_, stationKind());
  if (!match) return;
  const game::ItemDef* item = game::getItem(match.outKey);
  if (!cursor_.empty()) {
    if (cursor_.key != match.outKey || !stackable(match.outKey)) return;
    if (cursor_.count + match.outCount > stackMax(match.outKey)) return;
  }
  game::consumeGrid(craft_, craftSize_, *match.recipe);
  audio::sfx::craft();
  if (cursor_.empty()) {
    cursor_ = {match.outKey, match.outCount, -1};
    if (item && (item->type == game::ItemType::Tool || item->type == game::ItemType::Armor)) {
      cursor_.dura = item->durability;
    }
  } else {
    cursor_.count += match.outCount;
  }
}

// ---------------------------------------------------------------------------
// Mouse Tweaks: shift-click quick move
// ---------------------------------------------------------------------------

std::vector<InventoryUI::Target> InventoryUI::toInventoryTargets() const {
  // Main storage first, then the hotbar — so a shift-click out of a chest fills the
  // bag before it disturbs what you are holding.
  std::vector<int> indices = range(9, 36);
  const std::vector<int> hotbar = range(0, 9);
  indices.insert(indices.end(), hotbar.begin(), hotbar.end());
  return {{Container::Inv, std::move(indices)}};
}

bool InventoryUI::invSourceTargets(const game::ItemStack& stack, int index,
                                   std::vector<Target>& out) const {
  const game::ItemDef* item = game::getItem(stack.key);
  if (mode_ == InventoryMode::Chest) {
    out = {{Container::Chest, range(0, game::kChestSlots)}};
    return true;
  }
  if (mode_ == InventoryMode::Forge) {
    if (game::smeltingFor(stack.key)) {
      out = {{Container::ForgeIn, {0}}};
      return true;
    }
    if (game::fuelValue(stack.key) > 0.0f) {
      out = {{Container::ForgeFuel, {0}}};
      return true;
    }
    return false;
  }
  // The kitchens. Without this a shift-click lands in whichever slot happens to be
  // empty first, which for a station with four different KINDS of slot is close to
  // useless: bowls end up as ingredients and fuel ends up in the pot.
  if (mode_ == InventoryMode::Cutting || mode_ == InventoryMode::Stove ||
      mode_ == InventoryMode::Pot) {
    if (stack.key == "bowl" && mode_ == InventoryMode::Pot) {
      out = {{Container::CookContainer, {0}}};
      return true;
    }
    if (mode_ != InventoryMode::Cutting && game::fuelValue(stack.key) > 0.0f) {
      out = {{Container::CookFuel, {0}}};
      return true;
    }
    if (mode_ == InventoryMode::Pot) {
      out = {{Container::CookSlots, range(0, game::kPotSlots)}};
    } else {
      out = {{Container::CookIn, {0}}};
    }
    return true;
  }
  if (mode_ == InventoryMode::Inventory && item && item->type == game::ItemType::Armor &&
      inv_ && inv_->armor()[static_cast<std::size_t>(item->armorSlot)].empty()) {
    out = {{Container::Armor, {item->armorSlot}}};
    return true;
  }
  // Shuffle between the hotbar and main storage.
  out = {{Container::Inv, index < 9 ? range(9, 36) : range(0, 9)}};
  return true;
}

void InventoryUI::depositInto(game::ItemStack& stack, const std::vector<Target>& targets) {
  const bool stacks = stackable(stack.key);
  const int max = stackMax(stack.key);
  // Two sweeps: top up matching stacks first, then fill empty slots. Doing it in one
  // pass would scatter a stack across empty slots while a partial one went untouched.
  if (stacks) {
    for (const Target& t : targets) {
      for (int i : t.indices) {
        if (stack.count <= 0) return;
        if (isOutput(t.container)) continue;
        game::ItemStack* dest = slotAt({t.container, i});
        if (!dest || dest->empty()) continue;
        if (dest->key != stack.key || dest->count >= max) continue;
        if (!accepts({t.container, i}, stack.key)) continue;
        const int add = std::min(max - dest->count, stack.count);
        dest->count += add;
        stack.count -= add;
      }
    }
  }
  for (const Target& t : targets) {
    for (int i : t.indices) {
      if (stack.count <= 0) return;
      if (isOutput(t.container)) continue;
      game::ItemStack* dest = slotAt({t.container, i});
      if (!dest || !dest->empty()) continue;
      if (!accepts({t.container, i}, stack.key)) continue;
      const int put = stacks ? std::min(max, stack.count) : 1;
      *dest = {stack.key, put, stack.dura};
      stack.count -= put;
    }
  }
}

bool InventoryUI::sweepTouch(SlotId id) {
  for (const SlotId& s : acted_) {
    if (s == id) return false;
  }
  acted_.push_back(id);
  return true;
}

int DropRun::tick(bool sameTarget, double dt) {
  if (!running || !sameTarget) {
    running = true;
    timer = kDropFirstDelay;
    interval = kDropInterval;
    return 1;
  }
  timer -= dt;
  int n = 0;
  while (n < 8 && timer <= 0.0) {
    ++n;
    interval = std::max(kDropMinInterval, interval * kDropAccel);
    timer += interval;
  }
  if (timer < 0.0) timer = 0.0;
  return n;
}

game::ItemStack takeFromStack(game::ItemStack& stack, bool whole) {
  if (stack.empty()) return {};
  game::ItemStack out = stack;
  if (whole || stack.count <= 1) {
    stack.clear();
    return out;
  }
  out.count = 1;
  stack.count -= 1;
  return out;
}

void InventoryUI::dropSlot(SlotId id, bool whole) {
  // A crafting result is not yours until you take it, and dropping the forge's
  // output slot would quietly bin a smelt in progress.
  if (isOutput(id.container) || id.container == Container::Result) return;
  game::ItemStack* slot = slotAt(id);
  if (!slot || slot->empty()) return;
  const game::ItemStack leaving = takeFromStack(*slot, whole);
  if (!leaving.empty() && onDropStack) onDropStack(leaving);
}

void InventoryUI::tickDropRun(SlotId id, bool whole, double dt) {
  const int n = dropRun_.tick(dropAt_ == id, dt);
  dropAt_ = id;
  for (int i = 0; i < n; ++i) dropSlot(id, whole);
}

void InventoryUI::swapWithHotbar(SlotId id, int n) {
  const SlotId target {Container::Inv, n};  // 0-8 of Inv is the hotbar
  if (id == target) return;
  // A result slot is not yours until taken, and the forge's output would be a
  // free smelt. quickMove already refuses these; so does this.
  if (isOutput(id.container) || id.container == Container::Result) return;

  game::ItemStack* from = slotAt(id);
  game::ItemStack* to = slotAt(target);
  if (!from || !to) return;
  if (from->empty() && to->empty()) return;
  // Both directions: an armour slot will not accept a pickaxe, and the swap must
  // not use the number key as a way around that.
  if (!to->empty() && !accepts(id, to->key)) return;
  if (!from->empty() && !accepts(target, from->key)) return;

  std::swap(*from, *to);
}

void InventoryUI::quickMove(SlotId id) {
  if (id.container == Container::Result) {
    shiftCraft();
    return;
  }
  game::ItemStack* slot = slotAt(id);
  if (!slot || slot->empty()) return;

  std::vector<Target> targets;
  if (id.container == Container::Inv) {
    if (!invSourceTargets(*slot, id.index, targets)) return;
  } else {
    targets = toInventoryTargets();
  }
  depositInto(*slot, targets);
  if (slot->count <= 0) slot->clear();
}

bool InventoryUI::canAcceptInInventory(const std::string& key, int count) const {
  if (!inv_) return false;
  const int max = stackMax(key);
  int room = 0;
  for (const game::ItemStack& s : inv_->slots()) {
    if (s.empty()) room += max;
    else if (s.key == key && stackable(key)) room += std::max(0, max - s.count);
    if (room >= count) return true;
  }
  return room >= count;
}

void InventoryUI::shiftCraft() {
  int guard = 0, made = 0;
  while (guard++ < 999) {
    const game::CraftMatch match = game::matchGrid(craft_, craftSize_, stationKind());
    if (!match) break;
    if (!canAcceptInInventory(match.outKey, match.outCount)) break;
    const game::ItemDef* item = game::getItem(match.outKey);
    game::consumeGrid(craft_, craftSize_, *match.recipe);
    const int dura =
        (item && (item->type == game::ItemType::Tool || item->type == game::ItemType::Armor))
            ? item->durability
            : -1;
    inv_->give(match.outKey, match.outCount, dura);
    ++made;
  }
  if (made > 0) audio::sfx::craft();  // one knock for the whole batch
}

// ---------------------------------------------------------------------------
// Mouse Tweaks: scroll one item across
// ---------------------------------------------------------------------------

void InventoryUI::scrollSlot(SlotId id, int direction) {
  if (id.container == Container::Result) {
    if (direction > 0) quickMove(id);
    return;
  }
  game::ItemStack* slot = slotAt(id);
  if (!slot) return;

  if (direction > 0) {
    if (slot->empty()) return;
    game::ItemStack one {slot->key, 1, slot->dura};
    std::vector<Target> targets;
    if (id.container == Container::Inv) {
      if (!invSourceTargets(*slot, id.index, targets)) return;
    } else {
      targets = toInventoryTargets();
    }
    depositInto(one, targets);
    const int moved = 1 - one.count;
    if (moved > 0) {
      slot->count -= moved;
      if (slot->count <= 0) slot->clear();
    }
    return;
  }

  if (slot->empty() || !stackable(slot->key) || slot->count >= stackMax(slot->key)) return;
  if (isOutput(id.container)) return;
  std::vector<Target> sources;
  if (id.container == Container::Inv) {
    if (!invSourceTargets(*slot, id.index, sources)) return;
  } else {
    sources = toInventoryTargets();
  }
  for (const Target& t : sources) {
    bool done = false;
    for (int i : t.indices) {
      game::ItemStack* src = slotAt({t.container, i});
      if (!src || src->empty() || src->key != slot->key) continue;
      --src->count;
      if (src->count <= 0) src->clear();
      ++slot->count;
      done = true;
      break;
    }
    if (done) break;
  }
}

// ---------------------------------------------------------------------------
// Mouse Tweaks: click-drag across slots
// ---------------------------------------------------------------------------

void InventoryUI::addDragCell(SlotId id) {
  if (id.container == Container::Result || id.container == Container::ForgeOut) return;
  for (const SlotId& seen : drag_.cells) {
    if (seen == id) return;
  }
  drag_.cells.push_back(id);
}

void InventoryUI::finishDrag() {
  const Drag d = drag_;
  dragging_ = false;
  drag_.cells.clear();
  if (cursor_.empty()) return;

  // A drag that only ever touched one slot is just a normal click.
  if (d.cells.size() <= 1 || !stackable(cursor_.key)) {
    if (!d.cells.empty()) clickSlot(d.cells.front(), d.button);
    return;
  }

  const int max = stackMax(cursor_.key);
  std::vector<SlotId> eligible;
  for (const SlotId& id : d.cells) {
    if (isOutput(id.container)) continue;
    if (!accepts(id, cursor_.key)) continue;
    const game::ItemStack* s = slotAt(id);
    if (!s) continue;
    if (s->empty() || (s->key == cursor_.key && s->count < max)) eligible.push_back(id);
  }
  if (eligible.empty()) return;

  // Left drag splits evenly; right drag drops one each.
  const int per = d.button == 0
                      ? std::max(1, cursor_.count / static_cast<int>(eligible.size()))
                      : 1;
  for (const SlotId& id : eligible) {
    if (cursor_.count <= 0) break;
    game::ItemStack* s = slotAt(id);
    if (!s) continue;
    const int have = s->empty() ? 0 : s->count;
    const int add = std::min({per, max - have, cursor_.count});
    if (add <= 0) continue;
    if (s->empty()) *s = {cursor_.key, add, cursor_.dura};
    else s->count += add;
    cursor_.count -= add;
  }
  if (cursor_.count <= 0) cursor_.clear();
}

// ---------------------------------------------------------------------------
// Building the panels
// ---------------------------------------------------------------------------

widget::StackVisual InventoryUI::visualFor(const game::ItemStack& stack) const {
  widget::StackVisual v;
  if (icons_ && icons_->uvFor(stack.key, v.icon.u0, v.icon.v0, v.icon.u1, v.icon.v1)) {
    v.icon.texture = icons_->texture();
  }
  v.count = stack.count;
  const int maxDura = game::maxDurability(stack.key);
  if (stack.wears() && maxDura > 0) {
    v.duraFraction = static_cast<float>(stack.dura) / static_cast<float>(maxDura);
  }
  return v;
}

void InventoryUI::slotNode(SlotId id, const game::ItemStack* stack, widget::SlotKind kind,
                           bool hovered) {
  Style s = widget::islot(hovered, kind);
  const int node = doc_.begin(s, containerTag(id.container), id.index);
  if (stack && !stack->empty()) {
    const widget::StackVisual v = visualFor(*stack);
    // The icon fills the slot's content box; the count and the durability bar are
    // drawn on top in draw(), because both are absolutely positioned in the CSS.
    doc_.icon(v.icon, px(Scalar::InvSlot) - 4, px(Scalar::InvSlot) - 4);
  }
  doc_.end();
  (void)node;
}

void InventoryUI::invPanel(const UiEvent& event) {
  Style panel = widget::invPanel();
  doc_.begin(panel);
  doc_.label("Inventory", widget::invTitle(), [] {
    Style s;
    s.margin = Edges(0, 0, 10, 0);
    return s;
  }());

  Style grid;
  grid.display = Display::Grid;
  grid.gridCols = 9;
  grid.gridColWidth = px(Scalar::InvSlot);
  grid.gap = px(Scalar::InvSlotGap);

  doc_.begin(grid);
  for (int i = 9; i < game::kInventorySlots; ++i) {
    const SlotId id {Container::Inv, i};
    slotNode(id, &inv_->slots()[static_cast<std::size_t>(i)], widget::SlotKind::Normal,
             haveHover_ && hovered_ == id);
  }
  doc_.end();

  Style spacer;
  spacer.height = 10;
  doc_.box(spacer);

  doc_.begin(grid);
  for (int i = 0; i < game::kHotbarSlots; ++i) {
    const SlotId id {Container::Inv, i};
    slotNode(id, &inv_->slots()[static_cast<std::size_t>(i)], widget::SlotKind::Normal,
             haveHover_ && hovered_ == id);
  }
  doc_.end();
  doc_.end();
  (void)event;
}

void InventoryUI::craftPanel(const UiEvent& event) {
  const game::CraftMatch match = game::matchGrid(craft_, craftSize_, stationKind());
  game::ItemStack result;
  if (match) result = {match.outKey, match.outCount, -1};

  doc_.begin(widget::invPanel());
  // The title row carries the recipe book button, because "what can I make with
  // this?" is a question you ask while looking at the grid, not one you leave the
  // screen to ask.
  {
    Style head = Doc::row(10, Justify::SpaceBetween, Align::Center);
    head.margin = Edges(0, 0, 10, 0);
    doc_.begin(head);
    doc_.label(mode_ == InventoryMode::Workbench ? "Workbench" : "Crafting",
               widget::invTitle());
    const bool hovered = hoveredTag_ == kTagBook;
    Style s = widget::btnSmall(hovered, false, widget::ButtonKind::Normal);
    s.width = kAuto;
    s.margin = Edges(0);
    doc_.begin(s, kTagBook);
    doc_.label("Recipes \xC2\xB7 H", widget::btnSmallText(hovered, widget::ButtonKind::Normal));
    doc_.end();
    doc_.end();
  }

  // .craft-area { display: flex; align-items: center; gap: 8px }
  doc_.begin(Doc::row(8, Justify::Start, Align::Center));

  Style grid;
  grid.display = Display::Grid;
  grid.gridCols = craftSize_;
  grid.gridColWidth = px(Scalar::InvSlot);
  grid.gap = px(Scalar::InvSlotGap);
  doc_.begin(grid);
  for (int i = 0; i < craftSize_ * craftSize_; ++i) {
    const SlotId id {Container::Craft, i};
    slotNode(id, &craft_[static_cast<std::size_t>(i)], widget::SlotKind::Normal,
             haveHover_ && hovered_ == id);
  }
  doc_.end();

  // .arrow { font-size: 26px; color: muted; padding: 0 6px }
  TextStyle arrow;
  arrow.size = 26;
  arrow.color = col(Role::Muted);
  Style arrowBox;
  arrowBox.padding = Edges(0, 6);
  doc_.label("\xE2\x9E\xA4", arrow, arrowBox);  // ➤ (&#10148;)

  const SlotId resultId {Container::Result, 0};
  slotNode(resultId, match ? &result : nullptr, widget::SlotKind::Result,
           haveHover_ && hovered_ == resultId);

  doc_.end();
  doc_.end();
  (void)event;
}

void InventoryUI::armorPanel(const UiEvent& event) {
  doc_.begin(widget::invPanel());
  doc_.label("Armour", widget::invTitle(), [] {
    Style s;
    s.margin = Edges(0, 0, 10, 0);
    return s;
  }());
  Style grid;
  grid.display = Display::Grid;
  grid.gridCols = 1;
  grid.gridColWidth = px(Scalar::InvSlot);
  grid.gap = px(Scalar::InvSlotGap);
  doc_.begin(grid);
  for (int i = 0; i < game::kArmorSlots; ++i) {
    const SlotId id {Container::Armor, i};
    slotNode(id, &inv_->armor()[static_cast<std::size_t>(i)], widget::SlotKind::Armor,
             haveHover_ && hovered_ == id);
  }
  doc_.end();
  doc_.end();
  (void)event;
}

void InventoryUI::forgePanel(const UiEvent& event) {
  doc_.begin(widget::invPanel());
  doc_.label("Forge", widget::invTitle(), [] {
    Style s;
    s.margin = Edges(0, 0, 10, 0);
    return s;
  }());
  doc_.begin(Doc::row(8, Justify::Start, Align::Center));

  Style column;
  column.display = Display::Grid;
  column.gridCols = 1;
  column.gridColWidth = px(Scalar::InvSlot);
  column.gap = 8;
  doc_.begin(column);
  const SlotId inId {Container::ForgeIn, 0};
  const SlotId fuelId {Container::ForgeFuel, 0};
  slotNode(inId, slotAt(inId), widget::SlotKind::Normal, haveHover_ && hovered_ == inId);
  slotNode(fuelId, slotAt(fuelId), widget::SlotKind::Normal, haveHover_ && hovered_ == fuelId);
  doc_.end();

  // .forge-area: smelt progress on top with the input, burn below with the fuel.
  Style area = Doc::column(10, Align::Center);
  doc_.begin(area);
  Style bar;
  bar.width = 80;
  bar.height = 8;
  bar.radius = 4;
  bar.bg = kBlack;
  doc_.custom(bar, 2001, 0);  // smelt progress
  TextStyle arrow;
  arrow.size = 26;
  arrow.color = col(Role::Muted);
  doc_.label("\xE2\x9E\xA4", arrow);
  doc_.custom(bar, 2001, 1);  // fuel burn
  doc_.end();

  const SlotId outId {Container::ForgeOut, 0};
  slotNode(outId, slotAt(outId), widget::SlotKind::Normal, haveHover_ && hovered_ == outId);

  doc_.end();
  doc_.end();
  (void)event;
}

void InventoryUI::kitchenPanel(const UiEvent& event) {
  const bool isPot = mode_ == InventoryMode::Pot;
  const char* title = isPot      ? "Cooking Pot"
                      : mode_ == InventoryMode::Stove ? "Stove"
                                                      : "Cutting Board";

  doc_.begin(widget::invPanel());
  doc_.label(title, widget::invTitle(), [] {
    Style s;
    s.margin = Edges(0, 0, 10, 0);
    return s;
  }());
  doc_.begin(Doc::row(8, Justify::Start, Align::Center));

  // Ingredients. The pot takes six in two rows of three; the board and the stove
  // take one.
  if (isPot) {
    Style grid;
    grid.display = Display::Grid;
    grid.gridCols = 3;
    grid.gridColWidth = px(Scalar::InvSlot);
    grid.gap = 6;
    doc_.begin(grid);
    for (int i = 0; i < game::kPotSlots; ++i) {
      const SlotId id {Container::CookSlots, i};
      slotNode(id, slotAt(id), widget::SlotKind::Normal, haveHover_ && hovered_ == id);
    }
    doc_.end();
  } else {
    const SlotId in {Container::CookIn, 0};
    slotNode(in, slotAt(in), widget::SlotKind::Normal, haveHover_ && hovered_ == in);
  }

  // The bowl and the fuel, stacked. The bowl sits apart from the ingredients on
  // purpose: it is not one, and a player who drops a bowl in among the vegetables
  // should be able to see straight away that it went somewhere else.
  //
  // The cutting board shows neither — it serves nothing and burns nothing — so its
  // window is honestly two slots and a bar rather than four slots, two of which
  // silently do nothing.
  if (isPot || mode_ == InventoryMode::Stove) {
    doc_.begin(Doc::column(8, Align::Center));
    if (isPot) {
      const SlotId bowl {Container::CookContainer, 0};
      slotNode(bowl, slotAt(bowl), widget::SlotKind::Normal, haveHover_ && hovered_ == bowl);
    }
    const SlotId fuel {Container::CookFuel, 0};
    slotNode(fuel, slotAt(fuel), widget::SlotKind::Normal, haveHover_ && hovered_ == fuel);
    doc_.end();
  }

  // The same two gauges the forge draws: cook progress on top, burn below.
  doc_.begin(Doc::column(10, Align::Center));
  Style bar;
  bar.width = 80;
  bar.height = 8;
  bar.radius = 4;
  bar.bg = kBlack;
  doc_.custom(bar, 2001, 0);
  TextStyle arrow;
  arrow.size = 26;
  arrow.color = col(Role::Muted);
  doc_.label("\xE2\x9E\xA4", arrow);
  if (mode_ != InventoryMode::Cutting) doc_.custom(bar, 2001, 1);
  doc_.end();

  const SlotId out {Container::CookOut, 0};
  slotNode(out, slotAt(out), widget::SlotKind::Normal, haveHover_ && hovered_ == out);

  doc_.end();
  doc_.end();
  (void)event;
}

void InventoryUI::chestPanel(const UiEvent& event) {
  doc_.begin(widget::invPanel());
  doc_.label("Chest", widget::invTitle(), [] {
    Style s;
    s.margin = Edges(0, 0, 10, 0);
    return s;
  }());
  Style grid;
  grid.display = Display::Grid;
  grid.gridCols = 9;
  grid.gridColWidth = px(Scalar::InvSlot);
  grid.gap = px(Scalar::InvSlotGap);
  doc_.begin(grid);
  for (int i = 0; i < game::kChestSlots; ++i) {
    const SlotId id {Container::Chest, i};
    slotNode(id, slotAt({Container::Chest, i}), widget::SlotKind::Normal,
             haveHover_ && hovered_ == id);
  }
  doc_.end();
  doc_.end();
  (void)event;
}

float bagPanelWidth() {
  return px(Scalar::InvSlot) * 9 + px(Scalar::InvSlotGap) * 8 + px(Scalar::Pad) * 2 +
         px(Scalar::Border) * 2;
}

void InventoryUI::build(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens) {
  doc_.reset(&text);

  // #inventory.screen.dim, centring one column.
  Style root = widget::screen();
  doc_.begin(root);

  // The whole thing is a centred column: the station panels on top, the bag below,
  // then the hint line.
  doc_.begin(Doc::column(14, Align::Center));

  // The station row is given the BAG's width rather than its own.
  //
  // Every panel here was centred on its own content, which meant the armour column
  // and the crafting grid were centred as a pair on an axis the bag below them did
  // not share — so the top half of the screen sat visibly left of the bottom half,
  // and nothing lined up with anything. Measuring the bag once and handing that
  // width to the row above it is what makes the whole assembly read as one object.
  //
  const float bagWidth = bagPanelWidth();

  // Spread when there are two panels, centred when there is one. A lone crafting
  // grid pushed hard to one end of a bag-wide row would look like a mistake.
  const bool twoPanels = mode_ == InventoryMode::Inventory;
  Style stationRow = Doc::row(px(Scalar::GapWide),
                              twoPanels ? Justify::SpaceBetween : Justify::Center,
                              Align::Start);
  stationRow.width = bagWidth;
  doc_.begin(stationRow);
  switch (mode_) {
    case InventoryMode::Inventory:
      armorPanel(event);
      craftPanel(event);
      break;
    case InventoryMode::Workbench: craftPanel(event); break;
    case InventoryMode::Forge:
      if (station_) forgePanel(event);
      break;
    case InventoryMode::Chest:
      if (station_) chestPanel(event);
      break;
    case InventoryMode::Cutting:
    case InventoryMode::Stove:
    case InventoryMode::Pot:
      if (station_) kitchenPanel(event);
      break;
    case InventoryMode::Closed: break;
  }
  doc_.end();

  invPanel(event);

  doc_.label("Shift-drag moves stacks \xC2\xB7 Q-drag drops them \xC2\xB7 drag to split \xC2\xB7 "
             "scroll to nudge \xC2\xB7 E/Esc to close",
             widget::muted(12));
  doc_.end();
  doc_.end();

  doc_.layout({0, 0, ui.width(), ui.height()});
  (void)tweens;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void InventoryUI::update(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens) {
  if (mode_ == InventoryMode::Closed || !inv_) return;

  mouseX_ = event.mouseX;
  mouseY_ = event.mouseY;

  // The forge's block entity is refreshed by the caller every frame; if it vanished
  // (the block was mined while the screen was open) fall back to the plain bag rather
  // than writing through a dangling pointer.
  if ((mode_ == InventoryMode::Forge || mode_ == InventoryMode::Chest ||
       mode_ == InventoryMode::Cutting || mode_ == InventoryMode::Stove ||
       mode_ == InventoryMode::Pot) &&
      !station_) {
    mode_ = InventoryMode::Inventory;
    craftSize_ = 2;
    craft_.assign(4, game::ItemStack {});
  }

  // Two passes over the tree per frame: one to find what is under the cursor with last
  // frame's geometry, then the real build with the hover state applied. The alternative
  // is a frame of lag on every hover, which is visible on a 46px slot.
  build(ui, text, event, tweens);
  const int hit = doc_.hitTest(event.mouseX, event.mouseY);
  // main.js:120 hung one listener on the document and ticked whenever the click
  // landed inside a <button>; this is that listener.
  if (event.leftClick && doc_.clickedButton(hit)) audio::sfx::uiClick();
  const bool wasHovering = haveHover_;
  const SlotId previous = hovered_;
  const int previousTag = hoveredTag_;
  haveHover_ = false;
  hoveredTag_ = hit >= 0 ? doc_.node(hit).tag : 0;
  if (hit >= 0 && isSlotTag(doc_.node(hit).tag)) {
    hovered_ = {tagContainer(doc_.node(hit).tag), doc_.node(hit).index};
    haveHover_ = true;
  }
  if (haveHover_ != wasHovering || !(hovered_ == previous) || hoveredTag_ != previousTag) {
    build(ui, text, event, tweens);
  }

  if (event.leftClick && hoveredTag_ == kTagBook) {
    if (onOpenRecipeBook) onOpenRecipeBook();
    return;
  }

  // --- Mouse Tweaks sweeps ----------------------------------------------------
  //
  // Both are "hold something down and drag": shift+left repeats the shift-click
  // transfer, Q repeats a drop. They are checked before the click handling below so
  // a shift-click that becomes a drag is one gesture rather than a click and then a
  // separate sweep, and they end the moment the modifier does — letting go of shift
  // mid-drag stops the sweep, which is what stops it running away with a chest.
  const bool qDown = event.input && event.input->down(Key::Q);
  const bool qPressed = event.input && event.input->pressed(Key::Q);
  if (sweep_ == Sweep::Transfer && !(event.leftDown && event.shift)) sweep_ = Sweep::None;
  if (sweep_ == Sweep::Drop && !qDown) {
    sweep_ = Sweep::None;
    dropRun_.stop();
  }
  if (sweep_ == Sweep::None) acted_.clear();

  if (haveHover_ && cursor_.empty()) {
    if (qPressed) {
      sweep_ = Sweep::Drop;
      acted_.clear();
      dropRun_.stop();  // a fresh press always drops at once
    } else if (event.leftClick && event.shift) {
      sweep_ = Sweep::Transfer;
      acted_.clear();
    }
  }
  if (haveHover_) {
    if (sweep_ == Sweep::Transfer && sweepTouch(hovered_)) {
      quickMove(hovered_);
    } else if (sweep_ == Sweep::Drop) {
      // Shift decides one-versus-all at the moment of each drop rather than when
      // the key went down, so it can be taken or let go mid-run.
      tickDropRun(hovered_, event.shift, event.dt);
    }
  }

  // 1-9 over a slot swaps it with that hotbar slot, the way every other game in
  // this genre does it. A swap rather than a move, so the hotbar slot's contents
  // are never destroyed and the gesture is its own undo — press the same number
  // again and both are back where they started. Held by the cursor is excluded:
  // there the number would be ambiguous between "put this there" and "swap".
  if (haveHover_ && cursor_.empty() && event.input) {
    for (int n = 0; n < 9; ++n) {
      if (!event.input->pressed(static_cast<Key>(static_cast<int>(Key::Digit1) + n))) continue;
      swapWithHotbar(hovered_, n);
      break;
    }
  }
  // A sweep owns the pointer, so the click handling below is skipped entirely --
  // otherwise the same shift-click would transfer the slot a second time. Not an
  // early return: the forge's bars are refreshed at the end of this function and
  // they must not stop while somebody is emptying it.
  if (sweep_ != Sweep::None && dragging_) finishDrag();

  if (sweep_ == Sweep::None && haveHover_) {
    if (event.leftClick) {
      if (event.shift) {
        quickMove(hovered_);
      } else if (!cursor_.empty() && hovered_.container != Container::Result) {
        dragging_ = true;
        drag_.button = 0;
        drag_.cells.clear();
        addDragCell(hovered_);
      } else {
        clickSlot(hovered_, 0);
      }
    } else if (event.rightClick) {
      if (!cursor_.empty() && hovered_.container != Container::Result) {
        dragging_ = true;
        drag_.button = 1;
        drag_.cells.clear();
        addDragCell(hovered_);
      } else {
        clickSlot(hovered_, 1);
      }
    } else if (dragging_) {
      addDragCell(hovered_);
    } else if (event.wheel != 0.0f) {
      scrollSlot(hovered_, event.wheel > 0 ? 1 : -1);
    }
  }

  if (dragging_ && ((drag_.button == 0 && event.leftRelease) ||
                    (drag_.button == 1 && event.rightRelease))) {
    finishDrag();
  }

  if (mode_ == InventoryMode::Forge && station_) {
    forgeFuel_ = station_->fuelMax > 0.0f
                     ? std::max(0.0f, station_->fuelLeft / station_->fuelMax)
                     : 0.0f;
    const game::SmeltingRecipe* smelt =
        station_->input.empty() ? nullptr : game::smeltingFor(station_->input.key);
    forgeProgress_ = smelt ? std::min(1.0f, station_->progress / smelt->seconds) : 0.0f;
  }

  // The kitchens feed the same two gauges. Asking the matcher for the cook time
  // rather than storing it means the bar is right the instant the ingredients
  // change, including when they change to something that no longer cooks at all.
  if ((mode_ == InventoryMode::Cutting || mode_ == InventoryMode::Stove ||
       mode_ == InventoryMode::Pot) &&
      station_) {
    forgeFuel_ = station_->fuelMax > 0.0f
                     ? std::max(0.0f, station_->fuelLeft / station_->fuelMax)
                     : 0.0f;
    std::vector<game::ItemStack> offered;
    if (mode_ == InventoryMode::Pot) {
      offered = station_->slots;
    } else {
      offered.push_back(station_->input);
    }
    const game::Kitchen kitchen = mode_ == InventoryMode::Pot     ? game::Kitchen::Pot
                                  : mode_ == InventoryMode::Stove ? game::Kitchen::Stove
                                                                  : game::Kitchen::Cutting;
    const game::CookMatch m = game::matchCooking(kitchen, offered, station_->container);
    forgeProgress_ =
        m.seconds > 0.0f ? std::min(1.0f, station_->progress / m.seconds) : 0.0f;
  }
}

void InventoryUI::draw(Ui2D& ui, Text& text) {
  if (mode_ == InventoryMode::Closed || !inv_) return;

  // .screen.dim { background: col(Role::WashScreen) }
  ui.fillRect({0, 0, ui.width(), ui.height()}, col(Role::WashScreen));
  doc_.paint(ui);

  // The absolutely-positioned parts of a slot: the count and the durability bar sit
  // over the icon, and the layout engine has no absolute positioning by design.
  for (int i = 0; i < doc_.count(); ++i) {
    const Node& n = doc_.node(i);
    if (!isSlotTag(n.tag)) continue;
    const SlotId id {tagContainer(n.tag), n.index};
    const game::ItemStack* stack = slotAt(id);
    game::ItemStack resultStack;
    if (id.container == Container::Result) {
      const game::CraftMatch match = game::matchGrid(craft_, craftSize_, stationKind());
      if (!match) continue;
      resultStack = {match.outKey, match.outCount, -1};
      stack = &resultStack;
    }
    if (!stack || stack->empty()) continue;
    widget::StackVisual v = visualFor(*stack);
    v.icon.texture = 0;  // the icon quad is already in the tree
    widget::drawStack(ui, text, n.rect, v);
  }

  // The two gauges, which are custom nodes the layout only reserved space for. The
  // kitchens reuse them unchanged: a cook and a smelt are the same two questions,
  // "how far through" and "how much fuel is left".
  if (mode_ == InventoryMode::Forge || mode_ == InventoryMode::Cutting ||
      mode_ == InventoryMode::Stove || mode_ == InventoryMode::Pot) {
    const int progressNode = doc_.findTag(2001, 0);
    const int fuelNode = doc_.findTag(2001, 1);
    if (progressNode >= 0) {
      widget::drawBar(ui, doc_.node(progressNode).rect, forgeProgress_, col(Role::ProgressFill),
                      kBlack, 4);
    }
    if (fuelNode >= 0) {
      widget::drawBar(ui, doc_.node(fuelNode).rect, forgeFuel_, col(Role::FuelFill), kBlack, 4);
    }
  }

  // #held-stack — the floating stack that follows the cursor, centred on it.
  if (!cursor_.empty()) {
    const Rect r {mouseX_ - px(Scalar::InvSlot) * 0.5f, mouseY_ - px(Scalar::InvSlot) * 0.5f,
                  px(Scalar::InvSlot), px(Scalar::InvSlot)};
    widget::drawStack(ui, text, r, visualFor(cursor_));
  } else if (haveHover_) {
    // The tooltip only shows when nothing is on the cursor, which is what the JS did by
    // checking `this.cursor` before calling updateTip.
    const game::ItemStack* stack = slotAt(hovered_);
    game::ItemStack resultStack;
    if (hovered_.container == Container::Result) {
      const game::CraftMatch match = game::matchGrid(craft_, craftSize_, stationKind());
      if (match) {
        resultStack = {match.outKey, match.outCount, -1};
        stack = &resultStack;
      } else {
        stack = nullptr;
      }
    }
    if (stack && !stack->empty()) {
      const game::Tooltip tip =
          game::itemTooltip(stack->key, stack->dura, game::fuelValue(stack->key));
      widget::drawTooltip(ui, text, mouseX_, mouseY_, tip.name, tip.lines);
    }
  }
}

}  // namespace hr::ui
