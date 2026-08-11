#include "ui/settingsui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "audio/sfx.h"

namespace hr::ui {
namespace {

enum : int {
  kTagTab = 200,      // index = tab
  kTagControl = 201,  // index = schema row
  kTagTrack = 202,    // index = schema row
  kTagBack = 203,
  kTagPanel = 204,
};

// .setting .val { min-width: 48px; text-align: right }
constexpr float kValueWidth = 48.0f;
// The slider thumb, which <input type=range> draws and this has to.
constexpr float kThumbRadius = 7.0f;

std::string formatNumber(double v) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%g", v);
  return buffer;
}

// The tabs to show right now. A world's rules need a world to belong to, so
// Difficulty and Cheats simply are not there on the main menu -- there is no world
// whose rules could be displayed, and showing the last one's would be a lie a
// player would reasonably act on.
std::vector<std::string> visibleCategories() {
  std::vector<std::string> out;
  for (const std::string& c : settingsCategories()) {
    if (categoryScope(c) == SettingScope::World && !settings().inWorld()) continue;
    // A tab whose every row is gated off is not an empty tab, it is no tab. The
    // Debug tab does not exist until somebody switches the tools on.
    bool any = false;
    for (const SettingDef& def : settingsSchema()) {
      if (def.hidden || def.category != c) continue;
      if (settings().available(def)) {
        any = true;
        break;
      }
    }
    if (!any) continue;
    out.push_back(c);
  }
  return out;
}

}  // namespace

void SettingsScreen::onShow() {
  const std::vector<std::string> cats = visibleCategories();
  if (cats.empty()) return;
  if (std::find(cats.begin(), cats.end(), activeTab_) == cats.end()) activeTab_ = cats.front();
  hoveredTag_ = 0;
  draggingSlider_ = -1;
}

