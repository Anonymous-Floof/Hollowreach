#include "ui/paletteui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/input.h"
#include "game/dyeing.h"
#include "game/items.h"
#include "ui/theme.h"

namespace hr::ui {
namespace {

constexpr float kPi = 3.14159265358979f;

// Segments around the hue ring. 180 is two degrees each: smooth enough that the
// sweep has no visible facets, cheap enough to stay one flush.
constexpr int kHueSegments = 180;
// The saturation/value square is drawn as a grid of flat cells, because the shader's
// gradient primitive is one-dimensional and this needs two. 32 across at roughly
// 180 pixels is a shade under six pixels a cell, which at this size reads as a
// continuous field rather than as squares.
constexpr int kSquareCells = 32;

// Widget tags, so hit testing and drawing agree by construction.
constexpr int kTagApply = 1;
constexpr int kTagDone = 2;
constexpr int kTagHex = 3;
constexpr int kTagSaveWorld = 4;
constexpr int kTagSaveGlobal = 5;

Rgba packed(std::uint32_t rgb) {
  return {static_cast<std::uint8_t>((rgb >> 16) & 0xFF),
          static_cast<std::uint8_t>((rgb >> 8) & 0xFF),
          static_cast<std::uint8_t>(rgb & 0xFF), 255};
}

void ringQuad(Ui2D& ui, float cx, float cy, float inner, float outer, float a0, float a1,
              Rgba color) {
  const Vec2 pts[4] = {
      {cx + std::sin(a0) * outer, cy - std::cos(a0) * outer},
      {cx + std::sin(a1) * outer, cy - std::cos(a1) * outer},
      {cx + std::sin(a1) * inner, cy - std::cos(a1) * inner},
      {cx + std::sin(a0) * inner, cy - std::cos(a0) * inner},
  };
  ui.fillPoly(pts, 4, color);
}

bool button(Ui2D& ui, Text& text, const Rect& box, const std::string& label, bool hovered,
            bool enabled, bool primary) {
  Rgba bg = col(Role::PanelRaised);
  Rgba edge = col(Role::Edge);
  Rgba fg = col(Role::Text);
  if (primary) {
    bg = hovered ? col(Role::AccentHi) : col(Role::AccentLo);
    edge = col(Role::AccentEdge);
    fg = hovered ? col(Role::AccentInk) : col(Role::Text);
  } else if (hovered) {
    bg = col(Role::PanelHover);
  }
  if (!enabled) {
    bg = col(Role::SlotFill);
    edge = col(Role::SlotEdge);
    fg = col(Role::Muted);
  }
  ui.fillRect(box, bg, 8.0f);
  ui.strokeRect(box, edge, 1.0f, 8.0f);
  TextStyle ts;
  ts.font = FontId::SansSemibold;
  ts.size = 14.0f;
  ts.color = fg;
  text.drawInBox(ui, box, label, ts, TextAlign::Center);
  return enabled && hovered;
}

void label(Ui2D& ui, Text& text, const Rect& box, const std::string& s, Rgba color,
           float size, TextAlign align) {
  TextStyle ts;
  ts.font = FontId::Sans;
  ts.size = size;
  ts.color = color;
  text.drawInBox(ui, box, s, ts, align);
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

// --- colour space ------------------------------------------------------------

std::uint32_t hsvToRgb(float h, float s, float v) {
  h = h - std::floor(h);
  s = std::clamp(s, 0.0f, 1.0f);
  v = std::clamp(v, 0.0f, 1.0f);
  const float i = std::floor(h * 6.0f);
  const float f = h * 6.0f - i;
  const float p = v * (1.0f - s);
  const float q = v * (1.0f - f * s);
  const float t = v * (1.0f - (1.0f - f) * s);
  float r = 0, g = 0, b = 0;
  switch (static_cast<int>(i) % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  const auto q8 = [](float x) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f));
  };
  return (q8(r) << 16) | (q8(g) << 8) | q8(b);
}

void rgbToHsv(std::uint32_t rgb, float& h, float& s, float& v) {
  const float r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
  const float g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
  const float b = static_cast<float>(rgb & 0xFF) / 255.0f;
  const float mx = std::max({r, g, b});
  const float mn = std::min({r, g, b});
  const float d = mx - mn;
  v = mx;
  s = mx <= 0.0f ? 0.0f : d / mx;
  if (d <= 0.0f) {
    // A grey has no hue at all. Keeping the previous one would be wrong here —
    // this is a fresh read — and 0 is as good as any other arbitrary answer.
    h = 0.0f;
    return;
  }
  if (mx == r) {
    h = (g - b) / d / 6.0f;
  } else if (mx == g) {
    h = (2.0f + (b - r) / d) / 6.0f;
  } else {
    h = (4.0f + (r - g) / d) / 6.0f;
  }
  h = h - std::floor(h);
}

bool parseHex(const std::string& text, std::uint32_t& out) {
  std::string s;
  for (const char c : text) {
    if (c == '#' || c == ' ') continue;
    s.push_back(c);
  }
  if (s.size() != 3 && s.size() != 6) return false;
  std::uint32_t v = 0;
  for (const char c : s) {
    const int d = hexDigit(c);
    if (d < 0) return false;
    v = (v << 4) | static_cast<std::uint32_t>(d);
  }
  if (s.size() == 3) {
    // #abc is #aabbcc, the CSS shorthand. Worth accepting because a player typing a
    // colour from memory is far more likely to reach for it than for six digits.
    const std::uint32_t r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
    v = (r << 20) | (r << 16) | (g << 12) | (g << 8) | (b << 4) | b;
  }
  out = v & 0x00FFFFFFu;
  return true;
}

std::string toHex(std::uint32_t rgb) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "#%06X", rgb & 0x00FFFFFFu);
  return buf;
}

