// The Resource Packs screen.
//
// One list, not Minecraft's two. Minecraft splits available from selected and
// makes you drag between them; the same information fits in a single list where
// the enabled packs sit at the top in the order they are applied and everything
// else follows, because the question a player is answering — "which of these are
// on, and which one wins" — is one question about one list.
//
// Priority runs top-down: the first row is the pack whose files win. That is
// PackStack's own documented convention and Minecraft's screen order, so there is
// one answer to remember rather than one per layer.
//
// The screen owns nothing but its view of the folder. Turning a pack on writes the
// ordered id list to settings and asks App to rebuild the sound bank; the scan
// itself is re-read on every open, so a pack dropped in while the game is running
// appears without a restart.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "resource/pack.h"
#include "ui/dom.h"
#include "ui/text.h"
#include "ui/ui2d.h"
#include "ui/widgets.h"

namespace hr::ui {

class PacksScreen {
 public:
  // Applies the current selection: fires after every change, with the enabled
  // ids already written to settings. App rebuilds the sound bank from it.
  std::function<void()> onApply;
  std::function<void()> onBack;
  // Writes a fill-in-the-blanks pack into data/resourcepacks and rescans. App
  // supplies it because the event list belongs to the audio layer, not here.
  std::function<bool(std::string& messageOut)> onCreateExample;

  // Rescans data/resourcepacks. Cheap — a directory walk and a pack.mcmeta per
  // folder — so it runs on every open rather than being cached and going stale.
  void onShow();
  void refresh();

  void update(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens);
  void draw(Ui2D& ui, Text& text);
  void setHasBackdrop(bool on) { hasBackdrop_ = on; }

  // What the last rebuild loaded, shown under the title. Set by App, because the
  // bank is what knows and the screen should not reach into the audio layer to
  // ask.
  void setSummary(std::string line) { summary_ = std::move(line); }

 private:
  // One display row: a pack, and whether it is on. Rebuilt from the scan and the
  // saved selection every refresh, so the two can never disagree.
  struct Row {
    resource::PackInfo pack;
    bool enabled = false;
  };

  void build(Ui2D& ui, Text& text, const UiEvent& event, TweenStore& tweens);
  void handle(const UiEvent& event);
  // Writes the enabled ids, in row order, and fires onApply.
  void commit();
  void move(int index, int delta);

  std::vector<Row> rows_;
  std::string summary_;
  std::string notice_;
  Doc doc_;
  bool hasBackdrop_ = false;
  int hoveredTag_ = 0;
  int hoveredIndex_ = 0;
};

}  // namespace hr::ui