void SettingsScreen::build(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens) {
  doc_.reset(&text);
  doc_.begin(widget::screen());

  Style card = widget::menuCard(true);
  card.maxWidth = ui.width() * 0.92f;
  doc_.begin(card);

  Style titleRow = Doc::row(0, Justify::Center, Align::Center);
  titleRow.margin = Edges(0, 0, 18, 0);
  doc_.begin(titleRow);
  doc_.label("Settings", widget::h2());
  doc_.end();

  // Categories down the left, rows on the right.
  //
  // They used to be a wrapping row of tabs across the top. Two things were wrong
  // with that and both get worse rather than better over time: the tabs consumed
  // full height while using almost none of the width, on a screen whose rows are
  // a short label and a small control and therefore have width to spare; and a
  // horizontal strip has nowhere to grow. Five categories fit. Adding a sixth and
  // a seventh — which the gate mechanism exists to make easy — wrapped them onto a
  // second line and moved every tab under the cursor.
  //
  // A vertical rail has room for as many as the schema ever grows, puts them in a
  // list you read rather than a strip you scan, and costs width the rows were not
  // using.
  const std::vector<std::string> cats = visibleCategories();

  Style body = Doc::row(px(Scalar::Pad), Justify::Start, Align::Start);
  body.margin = Edges(0, 0, px(Scalar::Pad), 0);
  doc_.begin(body);

  Style rail = Doc::column(px(Scalar::GapTight), Align::Stretch);
  rail.width = 164;
  doc_.begin(rail);
  for (std::size_t i = 0; i < cats.size(); ++i) {
    const bool active = cats[i] == activeTab_;
    const bool hovered = hoveredTag_ == kTagTab && hoveredIndex_ == static_cast<int>(i);
    Style s = widget::settingsTab(active, hovered);
    // Left-aligned in the rail. Centred labels in a vertical list give every row a
    // different left edge, and the eye tracks a list by its left edge.
    s.justify = Justify::Start;
    s.padding = Edges(px(Scalar::PadTight), px(Scalar::GapWide) * 0.75f);
    doc_.begin(s, kTagTab, static_cast<int>(i));
    TextStyle ts;
    ts.font = FontId::SansSemibold;
    ts.size = 14;
    ts.letterSpacing = 0.5f;
    ts.color = active ? col(Role::TabActiveInk) : (hovered ? col(Role::Text) : col(Role::Muted));
    doc_.label(cats[i], ts);
    doc_.end();
  }
  doc_.end();

  // The rows. Taller than the old panel because the tab strip is no longer above
  // it, and it grows into whatever width the rail leaves.
  Style panel = Doc::column(0, Align::Stretch);
  panel.scrollY = true;
  panel.maxHeight = ui.height() * 0.62f;
  panel.minHeight = 220;
  panel.grow = 1;
  panel.padding = Edges(0, px(Scalar::GapTight), 0, 0);
  doc_.begin(panel, kTagPanel);

  const std::vector<SettingDef>& schema = settingsSchema();
  for (std::size_t i = 0; i < schema.size(); ++i) {
    const SettingDef& def = schema[i];
    if (def.hidden || def.category != activeTab_) continue;
    // Gated off means absent, not greyed. A row explaining that you cannot use it
    // is a row about the interface rather than about the game.
    if (!settings().available(def)) continue;

    // .setting { display: flex; justify-content: space-between; align-items: center;
    //            padding: 8px 4px; border-bottom: 1px solid #141a22 }
    Style rowStyle = Doc::row(0, Justify::SpaceBetween, Align::Center);
    rowStyle.padding = Edges(8, 4);
    doc_.begin(rowStyle, 0, static_cast<int>(i));
    doc_.label(def.label, widget::settingLabel());

    switch (def.type) {
      case SettingType::Slider: {
        // .row { display: flex; gap: 10px; align-items: center }
        doc_.begin(Doc::row(10, Justify::End, Align::Center));
        doc_.custom(widget::sliderTrack(), kTagTrack, static_cast<int>(i));
        Style valueBox;
        valueBox.width = kValueWidth;
        doc_.label(formatNumber(settings().number(def.key)), widget::settingValue(), valueBox);
        doc_.node(doc_.count() - 1).textAlign = TextAlign::Right;
        doc_.end();
        break;
      }
      case SettingType::Toggle: {
        const bool hovered = hoveredTag_ == kTagControl && hoveredIndex_ == static_cast<int>(i);
        Style s = widget::btnSmall(hovered, false, widget::ButtonKind::Normal);
        s.width = kAuto;
        s.margin = Edges(0);
        doc_.begin(s, kTagControl, static_cast<int>(i));
        doc_.label(settings().flag(def.key) ? "On" : "Off",
                   widget::btnSmallText(hovered, widget::ButtonKind::Normal));
        doc_.end();
        break;
      }
      case SettingType::Select: {
        const bool hovered = hoveredTag_ == kTagControl && hoveredIndex_ == static_cast<int>(i);
        Style s = widget::selectBox(hovered);
        doc_.begin(s, kTagControl, static_cast<int>(i));
        TextStyle ts;
        ts.size = 14;
        doc_.label(settings().text(def.key), ts);
        doc_.end();
        break;
      }
      case SettingType::Action: {
        // A button with no state behind it. Primary, because unlike every other row
        // here pressing it makes something happen rather than setting how things are.
        const bool hovered = hoveredTag_ == kTagControl && hoveredIndex_ == static_cast<int>(i);
        Style s = widget::btnSmall(hovered, false, widget::ButtonKind::Primary);
        s.width = kAuto;
        s.margin = Edges(0);
        doc_.begin(s, kTagControl, static_cast<int>(i));
        doc_.label("Go", widget::btnSmallText(hovered, widget::ButtonKind::Primary));
        doc_.end();
        break;
      }
      case SettingType::Text: break;  // set elsewhere; see SettingDef::hidden
    }
    doc_.end();

    // The 1px rule under each row.
    Style rule;
    rule.height = 1;
    rule.bg = col(Role::PanelRule);
    doc_.box(rule);
  }
  doc_.end();  // the rows
  doc_.end();  // the rail-and-rows body row

  // Back, under the whole body rather than under the rows, so it stays put when a
  // category with fewer rows is chosen.
  Style backRow = Doc::row(0, Justify::End, Align::Center);
  backRow.margin = Edges(px(Scalar::GapTight), 0, 0, 0);
  doc_.begin(backRow);
  const bool hovered = hoveredTag_ == kTagBack;
  Style s = widget::btn(hovered, false, widget::ButtonKind::Normal);
  s.width = kAuto;
  doc_.begin(s, kTagBack);
  doc_.label("Back", widget::btnText(hovered, widget::ButtonKind::Normal));
  doc_.end();
  doc_.end();

  doc_.end();
  doc_.end();
  doc_.layout({0, 0, ui.width(), ui.height()});
  (void)event;
  (void)tweens;
}