// --- screen ------------------------------------------------------------------

void PaletteUI::open() {
  open_ = true;
  drag_ = Drag::None;
  editingHex_ = false;
  // Reopen on whatever the slot already wears, so adjusting a colour is a nudge
  // rather than starting again from wherever the wheel was left.
  if (slot_ && slot_->dyed()) {
    rgbToHsv(static_cast<std::uint32_t>(slot_->tint), h_, s_, v_);
  }
  hexText_ = toHex(colour());
}

void PaletteUI::close() {
  open_ = false;
  drag_ = Drag::None;
  editingHex_ = false;
}

PaletteUI::Layout PaletteUI::layout(Ui2D& ui) const {
  Layout l;
  const float w = 560.0f, h = 400.0f;
  l.card = {(ui.width() - w) * 0.5f, (ui.height() - h) * 0.5f, w, h};

  // Left half: the wheel.
  l.cx = l.card.x + 150.0f;
  l.cy = l.card.y + 190.0f;
  l.outer = 118.0f;
  l.inner = 94.0f;
  // The square inscribed in the inner circle, with a little air.
  const float half = l.inner * 0.68f;
  l.square = {l.cx - half, l.cy - half, half * 2, half * 2};

  // Right half: the item, the readouts, the favourites.
  const float rx = l.card.x + 310.0f;
  l.slot = {rx, l.card.y + 56.0f, 52.0f, 52.0f};
  l.preview = {rx + 64.0f, l.card.y + 56.0f, 52.0f, 52.0f};
  l.hex = {rx, l.card.y + 122.0f, 116.0f, 30.0f};

  // The cost line sits at +160 and each favourites block needs its own title above
  // its swatches, so the rows start well clear of it. The first draft put the world
  // title at +164 and it printed straight through the cost.
  l.worldRow = {rx, l.card.y + 212.0f, 200.0f, 26.0f};
  l.saveWorld = {rx + 152.0f, l.card.y + 188.0f, 62.0f, 22.0f};
  l.globalRow = {rx, l.card.y + 272.0f, 200.0f, 26.0f};
  l.saveGlobal = {rx + 152.0f, l.card.y + 248.0f, 62.0f, 22.0f};

  l.apply = {rx, l.card.y + h - 66.0f, 104.0f, 34.0f};
  l.done = {rx + 112.0f, l.card.y + h - 66.0f, 104.0f, 34.0f};
  return l;
}

int PaletteUI::favouriteAt(const Layout& lay, float mx, float my, bool& global) const {
  const auto hit = [&](const Rect& row, const std::vector<std::uint32_t>* list) {
    if (list == nullptr || my < row.y || my > row.bottom()) return -1;
    const int i = static_cast<int>((mx - row.x) / 28.0f);
    if (mx < row.x || i < 0 || i >= static_cast<int>(list->size())) return -1;
    return i;
  };
  global = false;
  const int w = hit(lay.worldRow, worldFavourites);
  if (w >= 0) return w;
  global = true;
  return hit(lay.globalRow, globalFavourites);
}

