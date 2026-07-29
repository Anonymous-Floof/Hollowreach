#include "ui/widgets.h"

#include <algorithm>
#include <cmath>

namespace hr::ui {
namespace {

// Cubic Bezier with fixed endpoints (0,0) and (1,1): solve for t given x, then
// evaluate y. Three Newton steps is plenty at this precision.
float bezier(float x, float x1, float y1, float x2, float y2) {
  x = std::min(std::max(x, 0.0f), 1.0f);
  const auto sampleX = [&](float t) {
    const float u = 1.0f - t;
    return 3.0f * u * u * t * x1 + 3.0f * u * t * t * x2 + t * t * t;
  };
  const auto sampleDX = [&](float t) {
    const float u = 1.0f - t;
    return 3.0f * u * u * x1 + 6.0f * u * t * (x2 - x1) + 3.0f * t * t * (1.0f - x2);
  };
  float t = x;
  for (int i = 0; i < 4; ++i) {
    const float d = sampleX(t) - x;
    const float slope = sampleDX(t);
    if (std::fabs(slope) < 1e-5f) break;
    t -= d / slope;
    t = std::min(std::max(t, 0.0f), 1.0f);
  }
  const float u = 1.0f - t;
  return 3.0f * u * u * t * y1 + 3.0f * u * t * t * y2 + t * t * t;
}

Rgba lerpColor(Rgba a, Rgba b, float t) {
  const auto mix = [t](std::uint8_t x, std::uint8_t y) {
    return static_cast<std::uint8_t>(
        std::lround(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t));
  };
  return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
}

}  // namespace

float easeDefault(float t) { return bezier(t, 0.25f, 0.1f, 0.25f, 1.0f); }
float easeMenuIn(float t) { return bezier(t, 0.2f, 0.7f, 0.2f, 1.0f); }

void TweenStore::beginFrame(double dt) {
  dt_ = dt;
  // Anything the interface stopped asking about is gone; without this the store
  // would grow by one entry per inventory slot per screen visit.
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [](const Entry& e) { return !e.touched; }),
                 entries_.end());
  for (Entry& e : entries_) e.touched = false;
}

TweenStore::Entry& TweenStore::entry(int key) {
  for (Entry& e : entries_) {
    if (e.key == key) {
      e.touched = true;
      return e;
    }
  }
  entries_.push_back({key, 0.0f, true});
  return entries_.back();
}

float TweenStore::linear(int key, bool active, double duration) {
  Entry& e = entry(key);
  if (duration <= 0.0) {
    e.value = active ? 1.0f : 0.0f;
    return e.value;
  }
  const float step = static_cast<float>(dt_ / duration);
  e.value += active ? step : -step;
  e.value = std::min(std::max(e.value, 0.0f), 1.0f);
  return e.value;
}

float TweenStore::toward(int key, bool active, double duration) {
  return easeDefault(linear(key, active, duration));
}

float TweenStore::once(int key, double duration) {
  Entry& e = entry(key);
  if (duration <= 0.0) {
    e.value = 1.0f;
    return 1.0f;
  }
  e.value = std::min(1.0f, e.value + static_cast<float>(dt_ / duration));
  return e.value;
}

void TweenStore::reset(int key) { entry(key).value = 0.0f; }
void TweenStore::clear() { entries_.clear(); }

