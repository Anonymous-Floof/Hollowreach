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

#include <functional>
#include <string>
#include <vector>

#include "game/blockentities.h"
#include "game/inventory.h"
#include "render/iconatlas.h"
#include "ui/dom.h"
#include "ui/paletteui.h"
#include "ui/text.h"
#include "ui/ui2d.h"
#include "ui/widgets.h"
#include "game/recipes.h"
#include "world/blocks.h"

namespace hr::ui {

// Hold-to-drop pacing, in seconds. Shared with the world's own Q so the two feel
// the same: the hotbar has no UI object to own this, but a player pressing the
// same key should not meet two different rhythms.
//
// The first repeat waits long enough that a tap is unambiguously one item and
// nobody loses a second by accident; after that the run speeds up, because
// emptying a stack of sixty-four should not take sixty-four presses OR twelve
// seconds of holding. From 0.16 s down to 0.035 s takes about a second and a half
// to reach full rate, which clears a full stack in roughly three.
inline constexpr double kDropFirstDelay = 0.35;
inline constexpr double kDropInterval = 0.16;
inline constexpr double kDropMinInterval = 0.035;
inline constexpr double kDropAccel = 0.82;

// The cadence of a held Q, kept apart from both the screen and the world so the
// two cannot drift and so it can be tested without a window.
struct DropRun {
  bool running = false;
  double timer = 0;
  double interval = 0;

  // How many items should leave this frame. Entering a new target always yields
  // exactly one immediately, which is what makes both a tap and a drag across
  // slots feel instant; holding then repeats on the accelerating timer above.
  //
  // Bounded per call, so a frame that took a long time — a chunk upload, a
  // breakpoint — cannot empty a chest in one go.
  int tick(bool sameTarget, double dt);
  void stop() { running = false; }
};

// Splits one item, or everything, out of `stack` and returns what leaves. The
// remainder stays in `stack`, cleared if nothing is left. Wear rides along: a
// stack of part-used tools does not stack in the first place, but a single item
// leaving one must not arrive as a pristine copy if it ever could.
game::ItemStack takeFromStack(game::ItemStack& stack, bool whole);

// The outer width of the bag panel: nine columns, their gaps, the panel's padding
// and its two borders.
//
// Public because the station row above the bag is given this width rather than its
// own — see InventoryUI::build for why — and because that is the sort of agreement
// between two pieces of layout that silently drifts unless there is one function
// both of them call. Reads the theme, so a pack that resizes slots moves both.
float bagPanelWidth();

// Which panel set is showing. Matches js/ui/inventoryui.js's `mode`.
enum class InventoryMode { Closed, Inventory, Workbench, Forge, Chest, Cutting, Stove, Pot,
                           // The Dyer's Palette. A mode here rather than a screen of its own
                           // because the one thing it needs is a slot the player can PUT
                           // something in, and every gesture that fills a slot — drag,
                           // shift-click, sweep, Q-drop, 1-9 swap — lives in this class.
                           // It was built standalone first and had no way to fill its slot
                           // at all.
                           Palette };

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
  // The kitchen. ONE set shared by all three stations rather than three sets:
  // slotAt() is a single switch and every gesture in this screen routes through it,
  // so three near-identical families would be three chances to get one wrong.
  CookIn,         // the board's and the stove's single input; the pot uses CookSlots
  CookSlots,      // the pot's six ingredient slots
  CookContainer,  // the bowl
  CookFuel,
  CookOut,
  // The palette's single slot. Its own container rather than reusing CookIn, so
  // `accepts` can refuse anything that cannot take a dye.
  Dye,
  // The creative bin. The ONLY slot in the game that destroys what it is given, and
  // therefore the only one with a rule of its own for every gesture — see bulkSafe.
  Trash,
};

class InventoryUI {
 public:
  void attach(game::Inventory* inventory, const render::IconAtlas* icons);

  // Throws a stack into the world. The screen has no idea where the player is or
  // whether this is a guest, so App supplies the same toss that Q uses in the world.
  std::function<void(const game::ItemStack&)> onDropStack;
  // The Recipes button in the crafting panel's header.
  std::function<void()> onOpenRecipeBook;