void PaletteUI::applyNow() {
  if (!inv_ || !slot_) return;
  game::applyDye(*inv_, *slot_, colour());
}

void PaletteUI::update(Ui2D& ui, Text& text, const UiEvent& event) {
  (void)text;
  if (!open_) return;
  const Layout lay = layout(ui);
  const float mx = event.mouseX, my = event.mouseY;

  hover_ = 0;
  const auto inside = [&](const Rect& r) {
    return mx >= r.x && mx <= r.right() && my >= r.y && my <= r.bottom();
  };
  if (inside(lay.apply)) hover_ = kTagApply;
  else if (inside(lay.done)) hover_ = kTagDone;
  else if (inside(lay.hex)) hover_ = kTagHex;
  else if (inside(lay.saveWorld)) hover_ = kTagSaveWorld;
  else if (inside(lay.saveGlobal)) hover_ = kTagSaveGlobal;

  const float dx = mx - lay.cx, dy = my - lay.cy;
  const float dist = std::sqrt(dx * dx + dy * dy);

  if (event.leftDown) {
    if (drag_ == Drag::None) {
      if (dist <= lay.outer + 6.0f && dist >= lay.inner - 6.0f) {
        drag_ = Drag::Hue;
        editingHex_ = false;
      } else if (inside(lay.square)) {
        drag_ = Drag::Square;
        editingHex_ = false;
      }
    }
    if (drag_ == Drag::Hue) {
      float a = std::atan2(dx, -dy) / (kPi * 2.0f);
      h_ = a - std::floor(a);
      hexText_ = toHex(colour());
    } else if (drag_ == Drag::Square) {
      s_ = std::clamp((mx - lay.square.x) / lay.square.w, 0.0f, 1.0f);
      v_ = std::clamp(1.0f - (my - lay.square.y) / lay.square.h, 0.0f, 1.0f);
      hexText_ = toHex(colour());
    }
  } else {
    drag_ = Drag::None;
  }

  if (event.leftClick) {
    if (hover_ == kTagApply) {
      applyNow();
    } else if (hover_ == kTagDone) {
      if (onClose) onClose();
    } else if (hover_ == kTagHex) {
      editingHex_ = true;
      hexText_ = toHex(colour());
    } else if (hover_ == kTagSaveWorld && worldFavourites) {
      // Deduplicated, and newest first. A favourites row that fills up with the same
      // colour eight times because the button was clicked twice is not a feature.
      const std::uint32_t c = colour();
      auto& list = *worldFavourites;
      list.erase(std::remove(list.begin(), list.end(), c), list.end());
      list.insert(list.begin(), c);
      if (list.size() > 7) list.resize(7);
      if (onFavouritesChanged) onFavouritesChanged(false);
    } else if (hover_ == kTagSaveGlobal && globalFavourites) {
      const std::uint32_t c = colour();
      auto& list = *globalFavourites;
      list.erase(std::remove(list.begin(), list.end(), c), list.end());
      list.insert(list.begin(), c);
      if (list.size() > 7) list.resize(7);
      if (onFavouritesChanged) onFavouritesChanged(true);
    } else {
      bool global = false;
      const int fav = favouriteAt(lay, mx, my, global);
      if (fav >= 0) {
        const std::vector<std::uint32_t>* list = global ? globalFavourites : worldFavourites;
        rgbToHsv((*list)[static_cast<std::size_t>(fav)], h_, s_, v_);
        hexText_ = toHex(colour());
        editingHex_ = false;
      }
    }
  }
}

void PaletteUI::onChar(unsigned int codepoint) {
  if (!editingHex_) return;
  if (codepoint > 127) return;
  const char c = static_cast<char>(codepoint);
  if (hexDigit(c) < 0 && c != '#') return;
  if (hexText_.size() >= 7) return;
  hexText_.push_back(c);
}

