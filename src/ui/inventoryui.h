// Inventory, crafting, forge and chest, ported from js/ui/inventoryui.js.
//
// The layout is the easy half. The cost is in the interactions, which the original
// took from the "Mouse Tweaks" mod and which are genuinely fiddly:
//
//   • left click     — pick up / place whole / merge / swap
//   • right click    — pick up half / place one
//   • shift+left     — quick-move a stack to the other container
//   • left-drag      — split the held stack evenly across the slots dragged over
//   • right-drag     — drop one into each slot dragged over
//   • scroll a slot  — down pushes one across, up pulls one matching item in
//
// Those are transcribed rather than reinterpreted, including the order the branches
// are tested in, because the difference between "swap" and "merge" is one condition
// and getting it wrong loses items.
//
// Containers are addressed by a small enum instead of the JS `ref(name)` string
// switch, which is the one change: a typo'd container name silently returned null
// there, and here it will not compile.

#pragma once

#include <string>
#include <vector>

#include "game/blockentities.h"
#include "game/inventory.h"
#include "render/iconatlas.h"
#include "ui/dom.h"
#include "ui/text.h"
#include "ui/ui2d.h"
#include "ui/widgets.h"
#include "game/recipes.h"
#include "world/blocks.h"

namespace hr::ui {

// Which panel set is showing. Matches js/ui/inventoryui.js's `mode`.
enum class InventoryMode { Closed, Inventory, Workbench, Forge, Chest };

// The containers a slot can belong to.
enum class Container {
  None,
  Inv,    // 36 slots: 0-8 hotbar, 9-35 main storage
  Armor,  // 4 slots, each accepting only its own piece
  Craft,  // the 2x2 or 3x3 grid
  Result, // the crafting output, which is taken from but never placed into
  Chest,  // 27 slots in a block entity
  ForgeIn,
  ForgeFuel,
  ForgeOut,
};

class InventoryUI {
 public:
  void attach(game::Inventory* inventory, const render::IconAtlas* icons);

  void open(InventoryMode mode, game::BlockEntity* station);
  // Returns the crafting grid and the cursor stack to the inventory. Forge and chest
  // contents stay in their block entity, which is what makes them persist.
  void close();
  bool isOpen() const { return mode_ != InventoryMode::Closed; }
  InventoryMode mode() const { return mode_; }

  // The live block entity, refreshed every frame: a forge whose block was broken
  // while the screen was open must not be written to.
  void setStation(game::BlockEntity* station) { station_ = station; }

  // Handles the pointer, then lays out and draws. One call, because the drag
  // interaction needs the hit test from the same frame's layout.
  void update(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens);
  void draw(Ui2D& ui, Text& text);

  // True while a stack is on the cursor, so Escape can be made to drop it back first.
  bool holdingStack() const { return !cursor_.empty(); }

 private:
  struct SlotId {
    Container container = Container::None;
    int index = 0;
    bool operator==(const SlotId& o) const {
      return container == o.container && index == o.index;
    }
  };

  // --- container access, the ref(name) switch from the JS -------------------
  game::ItemStack* slotAt(SlotId id);
  const game::ItemStack* slotAt(SlotId id) const;
  bool isOutput(Container c) const { return c == Container::ForgeOut || c == Container::Result; }
  // Armour slots only take their own piece; everything else takes anything.
  bool accepts(SlotId id, const std::string& key) const;

  int stackMax(const std::string& key) const;
  bool stackable(const std::string& key) const;

  // --- interactions --------------------------------------------------------
  void clickSlot(SlotId id, int button);
  void takeResult();
  void quickMove(SlotId id);
  void shiftCraft();
  void scrollSlot(SlotId id, int direction);
  void addDragCell(SlotId id);
  void finishDrag();

  struct Target {
    Container container;
    std::vector<int> indices;
  };
  std::vector<Target> toInventoryTargets() const;
  // Where a shift-click from an inventory slot should send the stack, which depends on
  // the open station: a chest takes anything, a forge only fuel or ore, and with no
  // station open the stack shuttles between the hotbar and main storage.
  bool invSourceTargets(const game::ItemStack& stack, int index,
                        std::vector<Target>& out) const;
  void depositInto(game::ItemStack& stack, const std::vector<Target>& targets);
  bool canAcceptInInventory(const std::string& key, int count) const;

  game::CraftStation stationKind() const;

  // --- building ------------------------------------------------------------
  void build(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens);
  void slotNode(SlotId id, const game::ItemStack* stack, widget::SlotKind kind, bool hovered);
  void invPanel(const UiEvent& event);
  void craftPanel(const UiEvent& event);
  void armorPanel(const UiEvent& event);
  void forgePanel(const UiEvent& event);
  void chestPanel(const UiEvent& event);
  widget::StackVisual visualFor(const game::ItemStack& stack) const;

  game::Inventory* inv_ = nullptr;
  const render::IconAtlas* icons_ = nullptr;
  game::BlockEntity* station_ = nullptr;

  InventoryMode mode_ = InventoryMode::Closed;
  int craftSize_ = 2;
  std::vector<game::ItemStack> craft_;
  game::ItemStack cursor_;

  Doc doc_;
  SlotId hovered_ {};
  bool haveHover_ = false;
  float mouseX_ = 0, mouseY_ = 0;

  // An in-progress drag-distribute. `seen` prevents a slot being counted twice when
  // the pointer wanders back over it.
  struct Drag {
    int button = 0;
    std::vector<SlotId> cells;
  };
  bool dragging_ = false;
  Drag drag_;

  // Progress bars read from the block entity; cached only so draw() does not need the
  // pointer again after update() validated it.
  float forgeFuel_ = 0, forgeProgress_ = 0;
};

}  // namespace hr::ui