  void open(InventoryMode mode, game::BlockEntity* station);
  void attachPalette(PaletteUI* palette, game::ItemStack* slot) {
    palette_ = palette;
    dyeSlot_ = slot;
  }
  // The creative bin's slot, owned by App so that what is in it survives closing and
  // reopening the screen — the same reason the palette's slot lives there.
  void attachTrash(game::ItemStack* slot) { trashSlot_ = slot; }
  // Creative turns the bin on. Off, the panel is not drawn and the container holds
  // nothing that can be reached, which is why App hands the contents back rather
  // than leaving them in a slot nobody can see.
  void setCreative(bool on) { creative_ = on; }
  bool creative() const { return creative_; }

  // Whether a gesture OTHER than a plain left click on the slot itself may touch
  // this container.
  //
  // The trash is the one slot that destroys what it is given, so it is the one slot
  // that has to refuse everything convenient: the shift-click sweep, the Q-drop run,
  // the drag-distribute, the scroll nudge and the 1-9 hotbar swap all move stacks
  // without the player looking at where they land, and every one of them would
  // otherwise be a way to lose something without meaning to. One predicate, called
  // from every one of those paths, because six copies of `!= Container::Trash` is
  // six chances to leave one out — and the one left out is somebody's stack.
  static bool bulkSafe(Container container) { return container != Container::Trash; }

  // What a left click on the bin does, given what is on the cursor and what is in
  // it. The whole rule, in one function, for the same reason dyeSlotAccepts is one:
  // it is the part of this screen worth asserting without a window, and it is the
  // part where being wrong destroys something the player owns.
  //
  // Holding nothing takes back what is in the bin. Holding something puts it in and
  // the previous occupant is GONE — that is Terraria's rule and the reason to copy
  // it: the bin is one level of undo, not a hole. Retrieval is always available
  // right up until the next thing is thrown away.
  static void applyTrashClick(game::ItemStack& cursor, game::ItemStack& bin);

  // Whether the bin can be reached at all. Creative-only, and it needs the slot App
  // owns: called by slotAt, so a gesture cannot find a container the screen is not
  // drawing — which in this container would mean destroying something off-screen.
  bool trashReachable() const { return creative_ && trashSlot_ != nullptr; }
  // Returns the crafting grid and the cursor stack to the inventory. Forge and chest
  // contents stay in their block entity, which is what makes them persist.
  void close();
  bool isOpen() const { return mode_ != InventoryMode::Closed; }
  InventoryMode mode() const { return mode_; }

  // The live block entity, refreshed every frame: a forge whose block was broken
  // while the screen was open must not be written to.
  void setStation(game::BlockEntity* station) { station_ = station; }
  void setDiet(const game::Diet* diet) { diet_ = diet; }

  // Handles the pointer, then lays out and draws. One call, because the drag
  // interaction needs the hit test from the same frame's layout.
  void update(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens);
  void draw(Ui2D& ui, Text& text);

  // True while a stack is on the cursor, so Escape can be made to drop it back first.
  bool holdingStack() const { return !cursor_.empty(); }

  // Lays a recipe out in the crafting grid, taking one item per cell from the bag.
  // Whatever was already in the grid goes back first, so clicking a second recipe
  // replaces the first rather than refusing.
  //
  // All or nothing: if any ingredient is missing the grid is restored and this
  // returns false, because a half-filled grid is worse than an untouched one —
  // it looks like the recipe was laid out and quietly is not.
  enum class FillResult { Ok, NoGrid, TooBig, Missing };
  FillResult autoFill(const game::Recipe& recipe);

  // Whether the palette's slot will take this item.
  //
  // Public and static because it is ONE rule enforced in two places — a drop onto
  // the slot, and a shift-click aimed at it — and two copies of it is two chances
  // for the slot to accept something the Dye button then refuses. It is also the
  // only part of that screen worth asserting without a window.
  static bool dyeSlotAccepts(const std::string& key);