void PaletteUI::onKey(int key) {
  if (!editingHex_) return;
  if (key == static_cast<int>(Key::Backspace)) {
    if (!hexText_.empty()) hexText_.pop_back();
    return;
  }
  if (key == static_cast<int>(Key::Enter)) {
    std::uint32_t v = 0;
    if (parseHex(hexText_, v)) {
      rgbToHsv(v, h_, s_, v_);
    }
    // Either way the field goes back to showing the colour that is actually
    // selected, so a typo cannot leave the box disagreeing with the wheel.
    hexText_ = toHex(colour());
    editingHex_ = false;
    return;
  }
  if (key == static_cast<int>(Key::Escape)) {
    hexText_ = toHex(colour());
    editingHex_ = false;
  }
}

void PaletteUI::draw(Ui2D& ui, Text& text) {
  if (!open_) return;
  const Layout lay = layout(ui);

  ui.fillRect({0, 0, ui.width(), ui.height()}, col(Role::Scrim));
  ui.fillRect(lay.card, col(Role::Panel), 12.0f);
  ui.strokeRect(lay.card, col(Role::Edge), 1.0f, 12.0f);

  label(ui, text, {lay.card.x + 20, lay.card.y + 14, 300, 22}, "Dyer's Palette",
        col(Role::Text), 18.0f, TextAlign::Left);

  // The hue ring.
  for (int i = 0; i < kHueSegments; ++i) {
    const float t0 = static_cast<float>(i) / kHueSegments;
    const float t1 = static_cast<float>(i + 1) / kHueSegments;
    ringQuad(ui, lay.cx, lay.cy, lay.inner, lay.outer, t0 * kPi * 2.0f, t1 * kPi * 2.0f,
             packed(hsvToRgb(t0, 1.0f, 1.0f)));
  }
  // The hue handle.
  {
    const float a = h_ * kPi * 2.0f;
    const float r = (lay.inner + lay.outer) * 0.5f;
    const float hx = lay.cx + std::sin(a) * r, hy = lay.cy - std::cos(a) * r;
    ui.fillRect({hx - 7, hy - 7, 14, 14}, col(Role::Text), 7.0f);
    ui.fillRect({hx - 5, hy - 5, 10, 10}, packed(hsvToRgb(h_, 1.0f, 1.0f)), 5.0f);
  }

  // The saturation / value field, as a grid of flat cells.
  {
    const float cw = lay.square.w / kSquareCells;
    const float ch = lay.square.h / kSquareCells;
    for (int y = 0; y < kSquareCells; ++y) {
      for (int x = 0; x < kSquareCells; ++x) {
        const float s = (static_cast<float>(x) + 0.5f) / kSquareCells;
        const float v = 1.0f - (static_cast<float>(y) + 0.5f) / kSquareCells;
        // Half a pixel of overlap, or the seams between cells show as a grid.
        ui.fillRect({lay.square.x + x * cw, lay.square.y + y * ch, cw + 0.5f, ch + 0.5f},
                    packed(hsvToRgb(h_, s, v)));
      }
    }
    ui.strokeRect(lay.square, col(Role::Edge), 1.0f);
    const float px = lay.square.x + s_ * lay.square.w;
    const float py = lay.square.y + (1.0f - v_) * lay.square.h;
    ui.strokeRect({px - 6, py - 6, 12, 12}, col(Role::Text), 2.0f, 6.0f);
  }

  // The item slot and the colour it would become.
  const bool has = slot_ != nullptr && !slot_->empty();
  const bool dyeable = slot_ != nullptr && game::isDyeable(*slot_);
  ui.fillRect(lay.slot, col(Role::SlotFill), 6.0f);
  ui.strokeRect(lay.slot, dyeable ? col(Role::AccentEdge) : col(Role::SlotEdge), 1.0f, 6.0f);
  if (has && icons_ != nullptr) {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    if (icons_->uvFor(slot_->key, u0, v0, u1, v1)) {
      // Tinted by what the stack ALREADY wears, not by the wheel. The swatch beside
      // it is the colour being chosen; this is the thing as it stands, so the two
      // together read as before and after.
      const Rgba tint = slot_->dyed() ? packed(static_cast<std::uint32_t>(slot_->tint))
                                      : Rgba {255, 255, 255, 255};
      ui.texturedRect(lay.slot.inset(6), u0, v0, u1, v1, tint);
    }
    if (slot_->count > 1) {
      label(ui, text, {lay.slot.x, lay.slot.bottom() - 16, lay.slot.w - 4, 14},
            std::to_string(slot_->count), col(Role::Text), 12.0f, TextAlign::Right);
    }
  }
  ui.fillRect(lay.preview, packed(colour()), 6.0f);
  ui.strokeRect(lay.preview, col(Role::Edge), 1.0f, 6.0f);

  // The hex field.
  ui.fillRect(lay.hex, col(Role::SlotFill), 6.0f);
  ui.strokeRect(lay.hex, editingHex_ ? col(Role::AccentEdge) : col(Role::Edge), 1.0f, 6.0f);
  label(ui, text, lay.hex.inset(6), editingHex_ ? hexText_ + "_" : toHex(colour()),
        col(Role::Text), 14.0f, TextAlign::Left);

  // What this would cost, named. A palette that silently refuses because the wrong
  // dye ran out is indistinguishable from one that is broken.
  {
    std::string line;
    Rgba tone = col(Role::Muted);
    if (!has) {
      line = "Put something dyeable in the slot";
    } else if (!dyeable) {
      line = slot_->key + " cannot be dyed";
    } else if (slot_->count > game::kDyePerApplication) {
      line = "Too many: one dye colours " + std::to_string(game::kDyePerApplication);
    } else if (inv_) {
      const game::DyeCost cost = game::dyeCostFor(*inv_, colour());
      const game::ItemDef* def = game::getItem(cost.dyeKey);
      line = "Costs 1 " + std::string(def ? def->name : cost.dyeKey) + "  (you have " +
             std::to_string(cost.have) + ")";
      tone = cost.affordable ? col(Role::Text) : col(Role::Danger);
    }
    label(ui, text, {lay.card.x + 310, lay.card.y + 158, 230, 18}, line, tone, 12.0f,
          TextAlign::Left);
  }

  // Favourites, two rows. World above, global below, each with its own save.
  const auto favRow = [&](const Rect& row, const std::vector<std::uint32_t>* list,
                          const char* title, const Rect& save, int saveTag) {
    label(ui, text, {row.x, row.y - 22, 140, 16}, title, col(Role::Muted), 12.0f,
          TextAlign::Left);
    button(ui, text, save, "Save", hover_ == saveTag, true, false);
    if (list == nullptr || list->empty()) {
      label(ui, text, {row.x, row.y + 4, 200, 16}, "none yet", col(Role::Muted), 12.0f,
            TextAlign::Left);
      return;
    }
    for (std::size_t i = 0; i < list->size(); ++i) {
      const Rect sw {row.x + static_cast<float>(i) * 28.0f, row.y, 24.0f, 24.0f};
      ui.fillRect(sw, packed((*list)[i]), 4.0f);
      ui.strokeRect(sw, col(Role::Edge), 1.0f, 4.0f);
    }
  };
  favRow(lay.worldRow, worldFavourites, "Saved in this world", lay.saveWorld, kTagSaveWorld);
  favRow(lay.globalRow, globalFavourites, "Saved everywhere", lay.saveGlobal, kTagSaveGlobal);

  const bool canApply =
      dyeable && inv_ && slot_->count <= game::kDyePerApplication &&
      game::dyeCostFor(*inv_, colour()).affordable;
  button(ui, text, lay.apply, "Dye", hover_ == kTagApply, canApply, true);
  button(ui, text, lay.done, "Done", hover_ == kTagDone, true, false);
}


