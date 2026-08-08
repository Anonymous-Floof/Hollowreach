// Screenshot gallery, ported from Menu.showGallery() in js/ui/menu.js and the .gal-*
// rules in css/style.css:212-250.
//
// The web build kept captures as data URLs in IndexedDB with a pre-baked thumbnail. The
// native build already writes real PNG files to data/screenshots, so this reads the
// directory instead — which is better in the ways that matter (the files are the player's,
// not locked in a browser database) and needs no save layer.
//
// Two things wait for later milestones and are marked where they would go: panoramas need
// F8 capture, which is a render feature, and "Set BG" needs the menu panorama to accept a
// cube map from disk.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/gl.h"
#include "ui/confirm.h"
#include "ui/dom.h"
#include "ui/text.h"
#include "ui/ui2d.h"
#include "ui/widgets.h"

namespace hr::ui {

class Gallery {
 public:
  ~Gallery();

  // Rescans the screenshots directory. Thumbnails are decoded lazily, a few per frame,
  // so opening a folder with two hundred captures does not stall.
  void onShow();
  // The same screen in "choose a picture" mode, for hanging one in a painting. A
  // mode rather than a second screen because everything a picker needs — the
  // directory scan, the lazy thumbnails, the grid, the scroll — is already here,
  // and a copy of it would be a second place to fix the next time any of that
  // changes. Cards become one big Choose button and the per-card actions go away.
  void onShowPicker();
  bool picking() const { return picking_; }
  void destroy();

  std::function<void()> onBack;
  // Fires with the chosen file's path, in picker mode only.
  std::function<void(const std::string&)> onPick;

  void update(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens);
  void draw(Ui2D& ui, Text& text);

 private:
  struct Item {
    std::string path;
    std::string name;
    double ageSeconds = 0;
    GLuint thumbnail = 0;
    int thumbWidth = 0, thumbHeight = 0;
    bool decoded = false;
    bool failed = false;
  };

  void build(Ui2D& ui, Text& text, const UiEvent& event);
  void handle(const UiEvent& event);
  // Decodes at most `budget` thumbnails, nearest-downsampled to card size.
  void decodeSome(int budget);

  std::vector<Item> items_;
  Doc doc_;
  int hoveredTag_ = 0;
  int hoveredIndex_ = 0;
  bool picking_ = false;
  // The full-screen still viewer. -1 when closed.
  ConfirmPrompt confirm_;
  int viewing_ = -1;
  GLuint viewTexture_ = 0;
  int viewWidth_ = 0, viewHeight_ = 0;
};

}  // namespace hr::ui
