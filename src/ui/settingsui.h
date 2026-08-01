// The settings screen: a tab strip over a scrolling panel of rows.
// Ported from renderSettings() in js/ui/settings.js and the .settings-* rules.
//
// Driven entirely by the schema, exactly as the original was, so a new row in
// settingsSchema() appears here with no edit to this file. Two controls have no native
// equivalent: <input type=range> becomes a track the caller can drag or click, and
// <select> becomes a button that cycles its options, which is what the CSS already made
// it look like (`class="btn small"`).

#pragma once

#include <functional>
#include <string>

#include "ui/dom.h"
#include "ui/settings.h"
#include "ui/text.h"
#include "ui/ui2d.h"
#include "ui/widgets.h"

namespace hr::ui {

class SettingsScreen {
 public:
  // Fired on every change, with the key that changed. App re-reads the store and pushes
  // everything into its subsystem, which is what makes the change live.
  std::function<void(const std::string& key)> onChange;
  std::function<void()> onBack;

  // Remembers the open tab across visits, like the module-scoped `activeTab` did.
  void onShow();

  void update(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens);
  void draw(Ui2D& ui, Text& text);
  // See Menu::setHasBackdrop — same choice, same reason.
  void setHasBackdrop(bool on) { hasBackdrop_ = on; }

 private:
  bool hasBackdrop_ = false;
  void build(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens);
  void handle(const UiEvent& event);
  void setValueFromDrag(const SettingDef& def, const Rect& track, float mouseX);

  Doc doc_;
  std::string activeTab_;
  int hoveredTag_ = 0;
  int hoveredIndex_ = 0;
  // The slider being dragged, as an index into the schema, or -1.
  int draggingSlider_ = -1;
};

}  // namespace hr::ui