// --- embedded in the inventory screen ----------------------------------------

namespace {

// The wheel's geometry inside a reserved box, so drawing and hit-testing cannot
// disagree about where the ring is.
struct Dial {
  float cx = 0, cy = 0, outer = 0, inner = 0;
  Rect square;
};

Dial dialFor(const Rect& box) {
  Dial d;
  d.cx = box.centerX();
  d.cy = box.centerY();
  d.outer = std::min(box.w, box.h) * 0.5f - 2.0f;
  d.inner = d.outer - 24.0f;
  const float half = d.inner * 0.68f;
  d.square = {d.cx - half, d.cy - half, half * 2, half * 2};
  return d;
}

// Where the readouts sit relative to the dial. One function, so draw and hit-test
// cannot drift apart — the standalone screen kept two copies of these numbers and
// the cost line ended up printed through the favourites heading.
struct SidePanel {
  Rect swatch, apply, saveWorld, saveGlobal;
  float hexY = 0, costY = 0, worldY = 0, globalY = 0, x = 0;
};

// Everything measured from the box the LAYOUT gave us, so the panel that contains
// it has already been sized to fit. Measuring from the wheel instead is what put the
// buttons off the edge of the card.
SidePanel sideFor(const Rect& side) {
  SidePanel p;
  p.x = side.x;
  const float y = side.y + 6.0f;
  p.swatch = {p.x, y, 52, 52};
  p.hexY = y + 16.0f;
  p.costY = y + 62.0f;
  p.worldY = y + 88.0f;
  p.globalY = y + 142.0f;
  p.apply = {p.x, y + 196.0f, 104, 32};
  p.saveWorld = {p.x + 112.0f, y + 196.0f, 76, 32};
  p.saveGlobal = {p.x + 196.0f, y + 196.0f, 76, 32};
  return p;
}

}  // namespace

