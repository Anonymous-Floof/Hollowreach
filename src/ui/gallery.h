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
  void destroy();

  std::function<void()> onBack;

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
  // The full-screen still viewer. -1 when closed.
  int viewing_ = -1;
  GLuint viewTexture_ = 0;
  int viewWidth_ = 0, viewHeight_ = 0;
};

}  // namespace hr::ui
