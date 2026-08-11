#include "ui/gallery.h"

#include <algorithm>
#include <cstdio>

#include "audio/sfx.h"
#include "core/log.h"
#include "resource/image.h"
#include "save/gallery.h"
#include "ui/settings.h"

namespace hr::ui {
namespace {

enum : int {
  kTagCard = 500,   // index = item
  kTagView = 501,
  kTagSave = 502,
  kTagDelete = 503,
  kTagBack = 504,
  kTagScroll = 505,
  kTagViewer = 506,
  kTagSetBg = 507,
  kTagPick = 508,
};

// .gal-grid { repeat(auto-fill, minmax(210px, 1fr)); gap: 12px }
constexpr float kMinCard = 210.0f;
constexpr float kCardGap = 12.0f;
// .gal-thumb { aspect-ratio: 16 / 10 }
constexpr float kThumbAspect = 16.0f / 10.0f;
// Thumbnails are decoded at roughly card width, so a 1920x1080 capture does not become a
// two-megabyte texture per card.
constexpr int kThumbWidth = 256;
constexpr int kDecodeBudgetPerFrame = 2;

std::string timeAgo(double seconds) {
  if (seconds <= 0) return "\xE2\x80\x94";
  if (seconds < 60) return "just now";
  char buffer[32];
  if (seconds < 3600) std::snprintf(buffer, sizeof(buffer), "%dm ago", static_cast<int>(seconds / 60));
  else if (seconds < 86400) std::snprintf(buffer, sizeof(buffer), "%dh ago", static_cast<int>(seconds / 3600));
  else std::snprintf(buffer, sizeof(buffer), "%dd ago", static_cast<int>(seconds / 86400));
  return buffer;
}

GLuint uploadRgba(const Image& image) {
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width(), image.height(), 0, GL_RGBA,
               GL_UNSIGNED_BYTE, image.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  return texture;
}

}  // namespace

Gallery::~Gallery() { destroy(); }

void Gallery::destroy() {
  for (Item& item : items_) {
    if (item.thumbnail) glDeleteTextures(1, &item.thumbnail);
    item.thumbnail = 0;
  }
  items_.clear();
  if (viewTexture_) glDeleteTextures(1, &viewTexture_);
  viewTexture_ = 0;
  viewing_ = -1;
}

void Gallery::onShowPicker() {
  onShow();
  picking_ = true;
}

void Gallery::onShow() {
  destroy();
  picking_ = false;
  // The directory walk lives in the save layer; this screen only decodes and draws.
  for (save::gallery::Shot& shot : save::gallery::list()) {
    Item item;
    item.path = std::move(shot.path);
    item.name = std::move(shot.name);
    item.ageSeconds = shot.ageSeconds;
    items_.push_back(std::move(item));
  }
  doc_.scroll(kTagScroll).offset = 0;
}

void Gallery::decodeSome(int budget) {
  for (Item& item : items_) {
    if (budget <= 0) return;
    if (item.decoded || item.failed) continue;
    Image full;
    std::string error;
    if (!Image::loadPng(item.path, full, &error)) {
      item.failed = true;
      log::warn("gallery: %s", error.c_str());
      continue;
    }
    // Nearest downsample to card width. A box filter would be prettier, but these are
    // thumbnails of a pixel-art game and nearest keeps the blocks legible.
    const int tw = std::min(kThumbWidth, full.width());
    const int th = full.width() > 0 ? std::max(1, full.height() * tw / full.width()) : 1;
    const Image thumb = full.scaledNearest(tw, th);
    item.thumbnail = uploadRgba(thumb);
    item.thumbWidth = tw;
    item.thumbHeight = th;
    item.decoded = true;
    --budget;
  }
}

void Gallery::build(Ui2D& ui, Text& text, const UiEvent& event) {
  doc_.reset(&text);
  doc_.begin(widget::screen());

  // .menu-card.wide.gal-window { width: 820px; max-width: 94vw }
  Style card = widget::menuCard(true);
  card.width = std::min(820.0f, ui.width() * 0.94f);
  doc_.begin(card);

  Style titleRow = Doc::row(0, Justify::Center, Align::Center);
  titleRow.margin = Edges(0, 0, 18, 0);
  doc_.begin(titleRow);
  doc_.label(picking_ ? "Choose a Picture" : "Gallery", widget::h2());
  doc_.end();

  // #gallery-root { max-height: 66vh; overflow-y: auto }
  Style root = Doc::column(0, Align::Stretch);
  root.scrollY = true;
  root.maxHeight = ui.height() * 0.66f;
  root.padding = Edges(0, 6, 0, 0);
  doc_.begin(root, kTagScroll);

  if (items_.empty()) {
    Style note;
    note.margin = Edges(16, 0);
    note.maxWidth = 700;
    doc_.label(picking_ ? "Nothing to hang yet. Press F2 for a screenshot, then come back."
                        : "No captures yet. Press F2 in-game for a screenshot.",
               widget::emptyNote(), note);
  } else {
    Style grid;
    grid.display = Display::Grid;
    grid.gridCols = 0;
    grid.gridMinCol = kMinCard;
    grid.gap = kCardGap;
    doc_.begin(grid);
    for (std::size_t i = 0; i < items_.size(); ++i) {
      const Item& item = items_[i];
      // .gal-card { flex-direction: column; border-radius: 10px; overflow: hidden }
      Style cardStyle = Doc::column(0, Align::Stretch);
      cardStyle.bg = col(Role::PanelRaised);
      cardStyle.border = col(Role::Edge);
      cardStyle.borderWidth = 1;
      cardStyle.radius = 10;
      doc_.begin(cardStyle, kTagCard, static_cast<int>(i));

      // .gal-thumb — a 16:10 box; the image is object-fit: cover.
      Style thumb;
      thumb.display = Display::Block;
      thumb.bg = col(Role::InputBg);
      thumb.height = kMinCard / kThumbAspect;
      doc_.custom(thumb, kTagCard, static_cast<int>(i));

      // .gal-info { justify-content: space-between; align-items: baseline; padding: 7px 9px 4px }
      Style info = Doc::row(6, Justify::SpaceBetween, Align::Baseline);
      info.padding = Edges(7, 9, 4, 9);
      doc_.begin(info);
      // .gal-world { white-space: nowrap; overflow: hidden; text-overflow: ellipsis }
      Style nameBox;
      nameBox.maxWidth = 140;
      nameBox.ellipsis = true;
      nameBox.shrink = 1;
      doc_.label(item.name, widget::galWorld(), nameBox);
      // .gal-date { flex: none } — the date keeps its width and the name gives way.
      Style dateBox;
      dateBox.shrink = 0;
      doc_.label(timeAgo(item.ageSeconds), widget::galDate(), dateBox);
      doc_.end();

      // .gal-btns { display: flex; gap: 4px; padding: 4px 8px 8px }
      Style buttons = Doc::row(4, Justify::Start, Align::Center);
      buttons.padding = Edges(4, 8, 8, 8);
      doc_.begin(buttons);
      struct ButtonDef {
        int tag;
        const char* label;
        bool danger;
      };
      const std::vector<ButtonDef> actions =
          picking_ ? std::vector<ButtonDef> {{kTagPick, "Hang this", false}}
                   : std::vector<ButtonDef> {{kTagView, "View", false},
                                             {kTagSetBg, "Set BG", false},
                                             {kTagSave, "Reveal", false},
                                             {kTagDelete, "\xE2\x9C\x95", true}};
      for (const ButtonDef& b : actions) {
        const bool hovered = hoveredTag_ == b.tag && hoveredIndex_ == static_cast<int>(i);
        doc_.begin(widget::galleryButton(hovered, b.danger), b.tag, static_cast<int>(i));
        TextStyle ts;
        ts.font = FontId::SansSemibold;
        ts.size = 11.5f;
        ts.color = b.danger ? (hovered ? kWhite : col(Role::Danger)) : col(Role::Text);
        doc_.label(b.label, ts);
        doc_.end();
      }
      doc_.end();
      doc_.end();
    }
    doc_.end();
  }
  doc_.end();

  Style backRow = Doc::row(0, Justify::End, Align::Center);
  backRow.margin = Edges(14, 0, 0, 0);
  doc_.begin(backRow);
  const bool hovered = hoveredTag_ == kTagBack;
  Style back = widget::btnSmall(hovered, false, widget::ButtonKind::Normal);
  back.width = kAuto;
  doc_.begin(back, kTagBack);
  doc_.label("Close", widget::btnSmallText(hovered, widget::ButtonKind::Normal));
  doc_.end();
  doc_.end();

  doc_.end();
  doc_.end();
  doc_.layout({0, 0, ui.width(), ui.height()});
  (void)event;
}

void Gallery::handle(const UiEvent& event) {
  // The viewer swallows everything: `.gal-viewer { cursor: zoom-out }` closes on any click.
  if (viewing_ >= 0) {
    if (event.leftClick || event.rightClick) {
      if (viewTexture_) glDeleteTextures(1, &viewTexture_);
      viewTexture_ = 0;
      viewing_ = -1;
    }
    return;
  }

  const int hit = doc_.hitTest(event.mouseX, event.mouseY);
  // main.js:120 hung one listener on the document and ticked whenever the click
  // landed inside a <button>; this is that listener.
  if (event.leftClick && doc_.clickedButton(hit)) audio::sfx::uiClick();
  hoveredTag_ = hit >= 0 ? doc_.node(hit).tag : 0;
  hoveredIndex_ = hit >= 0 ? doc_.node(hit).index : 0;

  if (event.wheel != 0.0f) {
    const int node = doc_.findTag(kTagScroll);
    if (node >= 0 && doc_.node(node).rect.contains(event.mouseX, event.mouseY)) {
      doc_.scroll(kTagScroll).offset += event.wheel * 48.0f;
    }
  }

  if (!event.leftClick) return;
  if (hoveredTag_ == kTagBack) {
    if (onBack) onBack();
    return;
  }
  if (hoveredIndex_ >= static_cast<int>(items_.size())) return;
  Item& item = items_[static_cast<std::size_t>(hoveredIndex_)];
  switch (hoveredTag_) {
    case kTagPick:
      // The path goes out; whoever asked for a picture does the loading. This
      // screen has no idea a painting exists, which is what keeps it a gallery.
      if (onPick) onPick(item.path);
      break;
    case kTagView: {
      Image full;
      std::string error;
      if (!Image::loadPng(item.path, full, &error)) {
        log::warn("gallery: %s", error.c_str());
        break;
      }
      if (viewTexture_) glDeleteTextures(1, &viewTexture_);
      viewTexture_ = uploadRgba(full);
      viewWidth_ = full.width();
      viewHeight_ = full.height();
      viewing_ = hoveredIndex_;
      break;
    }
    case kTagSave:
      // The file is already on disk under the player's own data directory, so "Save"
      // becomes "Reveal": the browser had to offer a download because its captures lived
      // in IndexedDB.
      log::info("screenshot: %s", item.path.c_str());
      save::gallery::reveal(item.path);
      break;
    case kTagSetBg:
      // The menu's backdrop. Stored as a path, not a copy: the picture is already
      // the player's, sitting in their own screenshots folder, and a second copy
      // would go stale the moment they deleted the original.
      settings().setText("menuBackground", item.path);
      break;
    case kTagDelete: {
      const std::string path = item.path;
      const std::string name = item.name.empty() ? "this picture" : item.name;
      confirm_.open("Delete " + name + "?", "Delete", [this, path] {
        // If the wallpaper is the picture being deleted, stop pointing at it.
        if (settings().text("menuBackground") == path) settings().setText("menuBackground", "");
        save::gallery::erase(path);
        onShow();
      });
      break;
    }
    default: break;
  }
}

void Gallery::update(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens) {
  decodeSome(kDecodeBudgetPerFrame);
  build(ui, text, event);
  // Ahead of the screen's own handling, so the click that answers the question is
  // not also a click on the tile behind it.
  if (!confirm_.handle(ui.width(), ui.height(), event)) handle(event);
  build(ui, text, event);
  (void)tweens;
}

void Gallery::draw(Ui2D& ui, Text& text) {
  ui.fillRect({0, 0, ui.width(), ui.height()}, col(Role::WashScreen));
  doc_.paint(ui);

  // The thumbnails, drawn into the boxes the layout reserved. `object-fit: cover` means
  // the image fills the box and the overflow is clipped, so the UVs are inset rather than
  // the quad letterboxed.
  for (int i = 0; i < doc_.count(); ++i) {
    const Node& n = doc_.node(i);
    if (n.tag != kTagCard || n.content != Content::Custom) continue;
    const Item& item = items_[static_cast<std::size_t>(n.index)];
    if (!item.thumbnail) continue;
    const float boxAspect = n.rect.h > 0 ? n.rect.w / n.rect.h : 1.0f;
    const float imageAspect = item.thumbHeight > 0
                                  ? static_cast<float>(item.thumbWidth) / item.thumbHeight
                                  : 1.0f;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (imageAspect > boxAspect) {
      const float keep = boxAspect / imageAspect;
      u0 = (1.0f - keep) * 0.5f;
      u1 = 1.0f - u0;
    } else {
      const float keep = imageAspect / boxAspect;
      v0 = (1.0f - keep) * 0.5f;
      v1 = 1.0f - v0;
    }
    // .gal-card { border-radius: 10px; overflow: hidden } — the card is the mask, so the
    // thumbnail's top corners round and its bottom edge stays square.
    const int card = n.parent;
    const Rect mask = card >= 0 ? doc_.node(card).rect : n.rect;
    ui.setTexture(item.thumbnail);
    ui.texturedRectMasked(n.rect, u0, v0, u1, v1, mask, 10.0f);
    ui.setTexture(0);

    // .gal-badge — "SHOT" in the corner. Panoramas will add a second badge colour.
    const TextStyle badge = widget::galBadge();
    const std::string label = "SHOT";
    const float w = text.measure(label, badge);
    const TextMetrics m = text.metrics(badge);
    const Rect plate {n.rect.x + 6, n.rect.y + 6, w + 12, m.lineHeight + 4};
    ui.fillRect(plate, col(Role::Scrim, 0.72f), 5);
    text.drawInBox(ui, {plate.x + 6, plate.y + 2, w, m.lineHeight}, label, badge);
  }

  if (viewing_ >= 0 && viewTexture_) {
    // .gal-viewer — the still, at most 92vw x 84vh, over a near-opaque wash.
    ui.fillRect({0, 0, ui.width(), ui.height()}, col(Role::Scrim, 0.90f));
    const float maxW = ui.width() * 0.92f;
    const float maxH = ui.height() * 0.84f;
    const float scale = std::min(maxW / static_cast<float>(viewWidth_),
                                 maxH / static_cast<float>(viewHeight_));
    const float w = static_cast<float>(viewWidth_) * scale;
    const float h = static_cast<float>(viewHeight_) * scale;
    const Rect box {(ui.width() - w) * 0.5f, (ui.height() - h) * 0.5f - 14.0f, w, h};
    ui.shadow(box, {0, 20, 60, 0, col(Role::Shadow, 0.70f)}, 8);
    ui.setTexture(viewTexture_);
    ui.texturedRect(box, 0, 0, 1, 1);
    ui.setTexture(0);
    const TextStyle hint = widget::muted(13.0f);
    const std::string label = "click to close";
    text.drawInBox(ui, {0, box.bottom() + 10, ui.width(), 20}, label, hint,
                   TextAlign::Center);
  }

  confirm_.draw(ui, text);
}

}  // namespace hr::ui