void PaletteUI::drawInto(Ui2D& ui, Text& text, const Rect& box, const Rect& side,
                         const game::Inventory* inv, const game::ItemStack* slot) {
  const Dial d = dialFor(box);
  const SidePanel p = sideFor(side);

  for (int i = 0; i < kHueSegments; ++i) {
    const float t0 = static_cast<float>(i) / kHueSegments;
    const float t1 = static_cast<float>(i + 1) / kHueSegments;
    ringQuad(ui, d.cx, d.cy, d.inner, d.outer, t0 * kPi * 2.0f, t1 * kPi * 2.0f,
             packed(hsvToRgb(t0, 1.0f, 1.0f)));
  }
  {
    const float a = h_ * kPi * 2.0f;
    const float r = (d.inner + d.outer) * 0.5f;
    const float hx = d.cx + std::sin(a) * r, hy = d.cy - std::cos(a) * r;
    ui.fillRect({hx - 7, hy - 7, 14, 14}, col(Role::Text), 7.0f);
    ui.fillRect({hx - 5, hy - 5, 10, 10}, packed(hsvToRgb(h_, 1.0f, 1.0f)), 5.0f);
  }
  {
    const float cw = d.square.w / kSquareCells;
    const float ch = d.square.h / kSquareCells;
    for (int y = 0; y < kSquareCells; ++y) {
      for (int x = 0; x < kSquareCells; ++x) {
        const float s = (static_cast<float>(x) + 0.5f) / kSquareCells;
        const float v = 1.0f - (static_cast<float>(y) + 0.5f) / kSquareCells;
        ui.fillRect({d.square.x + x * cw, d.square.y + y * ch, cw + 0.5f, ch + 0.5f},
                    packed(hsvToRgb(h_, s, v)));
      }
    }
    ui.strokeRect(d.square, col(Role::Edge), 1.0f);
    const float sx = d.square.x + s_ * d.square.w;
    const float sy = d.square.y + (1.0f - v_) * d.square.h;
    ui.strokeRect({sx - 6, sy - 6, 12, 12}, col(Role::Text), 2.0f, 6.0f);
  }

  ui.fillRect(p.swatch, packed(colour()), 6.0f);
  ui.strokeRect(p.swatch, col(Role::Edge), 1.0f, 6.0f);
  label(ui, text, {p.x + 62, p.hexY, 120, 20}, toHex(colour()), col(Role::Text), 14.0f,
        TextAlign::Left);

  std::string line;
  Rgba tone = col(Role::Muted);
  const bool dyeable = slot != nullptr && game::isDyeable(*slot);
  if (slot == nullptr || slot->empty()) {
    line = "The slot is empty";
  } else if (!dyeable) {
    line = slot->key + " cannot be dyed";
  } else if (slot->count > game::kDyePerApplication) {
    line = "Too many: one dye colours " + std::to_string(game::kDyePerApplication);
  } else if (inv != nullptr) {
    const game::DyeCost cost = game::dyeCostFor(*inv, colour());
    const game::ItemDef* def = game::getItem(cost.dyeKey);
    line = "Costs 1 " + std::string(def ? def->name : cost.dyeKey) + "  (you have " +
           std::to_string(cost.have) + ")";
    tone = cost.affordable ? col(Role::Text) : col(Role::Danger);
  }
  label(ui, text, {p.x, p.costY, 280, 18}, line, tone, 12.0f, TextAlign::Left);

  const auto favRow = [&](float y, const std::vector<std::uint32_t>* list, const char* title) {
    label(ui, text, {p.x, y, 240, 16}, title, col(Role::Muted), 12.0f, TextAlign::Left);
    if (list == nullptr || list->empty()) {
      label(ui, text, {p.x, y + 20, 200, 16}, "none yet", col(Role::Muted), 12.0f,
            TextAlign::Left);
      return;
    }
    for (std::size_t i = 0; i < list->size(); ++i) {
      const Rect sw {p.x + static_cast<float>(i) * 28.0f, y + 20, 24.0f, 24.0f};
      ui.fillRect(sw, packed((*list)[i]), 4.0f);
      ui.strokeRect(sw, col(Role::Edge), 1.0f, 4.0f);
    }
  };
  favRow(p.worldY, worldFavourites, "Saved in this world");
  favRow(p.globalY, globalFavourites, "Saved everywhere");

  const bool canApply = dyeable && inv != nullptr && slot->count <= game::kDyePerApplication &&
                        game::dyeCostFor(*inv, colour()).affordable;
  button(ui, text, p.apply, "Dye", hover_ == kTagApply, canApply, true);
  button(ui, text, p.saveWorld, "Save here", hover_ == kTagSaveWorld, true, false);
  button(ui, text, p.saveGlobal, "Save all", hover_ == kTagSaveGlobal, true, false);
}

