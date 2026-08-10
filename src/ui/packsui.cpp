#include "ui/packsui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "audio/sfx.h"
#include "platform/paths.h"

namespace hr::ui {
namespace {

enum : int {
  kTagPanel = 240,
  kTagToggle = 241,  // index = row
  kTagUp = 242,      // index = row
  kTagDown = 243,    // index = row
  kTagReload = 244,
  kTagExample = 245,
  kTagBack = 246,
};

}  // namespace

void PacksScreen::onShow() {
  hoveredTag_ = 0;
  notice_.clear();
  refresh();
}

void PacksScreen::refresh() {
  const std::vector<resource::PackInfo> installed = resource::scanPacks();
  const std::vector<std::string> enabled = resource::enabledPackIds();

  rows_.clear();
  // Enabled first, in their saved order, so what the list shows top to bottom is
  // exactly the order the packs are applied in.
  for (const std::string& id : enabled) {
    for (const resource::PackInfo& pack : installed) {
      if (pack.id != id) continue;
      rows_.push_back(Row{pack, true});
      break;
    }
  }
  for (const resource::PackInfo& pack : installed) {
    const bool alreadyListed = std::any_of(
        rows_.begin(), rows_.end(), [&](const Row& r) { return r.pack.id == pack.id; });
    if (!alreadyListed) rows_.push_back(Row{pack, false});
  }

  // An id in the saved list whose folder has gone takes this opportunity to
  // disappear from the saved list too, rather than being carried forever.
  commit();
}

void PacksScreen::commit() {
  std::vector<std::string> ids;
  for (const Row& row : rows_) {
    if (row.enabled) ids.push_back(row.pack.id);
  }
  resource::setEnabledPackIds(ids);
  if (onApply) onApply();
}

void PacksScreen::move(int index, int delta) {
  const int target = index + delta;
  if (index < 0 || index >= static_cast<int>(rows_.size())) return;
  if (target < 0 || target >= static_cast<int>(rows_.size())) return;
  // Only within the enabled block: an off pack has no position in the load order,
  // so swapping one up past the last enabled pack would be moving it to a place
  // that does not exist.
  if (!rows_[static_cast<std::size_t>(index)].enabled ||
      !rows_[static_cast<std::size_t>(target)].enabled) {
    return;
  }
  std::swap(rows_[static_cast<std::size_t>(index)], rows_[static_cast<std::size_t>(target)]);
  commit();
}

void PacksScreen::build(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens) {
  doc_.reset(&text);
  doc_.begin(widget::screen());

  Style card = widget::menuCard(true);
  card.maxWidth = ui.width() * 0.92f;
  doc_.begin(card);

  Style titleRow = Doc::row(0, Justify::Center, Align::Center);
  titleRow.margin = Edges(0, 0, 6, 0);
  doc_.begin(titleRow);
  doc_.label("Resource Packs", widget::h2());
  doc_.end();

  // The folder path, spelled out. There is no file picker in this build, so
  // "where do I put it" has to be answered on the screen itself.
  Style note;
  note.margin = Edges(0, 0, 4, 0);
  doc_.label(paths::resourcePacksDir(), widget::muted(11.5f), note);
  doc_.label("One folder per pack. The top of the list wins.", widget::muted(11.5f), note);
  if (!summary_.empty()) doc_.label(summary_, widget::muted(12.0f), note);
  if (!notice_.empty()) {
    TextStyle ts = widget::muted(12.0f);
    ts.color = color::accent;
    doc_.label(notice_, ts, note);
  }

  Style panel = Doc::column(0, Align::Stretch);
  panel.scrollY = true;
  panel.maxHeight = ui.height() * 0.52f;
  panel.padding = Edges(10, 6, 0, 0);
  doc_.begin(panel, kTagPanel);

  if (rows_.empty()) {
    Style empty;
    empty.margin = Edges(16, 0);
    doc_.label("No packs installed yet \xE2\x80\x94 use Create Example Pack below.",
               widget::emptyNote(), empty);
  }

  for (std::size_t i = 0; i < rows_.size(); ++i) {
    const Row& row = rows_[i];
    const int index = static_cast<int>(i);

    Style rowStyle = Doc::row(0, Justify::SpaceBetween, Align::Center);
    rowStyle.bg = row.enabled ? color::panel2 : color::panel;
    rowStyle.border = row.enabled ? color::accentDark : color::edge;
    rowStyle.borderWidth = 2;
    rowStyle.radius = 9;
    rowStyle.padding = Edges(10, 14);
    rowStyle.margin = Edges(5, 0);
    doc_.begin(rowStyle, 0, index);

    doc_.begin(Doc::column(0, Align::Start));
    doc_.begin(Doc::row(6, Justify::Start, Align::Center));
    doc_.label(row.pack.name.empty() ? row.pack.id : row.pack.name, widget::worldName());
    if (row.enabled) {
      // The load position, on the row, because the whole point of the ordering
      // controls is knowing where a pack currently sits.
      Style pill = Doc::row(0, Justify::Center, Align::Center);
      pill.bg = fade(color::accent, 0.18);
      pill.border = color::accentDark;
      pill.borderWidth = 1;
      pill.radius = 5;
      pill.padding = Edges(1, 6, 2, 6);
      doc_.begin(pill);
      char slot[24];
      std::snprintf(slot, sizeof(slot), "#%d", index + 1);
      doc_.label(slot, widget::worldBadge());
      doc_.end();
    }
    doc_.end();

    if (!row.pack.usable()) {
      TextStyle ts = widget::worldMeta();
      ts.color = color::danger;
      doc_.label(row.pack.problem, ts);
    } else {
      char meta[192];
      // Textures are counted and shown even though nothing applies them yet. A
      // pack that supplies them is not broken and should not be described as if
      // it were; saying what was found and what is used is the honest version.
      std::snprintf(meta, sizeof(meta), "%d sound%s%s%s \xC2\xB7 %s", row.pack.soundFiles,
                    row.pack.soundFiles == 1 ? "" : "s",
                    row.pack.hasSoundsJson ? " \xC2\xB7 sounds.json" : "",
                    row.pack.textureFiles > 0 ? " \xC2\xB7 textures (not applied yet)" : "",
                    row.pack.id.c_str());
      doc_.label(meta, widget::worldMeta());
    }
    doc_.end();

    doc_.begin(Doc::row(8, Justify::End, Align::Center));
    if (row.enabled) {
      // Greyed rather than absent at the ends of the block: a control that jumps
      // out of existence when you reach the top is harder to aim at than one that
      // stays put and stops working.
      const bool canUp = index > 0 && rows_[static_cast<std::size_t>(index - 1)].enabled;
      const bool canDown = index + 1 < static_cast<int>(rows_.size()) &&
                           rows_[static_cast<std::size_t>(index + 1)].enabled;
      struct Arrow {
        int tag;
        const char* glyph;
        bool live;
      };
      for (const Arrow& a : {Arrow{kTagUp, "\xE2\x96\xB2", canUp},
                             Arrow{kTagDown, "\xE2\x96\xBC", canDown}}) {
        const bool hovered = a.live && hoveredTag_ == a.tag && hoveredIndex_ == index;
        Style s = widget::btnSmall(hovered, false, widget::ButtonKind::Normal);
        s.width = kAuto;
        s.margin = Edges(0);
        if (!a.live) s.opacity = 0.35f;
        doc_.begin(s, a.live ? a.tag : 0, index);
        doc_.label(a.glyph, widget::btnSmallText(hovered, widget::ButtonKind::Normal));
        doc_.end();
      }
    }

    const bool broken = !row.pack.usable();
    const bool hovered = !broken && hoveredTag_ == kTagToggle && hoveredIndex_ == index;
    const widget::ButtonKind kind =
        row.enabled ? widget::ButtonKind::Primary : widget::ButtonKind::Normal;
    Style s = widget::btnSmall(hovered, false, kind);
    s.width = kAuto;
    s.margin = Edges(0);
    if (broken) s.opacity = 0.35f;
    doc_.begin(s, broken ? 0 : kTagToggle, index);
    doc_.label(row.enabled ? "On" : "Off", widget::btnSmallText(hovered, kind));
    doc_.end();
    doc_.end();

    doc_.end();
  }
  doc_.end();

  Style buttons = Doc::row(10, Justify::End, Align::Center);
  buttons.margin = Edges(14, 0, 0, 0);
  doc_.begin(buttons);
  struct Action {
    int tag;
    const char* label;
    widget::ButtonKind kind;
  };
  for (const Action& a : {Action{kTagExample, "Create Example Pack", widget::ButtonKind::Normal},
                          Action{kTagReload, "Reload", widget::ButtonKind::Normal},
                          Action{kTagBack, "Back", widget::ButtonKind::Normal}}) {
    const bool hovered = hoveredTag_ == a.tag;
    Style s = widget::btn(hovered, false, a.kind);
    s.width = kAuto;
    doc_.begin(s, a.tag);
    doc_.label(a.label, widget::btnText(hovered, a.kind));
    doc_.end();
  }
  doc_.end();

  doc_.end();
  doc_.end();
  doc_.layout({0, 0, ui.width(), ui.height()});
  (void)event;
  (void)tweens;
}

void PacksScreen::handle(const UiEvent& event) {
  const int hit = doc_.hitTest(event.mouseX, event.mouseY);
  if (event.leftClick && doc_.clickedButton(hit)) audio::sfx::uiClick();
  hoveredTag_ = hit >= 0 ? doc_.node(hit).tag : 0;
  hoveredIndex_ = hit >= 0 ? doc_.node(hit).index : 0;

  if (event.wheel != 0.0f) {
    const int panel = doc_.findTag(kTagPanel);
    if (panel >= 0 && doc_.node(panel).rect.contains(event.mouseX, event.mouseY)) {
      doc_.scroll(kTagPanel).offset += event.wheel * 48.0f;
    }
  }

  if (!event.leftClick) return;

  switch (hoveredTag_) {
    case kTagToggle: {
      if (hoveredIndex_ < 0 || hoveredIndex_ >= static_cast<int>(rows_.size())) return;
      Row& row = rows_[static_cast<std::size_t>(hoveredIndex_)];
      if (!row.pack.usable()) return;
      row.enabled = !row.enabled;
      // Either way the row moves to the boundary between the two blocks, which
      // is the same index for both cases: switched on it becomes the LAST of the
      // enabled packs, switched off it becomes the first of the rest. Turning one
      // on must not let it arrive already outranking everything, and turning one
      // off must not leave a hole in the middle of the order.
      const Row moved = row;
      rows_.erase(rows_.begin() + hoveredIndex_);
      std::size_t boundary = 0;
      while (boundary < rows_.size() && rows_[boundary].enabled) ++boundary;
      rows_.insert(rows_.begin() + static_cast<std::ptrdiff_t>(boundary), moved);
      notice_.clear();
      commit();
      return;
    }
    case kTagUp: move(hoveredIndex_, -1); return;
    case kTagDown: move(hoveredIndex_, 1); return;
    case kTagReload:
      notice_.clear();
      refresh();
      return;
    case kTagExample: {
      std::string message;
      if (onCreateExample && onCreateExample(message)) {
        refresh();
      }
      notice_ = message;
      return;
    }
    case kTagBack:
      if (onBack) onBack();
      return;
    default: return;
  }
}

void PacksScreen::update(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens) {
  build(ui, text, event, tweens);
  handle(event);
  build(ui, text, event, tweens);
}

void PacksScreen::draw(Ui2D& ui, Text& text) {
  if (hasBackdrop_) {
    ui.fillRect({0, 0, ui.width(), ui.height()}, rgba(8, 11, 15, 0.62));
  } else {
    const float cx = ui.width() * 0.5f;
    const float cy = ui.height() * 0.30f;
    const float far = std::sqrt(std::max(cx, ui.width() - cx) * std::max(cx, ui.width() - cx) +
                                std::max(cy, ui.height() - cy) * std::max(cy, ui.height() - cy));
    ui.radialGradient({0, 0, ui.width(), ui.height()}, cx, cy, far, far, 0.0f, 0.75f,
                      rgb(0x1d2733), rgb(0x0e1218));
  }
  doc_.paint(ui);
  (void)text;
}

}  // namespace hr::ui
