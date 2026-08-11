// A tiny flex/grid layout engine and the node tree the screens are built from.
//
// The web build declared its interface in HTML and let the browser lay it out. The
// stylesheet is flex-heavy — `space-between`, `align-items: center`, gaps, and grids
// with `auto-fill minmax()` — and none of that can be produced by one-pass cursor
// stacking, because centring a row means knowing its width before you place its
// first child. So this is a real two-pass layout: measure intrinsic sizes bottom-up,
// then place top-down.
//
// The tree is rebuilt from scratch every frame, exactly the way the JS rebuilt
// innerHTML on every state change. That sounds wasteful and is not: a full screen is
// a few hundred nodes, and it means there is no diffing and no stale-cache class of
// bug. Everything that must survive a rebuild — hover, scroll offset, focus, which
// tab is open, tween progress — lives outside the tree, keyed by a caller-supplied
// tag, which is also what makes hit testing a single function.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/gl.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "ui/ui2d.h"

namespace hr::ui {

inline constexpr float kAuto = -1.0f;
inline constexpr float kNone = 1e9f;

struct Edges {
  float top = 0, right = 0, bottom = 0, left = 0;

  Edges() = default;
  explicit Edges(float all) : top(all), right(all), bottom(all), left(all) {}
  Edges(float vertical, float horizontal)
      : top(vertical), right(horizontal), bottom(vertical), left(horizontal) {}
  Edges(float t, float r, float b, float l) : top(t), right(r), bottom(b), left(l) {}

  float horizontal() const { return left + right; }
  float vertical() const { return top + bottom; }
};

enum class Display : std::uint8_t { Block, Flex, Grid };
enum class Dir : std::uint8_t { Row, Column };
enum class Justify : std::uint8_t { Start, Center, End, SpaceBetween };
enum class Align : std::uint8_t { Stretch, Start, Center, End, Baseline };

struct Style {
  Display display = Display::Block;
  Dir dir = Dir::Column;
  Justify justify = Justify::Start;
  Align align = Align::Stretch;
  bool wrap = false;

  float gap = 0;
  float rowGap = kAuto;  // falls back to `gap`

  Edges padding;
  Edges margin;

  float width = kAuto, height = kAuto;
  float minWidth = 0, maxWidth = kNone;
  float minHeight = 0, maxHeight = kNone;
  float grow = 0, shrink = 1;
  // align-self, overriding the parent's align-items.
  Align alignSelf = Align::Stretch;
  bool hasAlignSelf = false;

  // Grid. gridCols == 0 means auto-fill by gridMinCol; gridColWidth > 0 pins the
  // track width (repeat(9, 46px)); gridFirstColAuto is the `auto 1fr` two-column
  // form the controls table uses.
  int gridCols = 0;
  float gridColWidth = 0;
  float gridMinCol = 0;
  bool gridFirstColAuto = false;

  // Paint. `bg2` plus `gradient` is linear-gradient(180deg, bg, bg2).
  Rgba bg = kTransparent;
  Rgba bg2 = kTransparent;
  bool gradient = false;
  Rgba border = kTransparent;
  float borderWidth = 0;
  float radius = 0;
  BoxShadow shadow;
  // inset 0 1px 0 rgba(255,255,255,0.06) — the glass card's top highlight.
  Rgba insetHighlight = kTransparent;

  // overflow-y: auto. Children are clipped and shifted by the scroll offset held
  // against this node's tag.
  bool scrollY = false;
  // white-space: nowrap; overflow: hidden; text-overflow: ellipsis — a label that is too
  // wide for its box is cut and given a trailing ellipsis instead of wrapping. Needs a
  // maxWidth or an explicit width to have anything to overflow.
  bool ellipsis = false;

  // Multiplies every colour and glyph alpha in this subtree.
  float opacity = 1.0f;
  // transform: translate(...), applied to this node and its subtree after layout.
  float translateX = 0, translateY = 0;

  // A resource pack's nine-slice for this box, or nullptr for the ordinary case of
  // drawing it as a rounded rectangle. When present it replaces the fill, the
  // gradient and the border — a sprite of carved stone already HAS an edge, and
  // stroking a 2px accent line around it would be drawing this engine's idea of a
  // border on top of somebody else's. The shadow is still drawn, because that sits
  // outside the box and belongs to the layout rather than to the material.
  const struct UiSprite* sprite = nullptr;