bool PaletteUI::updateIn(const UiEvent& event, const Rect& box, const Rect& side,
                         game::Inventory& inv, game::ItemStack* slot) {
  const Dial d = dialFor(box);
  const SidePanel p = sideFor(side);
  const float mx = event.mouseX, my = event.mouseY;

  const auto inside = [&](const Rect& r) {
    return mx >= r.x && mx <= r.right() && my >= r.y && my <= r.bottom();
  };

  hover_ = 0;
  if (inside(p.apply)) {
    hover_ = kTagApply;
  } else if (inside(p.saveWorld)) {
    hover_ = kTagSaveWorld;
  } else if (inside(p.saveGlobal)) {
    hover_ = kTagSaveGlobal;
  }

  const float dx = mx - d.cx, dy = my - d.cy;
  const float dist = std::sqrt(dx * dx + dy * dy);

  if (event.leftDown) {
    if (drag_ == Drag::None) {
      if (dist <= d.outer + 6.0f && dist >= d.inner - 6.0f) {
        drag_ = Drag::Hue;
      } else if (inside(d.square)) {
        drag_ = Drag::Square;
      }
    }
    if (drag_ == Drag::Hue) {
      const float a = std::atan2(dx, -dy) / (kPi * 2.0f);
      h_ = a - std::floor(a);
      return true;
    }
    if (drag_ == Drag::Square) {
      s_ = std::clamp((mx - d.square.x) / d.square.w, 0.0f, 1.0f);
      v_ = std::clamp(1.0f - (my - d.square.y) / d.square.h, 0.0f, 1.0f);
      return true;
    }
  } else {
    drag_ = Drag::None;
  }

  if (!event.leftClick) return false;

  if (hover_ == kTagApply) {
    if (slot != nullptr) game::applyDye(inv, *slot, colour());
    return true;
  }
  const auto remember = [this](std::vector<std::uint32_t>* list, bool global) {
    if (list == nullptr) return;
    const std::uint32_t c = colour();
    list->erase(std::remove(list->begin(), list->end(), c), list->end());
    list->insert(list->begin(), c);
    if (list->size() > 7) list->resize(7);
    if (onFavouritesChanged) onFavouritesChanged(global);
  };
  if (hover_ == kTagSaveWorld) {
    remember(worldFavourites, false);
    return true;
  }
  if (hover_ == kTagSaveGlobal) {
    remember(globalFavourites, true);
    return true;
  }

  // A saved swatch, in either row.
  const auto pick = [&](float y, const std::vector<std::uint32_t>* list) {
    if (list == nullptr || my < y + 20 || my > y + 44) return false;
    const int i = static_cast<int>((mx - p.x) / 28.0f);
    if (mx < p.x || i < 0 || i >= static_cast<int>(list->size())) return false;
    rgbToHsv((*list)[static_cast<std::size_t>(i)], h_, s_, v_);
    return true;
  };
  if (pick(p.worldY, worldFavourites)) return true;
  if (pick(p.globalY, globalFavourites)) return true;
  return false;
}

}  // namespace hr::ui