  // What ONE slot of this container will hold. The item's own maximum everywhere
  // except the palette, which takes a dye's worth and no more.
  //
  // Public alongside dyeSlotAccepts and for the same reason: every path that puts
  // something into a slot — a drop, a merge, both halves of the shift-click sweep —
  // goes through this one function, so the cap is ENFORCED rather than announced.
  // It used to be announced: a shift-clicked stack of sixty-four landed in the slot
  // and the screen then explained that sixteen was the limit.
  int slotCapacity(Container container, const std::string& key) const;

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
  // Swaps `id` with hotbar slot `n` (0-8). No-op when the slot is an output, or
  // when either side would refuse the other — an armour slot will not take a
  // pickaxe, and swapping into one has to respect that in both directions.
  void swapWithHotbar(SlotId id, int n);
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
  // One panel for all three kitchens. The pot shows six ingredient slots and a bowl,
  // the other two show a single input; everything else about the layout is the same,
  // so writing it three times would only be three places to fix a spacing bug.
  void kitchenPanel(const UiEvent& event);
  void palettePanel(const UiEvent& event);
  // The creative bin, beside the diet bars. Only built when creative is on.
  void trashPanel(const UiEvent& event);
  // The five diet bars. On the inventory screen rather than the HUD: it is a thing
  // you check when deciding what to eat, not while running from something.
  void dietPanel(const UiEvent& event);
  widget::StackVisual visualFor(const game::ItemStack& stack) const;

  game::Inventory* inv_ = nullptr;
  const render::IconAtlas* icons_ = nullptr;
  game::BlockEntity* station_ = nullptr;
  // The palette's slot and its colour model, both owned by App. This class supplies
  // the one thing PaletteUI could not: a slot the bag can move an item into.
  game::ItemStack* dyeSlot_ = nullptr;
  // The creative bin's slot, likewise App's. Never saved: it is emptied when the
  // world is left, which is what stops a world file quietly carrying something the
  // player believes they threw away.
  game::ItemStack* trashSlot_ = nullptr;
  bool creative_ = false;
  PaletteUI* palette_ = nullptr;
  Rect paletteWheel_;  // reserved by the layout, painted and hit-tested after it
  Rect paletteSide_;   // and the readouts beside it
  const game::Diet* diet_ = nullptr;

  InventoryMode mode_ = InventoryMode::Closed;
  int craftSize_ = 2;
  std::vector<game::ItemStack> craft_;
  game::ItemStack cursor_;

  Doc doc_;
  SlotId hovered_ {};
  bool haveHover_ = false;
  int hoveredTag_ = 0;  // non-slot widgets, currently just the Recipes button
  float mouseX_ = 0, mouseY_ = 0;

  // An in-progress drag-distribute. `seen` prevents a slot being counted twice when
  // the pointer wanders back over it.
  struct Drag {
    int button = 0;
    std::vector<SlotId> cells;
  };
  bool dragging_ = false;
  Drag drag_;

  // Mouse Tweaks' sweep: hold the modifier and drag across slots, and every slot
  // the pointer enters gets the same action once. Distinct from Drag above, which
  // distributes ONE held stack across the cells it touches; a sweep acts on each
  // slot's own contents and holds nothing.
  //
  // `acted_` is what makes it once-per-slot rather than once-per-frame: the pointer
  // sits inside a slot for many frames and wanders back over slots it already
  // passed, and either would otherwise fire again.
  enum class Sweep : std::uint8_t { None, Transfer, Drop };
  Sweep sweep_ = Sweep::None;
  std::vector<SlotId> acted_;
  bool sweepTouch(SlotId id);  // true the first time this sweep sees `id`
  // `whole` empties the slot; otherwise one item leaves it.
  void dropSlot(SlotId id, bool whole);

  // Q held over a slot: one item at once, then a run that speeds up.
  //
  // Deliberately not the once-per-slot rule the Transfer sweep uses. Both gestures
  // want to fire as the pointer crosses a new slot, but only this one also wants to
  // keep going while it sits still — a stack of sixty-four is not something anybody
  // should have to press a key sixty-four times for. So the slot under the pointer
  // is tracked here rather than in `acted_`, and moving to a new one restarts the
  // run rather than being refused as already-acted.
  void tickDropRun(SlotId id, bool whole, double dt);
  SlotId dropAt_ {};
  DropRun dropRun_;

  // Progress bars read from the block entity; cached only so draw() does not need the
  // pointer again after update() validated it.
  float forgeFuel_ = 0, forgeProgress_ = 0;
};

}  // namespace hr::ui