  // "this node is a <button>". The web build hung one listener on the document and
  // played its click tick whenever `e.target.closest("button")` matched, so the
  // sound came for free on every button on every screen. Doc::clickedButton asks
  // the same question of the node tree; the widget builders set the flag.
  bool isButton = false;
};

// What a leaf paints. Containers use None and only paint their box decoration.
enum class Content : std::uint8_t { None, Text, Icon, Custom };

struct IconRef {
  GLuint texture = 0;
  float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  Rgba tint = kWhite;
};

struct Node {
  Style style;
  int parent = -1, firstChild = -1, lastChild = -1, nextSibling = -1;

  Content content = Content::None;
  std::string text;
  TextStyle textStyle;
  TextAlign textAlign = TextAlign::Left;
  IconRef icon;

  // Caller identity, for hit testing and retained state. 0 means "not interactive".
  int tag = 0;
  int index = 0;

  // Computed by layout().
  Rect rect;          // border box in layout px
  float measuredW = 0, measuredH = 0;
  float baseline = 0;  // ascent of the node's own text, for align-items: baseline
  float opacity = 1.0f;
  Rect clipRect;
  bool clipped = false;
};

// Where the scroll offset and hover state for a tagged node live between frames.
struct ScrollState {
  float offset = 0;
  float contentHeight = 0;
  float viewHeight = 0;
};

class Doc {
 public:
  void reset(Text* text);

  // --- building ----------------------------------------------------------
  int begin(const Style& style, int tag = 0, int index = 0);
  void end();

  int box(const Style& style, int tag = 0, int index = 0);
  int label(const std::string& s, const TextStyle& ts, const Style& style = {}, int tag = 0);
  int icon(const IconRef& ref, float w, float h, const Style& style = {}, int tag = 0,
           int index = 0);
  // A leaf the caller paints itself in a later pass (the map canvas, a bar fill).
  int custom(const Style& style, int tag = 0, int index = 0);

  // Convenience for the two shapes the stylesheet uses constantly.
  static Style row(float gap = 0, Justify justify = Justify::Start,
                   Align align = Align::Center);
  static Style column(float gap = 0, Align align = Align::Stretch);

  // --- layout ------------------------------------------------------------
  // Lays the root out inside `viewport`, honouring the root's own justify/align so
  // `.screen { display:flex; align-items:center; justify-content:center }` works.
  void layout(const Rect& viewport);

  // --- query -------------------------------------------------------------
  int count() const { return static_cast<int>(nodes_.size()); }
  const Node& node(int i) const { return nodes_[i]; }
  Node& node(int i) { return nodes_[i]; }
  int root() const { return nodes_.empty() ? -1 : 0; }

  // Deepest tagged node containing the point and not clipped away. Returns -1 when
  // nothing interactive is under the cursor.
  int hitTest(float x, float y) const;
  // Whether `node` — or anything it sits inside — was built as a button. -1 is a
  // miss and answers false, so a screen can pass a hitTest result straight in.
  bool clickedButton(int node) const;
  // Rect of the first node with this tag (and index), or an empty rect.
  Rect rectOf(int tag, int index = 0) const;
  int findTag(int tag, int index = 0) const;

  // Scroll state, created on demand. Owned by the Doc's user (a screen), which is
  // why it is keyed rather than stored on the node.
  ScrollState& scroll(int tag);
  const ScrollState* scrollIfAny(int tag) const;

  // --- painting ----------------------------------------------------------
  // Walks the tree in document order and draws every node's box and content.
  void paint(Ui2D& ui) const;
  // Paints a subtree only, for a screen that needs to interleave custom drawing.
  void paintSubtree(Ui2D& ui, int node) const;

 private:
  float measure(int i, float availableWidth);
  void place(int i, Rect content);
  void placeFlex(int i, const Rect& content);
  void placeGrid(int i, const Rect& content);
  void applyTranslate(int i, float dx, float dy);
  void applyOpacity(int i, float parentOpacity);
  void applyClip(int i, Rect current);
  void paintNode(Ui2D& ui, const Node& n) const;

  std::vector<Node> nodes_;
  std::vector<int> stack_;
  std::vector<std::pair<int, ScrollState>> scrolls_;
  Text* text_ = nullptr;
};

}  // namespace hr::ui
