// The stylesheet's classes, as functions.
//
// Every rule in css/style.css that a screen reaches for lives here as a Style or
// TextStyle builder, named after the class it came from. That keeps the screens
// looking like the HTML they were ported from — `doc.begin(widget::menuCard())` reads
// the way `<div class="menu-card">` did — and it means a colour or a corner radius is
// changed in one place rather than nine.
//
// Hover and active states are parameters rather than separate builders, because CSS
// resolves them by cascade and there is no cascade here.

#pragma once

#include <string>
#include <vector>

#include "core/input.h"
#include "ui/dom.h"
#include "ui/text.h"
#include "ui/theme.h"

namespace hr::ui {

// Everything a screen needs to know about the pointer and keyboard this frame.
// Positions are in layout pixels, so they line up with node rects directly.
struct UiEvent {
  float mouseX = 0, mouseY = 0;
  bool leftClick = false;    // pressed this frame
  bool rightClick = false;
  bool leftDown = false;     // held
  bool rightDown = false;
  bool leftRelease = false;
  bool rightRelease = false;
  float wheel = 0;           // browser deltaY sign: positive scrolls down
  bool shift = false, ctrl = false, alt = false;
  const Input* input = nullptr;   // for key edges and typed text
  double dt = 0;
};

// CSS transitions and keyframes. There is no cascade to hang a transition off, so
// each animated property asks for its progress by key once per frame.
//
// Keys are (tag, index) pairs from the node tree, which is what makes the store
// survive the tree being rebuilt every frame.
class TweenStore {
 public:
  void beginFrame(double dt);

  // Moves the value for `key` toward 1 while `active`, toward 0 otherwise, at
  // 1/duration per second. Returns the eased progress — CSS's default `ease`
  // timing function, so a hover ramp has the same shape as the browser's.
  float toward(int key, bool active, double duration);
  // Raw linear progress, for a property that should not be eased twice.
  float linear(int key, bool active, double duration);
  // A one-shot that runs from 0 to 1 once and stays there: `animation: … both`.
  float once(int key, double duration);
  void reset(int key);
  void clear();

  static int key(int tag, int index = 0) { return tag * 4096 + index; }