void SettingsScreen::setValueFromDrag(const SettingDef& def, const Rect& track, float mouseX) {
  const float t = track.w > 0 ? std::min(1.0f, std::max(0.0f, (mouseX - track.x) / track.w))
                              : 0.0f;
  double v = def.min + static_cast<double>(t) * (def.max - def.min);
  if (def.step > 0) v = def.min + std::round((v - def.min) / def.step) * def.step;
  v = std::min(def.max, std::max(def.min, v));
  if (v == settings().number(def.key)) return;
  settings().setNumber(def.key, v);
  if (onChange) onChange(def.key);
}

void SettingsScreen::handle(const UiEvent& event) {
  const int hit = doc_.hitTest(event.mouseX, event.mouseY);
  // main.js:120 hung one listener on the document and ticked whenever the click
  // landed inside a <button>; this is that listener.
  if (event.leftClick && doc_.clickedButton(hit)) audio::sfx::uiClick();
  hoveredTag_ = hit >= 0 ? doc_.node(hit).tag : 0;
  hoveredIndex_ = hit >= 0 ? doc_.node(hit).index : 0;

  if (event.wheel != 0.0f) {
    const int panel = doc_.findTag(kTagPanel);
    if (panel >= 0 && doc_.node(panel).rect.contains(event.mouseX, event.mouseY)) {
      doc_.scroll(kTagPanel).offset += event.wheel * 48.0f;
    }
  }

  const std::vector<SettingDef>& schema = settingsSchema();

  // A slider keeps the drag once it has started, even if the pointer leaves the track —
  // which is what a native range input does.
  if (draggingSlider_ >= 0) {
    if (!event.leftDown) {
      draggingSlider_ = -1;
    } else {
      const int node = doc_.findTag(kTagTrack, draggingSlider_);
      if (node >= 0) {
        setValueFromDrag(schema[static_cast<std::size_t>(draggingSlider_)],
                         doc_.node(node).rect, event.mouseX);
      }
    }
  }

  if (!event.leftClick) return;

  if (hoveredTag_ == kTagTab) {
    const std::vector<std::string> cats = visibleCategories();
    if (hoveredIndex_ < static_cast<int>(cats.size())) {
      activeTab_ = cats[static_cast<std::size_t>(hoveredIndex_)];
      doc_.scroll(kTagPanel).offset = 0;
    }
    return;
  }
  if (hoveredTag_ == kTagBack) {
    if (onBack) onBack();
    return;
  }
  if (hoveredTag_ == kTagTrack) {
    if (hoveredIndex_ < static_cast<int>(schema.size()) &&
        !settings().editable(schema[static_cast<std::size_t>(hoveredIndex_)].key)) {
      return;
    }
    draggingSlider_ = hoveredIndex_;
    const int node = doc_.findTag(kTagTrack, hoveredIndex_);
    if (node >= 0) {
      setValueFromDrag(schema[static_cast<std::size_t>(hoveredIndex_)], doc_.node(node).rect,
                       event.mouseX);
    }
    return;
  }
  if (hoveredTag_ == kTagControl && hoveredIndex_ < static_cast<int>(schema.size())) {
    const SettingDef& def = schema[static_cast<std::size_t>(hoveredIndex_)];
    // A guest is in somebody else's world and does not set its rules. Refused here
    // rather than hidden, so they can still SEE what they are playing under.
    if (!settings().editable(def.key)) return;
    if (def.type == SettingType::Toggle) {
      settings().setFlag(def.key, !settings().flag(def.key));
      if (onChange) onChange(def.key);
    } else if (def.type == SettingType::Select && !def.options.empty()) {
      // No native dropdown: clicking cycles, which the CSS already styled as a button.
      const int next = (settings().selectedIndex(def.key) + 1) %
                       static_cast<int>(def.options.size());
      settings().setText(def.key, def.options[static_cast<std::size_t>(next)]);
      if (onChange) onChange(def.key);
    } else if (def.type == SettingType::Action) {
      // Nothing stored, nothing toggled. The callback is the entire row.
      if (onChange) onChange(def.key);
    }
  }
}

