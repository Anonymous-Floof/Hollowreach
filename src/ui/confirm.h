// "Are you sure?" — once, for everything that destroys something.
//
// Every delete in the game used to happen on the first click. A world
// (ui/menu.cpp), a screenshot (ui/gallery.cpp) and an Atlas waypoint (ui/map.cpp)
// all went straight to erase, and the world is the one that hurts: a misclick on a
// row you did not mean to be hovering, and it is gone with no copy anywhere.
//
// Three call sites is past the point of writing three dialogs, so this is written
// once. It is deliberately NOT a Doc page: only the menu has pages, and the Gallery
// and the Atlas are single screens with nowhere to navigate to. It combines the two
// patterns the codebase already had for this shape —
//
//   * TimeWheel hand-lays a card with explicit button rects and hit-tests them
//     itself, which is the whole visual and layout job, already solved once.
//   * Gallery::viewing_ is the input-interception half: a state field checked at
//     the top of handle() that consumes the frame's click and returns before
//     anything behind it sees the event.
//
// It also has no GL and no Doc, which means — unlike almost everything else in
// src/ui — it can be tested directly. That is a large part of the argument for
// pulling it out rather than writing the same twenty lines three times.

#pragma once

#include <functional>
#include <string>

#include "ui/text.h"
#include "ui/ui2d.h"
#include "ui/widgets.h"

namespace hr::ui {

class ConfirmPrompt {
 public:
  // Arms the prompt. `message` should name the thing — "Delete Elder?" rather than
  // "Are you sure?" — because the failure this exists to prevent is acting on the
  // wrong row, and a yes/no with no subject cannot tell you that you are about to.
  void open(std::string message, std::string confirmLabel, std::function<void()> onConfirm);

  void close();
  bool active() const { return active_; }

  // Consumes this frame's input. Returns true when the prompt took it, in which
  // case the caller must return before touching anything of its own — the whole
  // point is that the screen behind cannot act on a click meant for the prompt.
  //
  // Takes the viewport as two numbers rather than the Ui2D it will later draw
  // into, because Ui2D::begin issues GL calls and a class that needs a live
  // context to decide whether a click landed on a button is a class nothing can
  // test. Deciding is arithmetic; only drawing needs the renderer.
  bool handle(float viewW, float viewH, const UiEvent& event);

  // Draws over whatever the screen has already painted.
  void draw(Ui2D& ui, Text& text) const;

 private:
  struct Layout {
    Rect card, cancel, confirm;
  };
  Layout layout(float viewW, float viewH) const;

  bool active_ = false;
  std::string message_;
  std::string confirmLabel_;
  std::function<void()> onConfirm_;
  bool hoverCancel_ = false;
  bool hoverConfirm_ = false;
  // Frames of input to swallow after opening. A prompt appears under a cursor that
  // is already mid-click on the button that opened it, and without this the same
  // click confirms it — the same failure Interface::guardMouse exists for when a
  // whole screen opens, which this cannot use because no screen changed.
  int guard_ = 0;
};

}  // namespace hr::ui