 private:
  struct Entry {
    int key = 0;
    float value = 0;
    bool touched = false;
  };
  Entry& entry(int key);
  std::vector<Entry> entries_;
  double dt_ = 0;
};

// CSS cubic-bezier(0.25, 0.1, 0.25, 1.0) — the `ease` default — evaluated for x.
float easeDefault(float t);
// cubic-bezier(.2, .7, .2, 1), the menu card's entrance curve.
float easeMenuIn(float t);

namespace widget {

enum class ButtonKind { Normal, Primary, Danger };

// ---------------------------------------------------------------------------
// Cards and panels
// ---------------------------------------------------------------------------

// .screen — a full-viewport flex centring box with the radial wash behind it.
Style screen();
// .screen.dim
Style screenDim();
// .menu-card / .menu-card.wide
Style menuCard(bool wide = false);
// .menu-card.menu-glass, whose backdrop blur is drawn separately.
Style glassCard();
// .inv-panel
Style invPanel();

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

// .btn — full width, 12px 18px padding, 8px vertical margin.
Style btn(bool hovered, bool active, ButtonKind kind = ButtonKind::Normal);
// .btn.small — inline, auto width.
Style btnSmall(bool hovered, bool active, ButtonKind kind = ButtonKind::Normal);
// #menu-root > .btn — uppercase, letter-spaced, with the sliding accent bar.
Style menuButton(bool hovered, ButtonKind kind = ButtonKind::Normal);
// .settings-tab
Style settingsTab(bool active, bool hovered);
// .rb-tab
Style rbTab(bool active, bool hovered);
// .gal-btns button
Style galleryButton(bool hovered, bool danger);
// .rb-arrow
Style rbArrow(bool hovered);

TextStyle btnText(bool hovered, ButtonKind kind);
TextStyle btnSmallText(bool hovered, ButtonKind kind);
TextStyle menuButtonText(bool hovered, ButtonKind kind);

// ---------------------------------------------------------------------------
// Form controls
// ---------------------------------------------------------------------------

// input[type=text]
Style textInput(bool focused);
// .rb-search
Style searchInput(bool focused);
// A range slider's track and its filled portion; the thumb is drawn by the caller.
Style sliderTrack();
// select, rendered as a cycling button since there is no native dropdown.
Style selectBox(bool hovered);

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

enum class SlotKind { Normal, Result, Armor };
// .islot
Style islot(bool hovered, SlotKind kind = SlotKind::Normal);
// .hslot / .hslot.sel
Style hotbarSlot(bool selected);
// .rb-cell / .rb-cell.empty
Style rbCell(bool empty);

// ---------------------------------------------------------------------------
// Text roles
// ---------------------------------------------------------------------------

TextStyle h2();          // h2
TextStyle h3();          // h3
TextStyle subtitle();    // .subtitle
TextStyle glassSubtitle();
TextStyle body();
TextStyle muted(float size = 13.0f);
TextStyle emptyNote();   // .empty-note
TextStyle fieldLabel();  // label.field
TextStyle settingLabel();
TextStyle settingValue();
TextStyle invTitle();    // .inv-title
TextStyle slotCount();   // .slot-count
TextStyle tooltipName(); // .tooltip .tname
TextStyle tooltipDesc(); // .tooltip .tdesc
TextStyle kbd();         // .kbd
TextStyle debug();       // #debug
TextStyle toast();       // .toast
TextStyle worldName();   // .world-row .name
TextStyle worldMeta();   // .world-row .meta
TextStyle versionTag();
TextStyle menuSignature();
TextStyle rbName();
TextStyle rbFoot();
TextStyle galWorld();
TextStyle galDate();
TextStyle galBadge();
TextStyle aboutLead();
TextStyle aboutBody();
TextStyle aboutHeading();
TextStyle featTitle();
TextStyle featBody();
TextStyle controlValue();
TextStyle heldItemLabel();

// ---------------------------------------------------------------------------
// Composites used by more than one screen
// ---------------------------------------------------------------------------

// .tooltip — a cursor-anchored card. Positioned and drawn immediately rather than
// laid out, since it must escape every clip in the tree.
void drawTooltip(Ui2D& ui, Text& text, float mouseX, float mouseY, const std::string& name,
                 const std::vector<std::string>& sub);

// An item icon plus its count and durability bar, the shape shared by the hotbar,
// the inventory grid and the cursor stack.
struct StackVisual {
  IconRef icon;
  int count = 1;
  float duraFraction = -1.0f;  // < 0 hides the bar
};
void drawStack(Ui2D& ui, Text& text, const Rect& slot, const StackVisual& v);

// A segmented bar (.fuel-bar, .slot-dura, the break-progress pip).
void drawBar(Ui2D& ui, const Rect& r, float fraction, Rgba fill, Rgba track,
             float radius = 0.0f);

}  // namespace widget

// An editable single-line field: the world name, the seed, waypoint names, the recipe
// search, and later the join code. There is no DOM <input>, so caret movement,
// selection, clipboard and maxlength all live here.
//
// Focus is tracked by the owning screen (one field can be focused at a time), and while
// focused the Input layer is put into text-capture mode so gameplay keybinds do not fire
// on every letter typed.
class TextField {
 public:
  void setText(std::string s);
  const std::string& text() const { return value_; }
  void clear();

  bool focused() const { return focused_; }
  // Focus changes go through here so text capture is turned on and off in step.
  void setFocused(bool on, Input* input);

  int maxLength = 0;  // 0 = unlimited, in code points
  std::string placeholder;

  // Applies this frame's typed characters and edit keys. Returns true when the value
  // changed, and sets `submitted` when Enter was pressed.
  bool handle(const UiEvent& event, bool& submitted);

  // Draws the value (or the placeholder), the caret and any selection inside `box`.
  void draw(Ui2D& ui, Text& text, const Rect& box, const TextStyle& style, double time);
  // Moves the caret to the code point nearest `x`, for a click inside the field.
  void placeCaretAt(Text& text, const Rect& box, const TextStyle& style, float x);

 private:
  void deleteSelection();
  std::size_t selectionStart() const { return std::min(caret_, anchor_); }
  std::size_t selectionEnd() const { return std::max(caret_, anchor_); }
  int codePointCount() const;

  std::string value_;
  std::size_t caret_ = 0;   // byte index
  std::size_t anchor_ = 0;  // byte index; equal to caret_ means no selection
  bool focused_ = false;
};
}  // namespace hr::ui