namespace widget {

// ---------------------------------------------------------------------------
// Cards and panels
// ---------------------------------------------------------------------------

Style screen() {
  Style s = Doc::column(0, Align::Center);
  s.justify = Justify::Center;
  s.display = Display::Flex;
  return s;
}

Style screenDim() { return screen(); }

Style menuCard(bool wide) {
  Style s;
  s.display = Display::Block;
  s.bg = color::panel;
  s.border = color::edge;
  s.borderWidth = 2;
  s.radius = 14;
  s.padding = Edges(28, 32);
  s.minWidth = wide ? 540.0f : 340.0f;
  s.shadow = {0, 18, 50, 0, rgba(0, 0, 0, 0.5)};
  return s;
}

Style glassCard() {
  Style s;
  s.display = Display::Block;
  s.bg = rgba(28, 37, 48, 0.80);
  s.bg2 = rgba(16, 22, 30, 0.86);
  s.gradient = true;
  s.border = rgba(127, 209, 127, 0.22);
  s.borderWidth = 1;
  s.radius = 16;
  s.padding = Edges(30, 34, 24, 34);
  s.minWidth = 360;
  s.shadow = {0, 26, 72, 0, rgba(0, 0, 0, 0.62)};
  s.insetHighlight = rgba(255, 255, 255, 0.06);
  return s;
}

Style invPanel() {
  Style s;
  s.display = Display::Block;
  s.bg = color::panel;
  s.border = color::edge;
  s.borderWidth = 2;
  s.radius = 12;
  s.padding = Edges(16);
  return s;
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

Style btn(bool hovered, bool active, ButtonKind kind) {
  Style s = Doc::row(0, Justify::Center, Align::Center);
  s.padding = Edges(12, 18);
  s.margin = Edges(8, 0);
  s.radius = 9;
  s.borderWidth = 2;
  s.width = kAuto;  // display: block, width: 100% — the parent stretches it
  switch (kind) {
    case ButtonKind::Primary:
      s.bg = hovered ? color::accent : color::accentDark;
      s.border = color::primaryEdge;
      break;
    case ButtonKind::Danger:
      s.bg = hovered ? color::danger : color::panel2;
      s.border = hovered ? color::dangerEdge : color::edge;
      break;
    case ButtonKind::Normal:
      s.bg = hovered ? color::hover : color::panel2;
      s.border = hovered ? color::accentDark : color::edge;
      break;
  }
  // .btn:active { transform: translateY(1px) }
  if (active) s.translateY = 1;
  s.isButton = true;  // <button>: gets the UI click tick
  return s;
}

Style btnSmall(bool hovered, bool active, ButtonKind kind) {
  Style s = btn(hovered, active, kind);
  s.padding = Edges(8, 14);
  s.margin = Edges(4);
  s.isButton = true;  // <button>: gets the UI click tick
  return s;
}

Style menuButton(bool hovered, ButtonKind kind) {
  Style s = Doc::row(0, Justify::Center, Align::Center);
  s.padding = Edges(12, 18);
  s.margin = Edges(8, 0);
  s.radius = 9;
  s.borderWidth = 1;
  if (kind == ButtonKind::Primary) {
    s.gradient = true;
    s.bg = hovered ? color::accent : color::accentDark;
    s.bg2 = hovered ? color::primaryHi : color::primaryLo;
    s.border = color::primaryEdge;
  } else {
    s.bg = hovered ? rgba(58, 78, 100, 0.86) : rgba(38, 50, 64, 0.70);
    s.border = hovered ? rgba(127, 209, 127, 0.42) : rgba(255, 255, 255, 0.08);
  }
  s.isButton = true;  // <button>: gets the UI click tick
  return s;
}

Style settingsTab(bool active, bool hovered) {
  Style s = Doc::row(0, Justify::Center, Align::Center);
  s.padding = Edges(8, 16);
  s.radius = 8;
  s.borderWidth = 2;
  if (active) {
    s.bg = color::accentDark;
    s.border = color::primaryEdge;
  } else {
    s.bg = hovered ? color::hover : color::panel2;
    s.border = color::edge;
  }
  s.isButton = true;  // <button>: gets the UI click tick
  return s;
}

Style rbTab(bool active, bool hovered) {
  Style s = Doc::row(0, Justify::Center, Align::Center);
  s.padding = Edges(6, 10);
  s.radius = 6;
  s.borderWidth = 1;
  if (active) {
    s.bg = color::accent;
    s.border = color::accent;
  } else {
    s.bg = color::panel2;
    s.border = color::edge;
    if (hovered) s.bg = color::hover;
  }
  s.isButton = true;  // <button>: gets the UI click tick
  return s;
}

Style galleryButton(bool hovered, bool danger) {
  Style s = Doc::row(0, Justify::Center, Align::Center);
  s.padding = Edges(5, 4);
  s.radius = 6;
  s.borderWidth = 1;
  s.bg = hovered ? (danger ? color::danger : color::hover) : color::slot;
  s.border = hovered ? (danger ? color::dangerEdge : color::accentDark) : color::slotEdge;
  s.grow = danger ? 0.0f : 1.0f;
  if (danger) s.width = 30;
  s.isButton = true;  // <button>: gets the UI click tick
  return s;
}

Style rbArrow(bool hovered) {
  Style s = Doc::row(0, Justify::Center, Align::Center);
  s.padding = Edges(1, 6);
  s.radius = 4;
  s.borderWidth = 1;
  s.bg = hovered ? color::accent : color::slot;
  s.border = color::slotEdge;
  s.isButton = true;  // <button>: gets the UI click tick
  return s;
}

TextStyle btnText(bool hovered, ButtonKind kind) {
  TextStyle t;
  t.font = FontId::SansSemibold;
  t.size = 16;
  t.letterSpacing = 0.5f;
  t.color = (kind == ButtonKind::Primary && hovered) ? color::onPrimary : color::text;
  return t;
}

TextStyle btnSmallText(bool hovered, ButtonKind kind) {
  TextStyle t = btnText(hovered, kind);
  t.size = 14;
  return t;
}

TextStyle menuButtonText(bool hovered, ButtonKind kind) {
  TextStyle t;
  t.font = FontId::SansSemibold;
  t.size = 15;
  t.letterSpacing = 1.4f;
  t.uppercase = true;
  t.color = (kind == ButtonKind::Primary && hovered) ? color::onPrimaryMenu : color::text;
  return t;
}

// ---------------------------------------------------------------------------
// Form controls
// ---------------------------------------------------------------------------

Style textInput(bool focused) {
  Style s = Doc::row(0, Justify::Start, Align::Center);
  s.padding = Edges(10, 12);
  s.margin = Edges(6, 0);
  s.radius = 8;
  s.borderWidth = 2;
  s.bg = color::inputBg;
  s.border = focused ? color::accentDark : color::inputEdge;
  return s;
}

Style searchInput(bool focused) {
  Style s = Doc::row(0, Justify::Start, Align::Center);
  s.padding = Edges(7, 10);
  s.radius = 6;
  s.borderWidth = 1;
  s.bg = color::panel2;
  s.border = focused ? color::accent : color::edge;
  s.grow = 1;
  s.minWidth = 140;
  return s;
}

Style sliderTrack() {
  Style s;
  s.display = Display::Block;
  s.width = 220;
  s.height = 4;
  s.radius = 2;
  s.bg = color::inputBg;
  return s;
}

Style selectBox(bool hovered) {
  Style s = Doc::row(0, Justify::Center, Align::Center);
  s.padding = Edges(8, 14);
  s.radius = 9;
  s.borderWidth = 2;
  s.bg = hovered ? color::hover : color::panel2;
  s.border = hovered ? color::accentDark : color::edge;
  return s;
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

Style islot(bool hovered, SlotKind kind) {
  Style s;
  s.display = Display::Block;
  s.width = metric::invSlot;
  s.height = metric::invSlot;
  s.radius = 5;
  s.borderWidth = 2;
  s.bg = kind == SlotKind::Result ? color::resultSlot
         : kind == SlotKind::Armor ? color::armorSlot
                                   : color::slot;
  s.border = hovered ? color::accentDark : color::slotEdge;
  return s;
}

Style hotbarSlot(bool selected) {
  Style s;
  s.display = Display::Block;
  s.width = metric::hotbarSlot;
  s.height = metric::hotbarSlot;
  s.radius = 6;
  s.borderWidth = 2;
  s.bg = rgba(20, 26, 33, 0.72);
  s.border = selected ? color::white : color::edge;
  return s;
}

Style rbCell(bool empty) {
  Style s = Doc::row(0, Justify::Center, Align::Center);
  s.width = metric::rbCell;
  s.height = metric::rbCell;
  s.radius = 3;
  s.borderWidth = 1;
  s.bg = empty ? kTransparent : color::slot;
  s.border = empty ? kTransparent : color::slotEdge;
  return s;
}

// ---------------------------------------------------------------------------
// Text roles
// ---------------------------------------------------------------------------

TextStyle h2() {
  TextStyle t;
  t.font = FontId::SansBold;
  t.size = 24;  // browser default h2 is 1.5em of the 16px root
  t.letterSpacing = 1.0f;
  return t;
}

TextStyle h3() {
  TextStyle t;
  t.font = FontId::SansBold;
  t.size = 15;
  t.letterSpacing = 1.0f;
  t.color = color::accent;
  return t;
}

TextStyle subtitle() {
  TextStyle t;
  t.font = FontId::SansItalic;
  t.size = 16;
  t.color = color::muted;
  return t;
}

TextStyle glassSubtitle() {
  TextStyle t = subtitle();
  t.color = color::subtitleGlass;
  t.letterSpacing = 0.4f;
  return t;
}

TextStyle body() {
  TextStyle t;
  t.size = 16;
  return t;
}

TextStyle muted(float size) {
  TextStyle t;
  t.size = size;
  t.color = color::muted;
  return t;
}

TextStyle emptyNote() {
  TextStyle t;
  t.font = FontId::SansItalic;
  t.size = 16;
  t.color = color::muted;
  return t;
}

TextStyle fieldLabel() {
  TextStyle t;
  t.size = 13;
  t.color = color::muted;
  return t;
}

TextStyle settingLabel() {
  TextStyle t;
  t.size = 14;
  return t;
}

TextStyle settingValue() {
  TextStyle t;
  t.size = 13;
  t.color = color::accent;
  return t;
}

TextStyle invTitle() {
  TextStyle t;
  t.size = 14;
  t.letterSpacing = 1.0f;
  t.color = color::accent;
  return t;
}

TextStyle slotCount() {
  TextStyle t;
  t.font = FontId::SansBlack;
  t.size = 13;
  t.color = color::white;
  // text-shadow: 1px 1px 0 #000, -1px 1px 0 #000
  t.withShadow(1, 1, 0, color::black).withShadow(-1, 1, 0, color::black);
  return t;
}

TextStyle tooltipName() {
  TextStyle t;
  t.font = FontId::SansBold;
  t.size = 13;
  return t;
}

TextStyle tooltipDesc() {
  TextStyle t;
  t.size = 12;
  t.color = color::muted;
  return t;
}

TextStyle kbd() {
  TextStyle t;
  t.font = FontId::Mono;
  t.size = 11.5f;
  t.color = color::kbdText;
  return t;
}

TextStyle debug() {
  TextStyle t;
  t.font = FontId::Mono;
  t.size = 12;
  t.color = color::progressFill;
  return t;
}

TextStyle toast() {
  TextStyle t;
  t.size = 13;
  return t;
}

TextStyle worldName() {
  TextStyle t;
  t.font = FontId::SansBold;
  t.size = 16;
  return t;
}

TextStyle worldMeta() {
  TextStyle t;
  t.size = 12;
  t.color = color::muted;
  return t;
}

TextStyle versionTag() {
  TextStyle t;
  t.size = 12;
  t.color = rgba(200, 210, 220, 0.62);
  t.withShadow(0, 1, 3, rgba(0, 0, 0, 0.85));
  return t;
}

TextStyle menuSignature() {
  TextStyle t = versionTag();
  t.font = FontId::SansItalic;
  return t;
}

TextStyle rbName() {
  TextStyle t;
  t.font = FontId::SansSemibold;
  t.size = 13;
  return t;
}

TextStyle rbFoot() {
  TextStyle t;
  t.size = 12;
  t.color = color::muted;
  return t;
}

TextStyle galWorld() {
  TextStyle t;
  t.font = FontId::SansSemibold;
  t.size = 12.5f;
  return t;
}

TextStyle galDate() {
  TextStyle t;
  t.size = 11;
  t.color = color::muted;
  return t;
}

TextStyle galBadge() {
  TextStyle t;
  t.font = FontId::SansBlack;
  t.size = 10;
  t.letterSpacing = 0.5f;
  t.color = color::galBadge;
  return t;
}

TextStyle aboutLead() {
  TextStyle t;
  t.size = 14.5f;
  t.color = color::aboutLead;
  return t;
}

TextStyle aboutBody() {
  TextStyle t;
  t.size = 13.5f;
  t.color = color::aboutText;
  return t;
}

TextStyle aboutHeading() {
  TextStyle t;
  t.font = FontId::SansBold;
  t.size = 12.5f;
  t.letterSpacing = 1.6f;
  t.uppercase = true;
  t.color = color::accent;
  return t;
}

TextStyle featTitle() {
  TextStyle t;
  t.font = FontId::SansBold;
  t.size = 12.5f;
  t.color = color::accent;
  return t;
}

TextStyle featBody() {
  TextStyle t;
  t.size = 11.8f;
  t.color = color::featText;
  return t;
}

TextStyle controlValue() {
  TextStyle t;
  t.size = 13.5f;
  t.color = color::controlValue;
  return t;
}

TextStyle heldItemLabel() {
  TextStyle t;
  t.font = FontId::SansSemibold;
  t.size = 15;
  t.color = color::white;
  t.withShadow(1, 1, 2, color::black);
  return t;
}

// ---------------------------------------------------------------------------
// Composites
// ---------------------------------------------------------------------------

void drawTooltip(Ui2D& ui, Text& text, float mouseX, float mouseY, const std::string& name,
                 const std::vector<std::string>& sub) {
  if (name.empty()) return;
  const TextStyle nameStyle = tooltipName();
  const TextStyle descStyle = tooltipDesc();
  const TextMetrics nameMetrics = text.metrics(nameStyle);
  const TextMetrics descMetrics = text.metrics(descStyle);

  // .tooltip { max-width: 220px; padding: 6px 9px }
  constexpr float kPadX = 9, kPadY = 6, kMaxWidth = 220;
  std::string joined;
  for (std::size_t i = 0; i < sub.size(); ++i) {
    if (i) joined += " \xC2\xB7 ";  // " · "
    joined += sub[i];
  }

  std::vector<std::string> descLines;
  if (!joined.empty()) {
    descLines = text.wrap(joined, descStyle, kMaxWidth - kPadX * 2);
  }
  float w = text.measure(name, nameStyle);
  for (const std::string& line : descLines) w = std::max(w, text.measure(line, descStyle));
  w = std::min(w, kMaxWidth - kPadX * 2);

  float h = nameMetrics.lineHeight;
  if (!descLines.empty()) {
    h += 3.0f;  // margin-top: 3px
    h += descMetrics.lineHeight * static_cast<float>(descLines.size());
  }

  Rect box {mouseX + 14, mouseY + 16, w + kPadX * 2, h + kPadY * 2};
  // Keep it on screen: the browser let it overflow, but a clipped tooltip in a
  // fullscreen window is just a bug.
  if (box.right() > ui.width() - 4) box.x = std::max(4.0f, mouseX - box.w - 6);
  if (box.bottom() > ui.height() - 4) box.y = std::max(4.0f, ui.height() - 4 - box.h);

  ui.fillRect(box, color::inputBg, 6);
  ui.strokeRect(box, color::accentDark, 1, 6);

  float y = box.y + kPadY;
  text.drawInBox(ui, {box.x + kPadX, y, w, nameMetrics.lineHeight}, name, nameStyle);
  y += nameMetrics.lineHeight + 3.0f;
  for (const std::string& line : descLines) {
    text.drawInBox(ui, {box.x + kPadX, y, w, descMetrics.lineHeight}, line, descStyle);
    y += descMetrics.lineHeight;
  }
}

void drawBar(Ui2D& ui, const Rect& r, float fraction, Rgba fill, Rgba track, float radius) {
  ui.fillRect(r, track, radius);
  const float f = std::min(std::max(fraction, 0.0f), 1.0f);
  if (f <= 0.0f) return;
  ui.fillRect({r.x, r.y, r.w * f, r.h}, fill, radius);
}

void drawStack(Ui2D& ui, Text& text, const Rect& slot, const StackVisual& v) {
  if (v.icon.texture != 0) {
    ui.setTexture(v.icon.texture);
    ui.texturedRect(slot, v.icon.u0, v.icon.v0, v.icon.u1, v.icon.v1, v.icon.tint);
    ui.setTexture(0);
  }
  if (v.count > 1) {
    // .slot-count { position: absolute; right: 3px; bottom: 1px }
    const TextStyle ts = slotCount();
    const std::string label = std::to_string(v.count);
    const TextMetrics m = text.metrics(ts);
    const float w = text.measure(label, ts);
    text.draw(ui, slot.right() - 3 - w, slot.bottom() - 1 - m.descent, label, ts);
  }
  if (v.duraFraction >= 0.0f) {
    // .slot-dura { left: 4px; right: 4px; bottom: 3px; height: 3px }
    const Rect bar {slot.x + 4, slot.bottom() - 3 - 3, slot.w - 8, 3};
    drawBar(ui, bar, v.duraFraction, color::accent, color::black, 2);
  }
}

}  // namespace widget

// ---------------------------------------------------------------------------
// TextField
// ---------------------------------------------------------------------------

void TextField::setText(std::string s) {
  value_ = std::move(s);
  caret_ = anchor_ = value_.size();
}

void TextField::clear() {
  value_.clear();
  caret_ = anchor_ = 0;
}

void TextField::setFocused(bool on, Input* input) {
  if (focused_ == on) return;
  focused_ = on;
  if (input) {
    if (on) input->beginTextCapture();
    else input->endTextCapture();
  }
  if (on) {
    // Focusing selects nothing and puts the caret at the end, matching a click into an
    // <input> whose text is shorter than the click position.
    caret_ = anchor_ = value_.size();
  }
}

int TextField::codePointCount() const {
  int n = 0;
  std::size_t i = 0;
  while (i < value_.size()) {
    decodeUtf8(value_, i);
    ++n;
  }
  return n;
}

void TextField::deleteSelection() {
  const std::size_t a = selectionStart();
  const std::size_t b = selectionEnd();
  if (a == b) return;
  value_.erase(a, b - a);
  caret_ = anchor_ = a;
}

bool TextField::handle(const UiEvent& event, bool& submitted) {
  submitted = false;
  if (!focused_ || !event.input) return false;
  const Input& in = *event.input;
  bool changed = false;

  const auto moveCaret = [&](int delta, bool select) {
    caret_ = stepUtf8(value_, caret_, delta);
    if (!select) anchor_ = caret_;
  };

  if (in.pressed(Key::Left)) moveCaret(-1, event.shift);
  if (in.pressed(Key::Right)) moveCaret(1, event.shift);
  if (in.pressed(Key::Home)) {
    caret_ = 0;
    if (!event.shift) anchor_ = 0;
  }
  if (in.pressed(Key::End)) {
    caret_ = value_.size();
    if (!event.shift) anchor_ = caret_;
  }
  if (in.pressed(Key::Backspace)) {
    if (selectionStart() != selectionEnd()) {
      deleteSelection();
    } else if (caret_ > 0) {
      const std::size_t prev = stepUtf8(value_, caret_, -1);
      value_.erase(prev, caret_ - prev);
      caret_ = anchor_ = prev;
    }
    changed = true;
  }
  if (in.pressed(Key::Delete)) {
    if (selectionStart() != selectionEnd()) {
      deleteSelection();
    } else if (caret_ < value_.size()) {
      const std::size_t next = stepUtf8(value_, caret_, 1);
      value_.erase(caret_, next - caret_);
    }
    changed = true;
  }
  if (event.ctrl && in.pressed(Key::A)) {
    anchor_ = 0;
    caret_ = value_.size();
  }
  if (in.pressed(Key::Enter) || in.pressed(Key::KeypadEnter)) submitted = true;

  // Ctrl+C/V/X arrive as key edges rather than characters, because the platform layer
  // suppresses the control character. The clipboard itself is the window's, so a paste
  // is handled by the owning screen calling insert() with what it read; here we only
  // handle the keyboard-only edits.
  for (std::uint32_t cp : in.typedText()) {
    if (event.ctrl) break;  // Ctrl+letter is a shortcut, not text
    if (maxLength > 0 && codePointCount() >= maxLength && selectionStart() == selectionEnd()) {
      break;
    }
    deleteSelection();
    std::string encoded;
    appendUtf8(encoded, cp);
    value_.insert(caret_, encoded);
    caret_ += encoded.size();
    anchor_ = caret_;
    changed = true;
  }
  return changed;
}

void TextField::placeCaretAt(Text& text, const Rect& box, const TextStyle& style, float x) {
  // Walk code points until the advance passes the click, which is what a browser does
  // for a hit test inside a text run.
  std::size_t best = 0;
  float bestDistance = 1e9f;
  std::size_t i = 0;
  while (true) {
    const float w = text.measure(value_.substr(0, i), style);
    const float d = std::fabs(box.x + w - x);
    if (d < bestDistance) {
      bestDistance = d;
      best = i;
    }
    if (i >= value_.size()) break;
    i = stepUtf8(value_, i, 1);
  }
  caret_ = anchor_ = best;
}

void TextField::draw(Ui2D& ui, Text& text, const Rect& box, const TextStyle& style,
                     double time) {
  const TextMetrics m = text.metrics(style);
  const float baseline =
      box.y + (box.h - m.lineHeight) * 0.5f + (m.lineHeight - (m.ascent + m.descent)) * 0.5f +
      m.ascent;

  if (value_.empty() && !placeholder.empty()) {
    TextStyle ph = style;
    ph.color = color::muted;
    text.draw(ui, box.x, baseline, placeholder, ph);
  }

  if (focused_ && selectionStart() != selectionEnd()) {
    const float a = text.measure(value_.substr(0, selectionStart()), style);
    const float b = text.measure(value_.substr(0, selectionEnd()), style);
    ui.fillRect({box.x + a, box.y + (box.h - m.lineHeight) * 0.5f, b - a, m.lineHeight},
                fade(color::accentDark, 0.55));
  }

  if (!value_.empty()) text.draw(ui, box.x, baseline, value_, style);

  if (focused_) {
    // A 1s blink, on for the first half — the Windows caret cadence.
    const bool on = std::fmod(time, 1.0) < 0.5;
    if (on) {
      const float cx = box.x + text.measure(value_.substr(0, caret_), style);
      ui.fillRect({cx, box.y + (box.h - m.lineHeight) * 0.5f + 1.0f, 1.0f, m.lineHeight - 2.0f},
                  style.color);
    }
  }
}

}  // namespace hr::ui