void SettingsScreen::update(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens) {
  build(ui, text, event, tweens);
  handle(event);
  build(ui, text, event, tweens);
}

void SettingsScreen::draw(Ui2D& ui, Text& text) {
  if (hasBackdrop_) {
    // Something is already drawn behind us — a chosen menu picture, or the world
    // when this is opened from the pause menu. Darken it enough to read against
    // instead of painting over it.
    ui.fillRect({0, 0, ui.width(), ui.height()}, col(Role::Scrim, 0.62f));
  } else {
    const float cx = ui.width() * 0.5f;
    const float cy = ui.height() * 0.30f;
    const float far = std::sqrt(std::max(cx, ui.width() - cx) * std::max(cx, ui.width() - cx) +
                               std::max(cy, ui.height() - cy) * std::max(cy, ui.height() - cy));
    ui.radialGradient({0, 0, ui.width(), ui.height()}, cx, cy, far, far, 0.0f, 0.75f,
                      rgb(0x1d2733), rgb(0x0e1218));
  }
  doc_.paint(ui);

  // The tab strip's bottom rule and every slider's fill and thumb: both are CSS
  // decorations with no node of their own.
  const int panel = doc_.findTag(kTagPanel);
  if (panel >= 0) {
    const Rect r = doc_.node(panel).rect;
    ui.fillRect({r.x, r.y - 14 - 2, r.w, 2}, col(Role::Edge));
  }

  const std::vector<SettingDef>& schema = settingsSchema();
  ui.pushClip(panel >= 0 ? doc_.node(panel).rect : Rect {0, 0, ui.width(), ui.height()});
  for (int i = 0; i < doc_.count(); ++i) {
    const Node& n = doc_.node(i);
    if (n.tag != kTagTrack) continue;
    const SettingDef& def = schema[static_cast<std::size_t>(n.index)];
    const double span = def.max - def.min;
    const float t = span > 0 ? static_cast<float>((settings().number(def.key) - def.min) / span)
                             : 0.0f;
    // accent-color: var(--accent) — the filled portion left of the thumb.
    ui.fillRect(n.rect, col(Role::InputBg), 2);
    ui.fillRect({n.rect.x, n.rect.y, n.rect.w * t, n.rect.h}, col(Role::Accent), 2);
    const float thumbX = n.rect.x + n.rect.w * t;
    ui.fillRect({thumbX - kThumbRadius, n.rect.centerY() - kThumbRadius, kThumbRadius * 2,
                 kThumbRadius * 2},
                col(Role::Accent), kThumbRadius);
    ui.strokeRect({thumbX - kThumbRadius, n.rect.centerY() - kThumbRadius, kThumbRadius * 2,
                   kThumbRadius * 2},
                  col(Role::Edge), 1, kThumbRadius);
  }
  ui.popClip();
  (void)text;
}

}  // namespace hr::ui
