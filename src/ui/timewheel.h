// The Time Wheel: what a bed shows you.
//
// A 24-hour dial with the current hour marked on it and a handle you drag to the
// hour you want to wake at. Two things it deliberately is, in the spirit of the
// games this is borrowed from:
//
//  * **A clock.** Opening a bed and closing it again tells you the time and costs
//    nothing. Hollowreach has no watch item, and the HUD clock is a debug readout;
//    this is the diegetic one, and it is the reason the screen is worth having even
//    when you cannot sleep.
//  * **A choice.** You sleep until an hour you picked, not until dawn. A nap
//    through the worst of a night is a different decision from sleeping the whole
//    of it, and the wheel is where that decision gets made.
//
// The rim is painted with the day it describes — lit through the middle of the
// day, dark through the night, warm at the two edges — so where you want the
// handle is legible before you have read a single number.

#pragma once

#include <functional>
#include <string>

#include "render/sky.h"
#include "ui/text.h"
#include "ui/ui2d.h"
#include "ui/widgets.h"

namespace hr::ui {

class TimeWheel {
 public:
  // A bed opened normally: the handle starts at dawn, or at the current hour when
  // that is already past dawn, and you may move it anywhere.
  void open(float now);
  // A bed opened while somebody else is already waiting to sleep. The hour is
  // theirs and the wheel is read-only; all this screen can do is agree or not.
  void openVote(float now, float proposed, const std::string& who);
  void close() { voter_.clear(); }

  float target() const { return target_; }
  bool isVote() const { return !voter_.empty(); }

  // Screen direction to a 0..1 clock reading, and back. Midnight is straight up
  // and the hours run clockwise, because that is the direction of every clock face
  // the player has ever looked at. Public because they are the whole geometry of
  // the dial and the one part of it worth testing without a window.
  static float angleToTime(float dx, float dy);
  static void timeToUnit(float t, float& ux, float& uy);

  // Input and layout. `sky` supplies the current hour and how long the player has
  // been awake; nothing about tiredness is cached, so a wheel left open while the
  // clock runs past the eight-hour mark unlocks itself.
  void update(Ui2D& ui, Text& text, const UiEvent& event, const render::Sky& sky);
  void draw(Ui2D& ui, Text& text, const render::Sky& sky);

  // Confirm hands back the chosen hour on the 0..1 clock. Cancel is also what
  // Escape does, and what a bed used purely as a clock does.
  std::function<void(float)> onConfirm;
  std::function<void()> onCancel;

 private:
  struct Layout {
    Rect card;
    float cx = 0, cy = 0;   // dial centre
    float radius = 0;       // outer rim
    float thickness = 0;    // rim band
    Rect confirm, cancel;
  };
  Layout layout(Ui2D& ui) const;
  bool canSleep(const render::Sky& sky) const;

  float target_ = render::Sky::kDawn;
  std::string voter_;   // non-empty while this is a vote on someone else's hour
  bool dragging_ = false;
  bool hoverConfirm_ = false;
  bool hoverCancel_ = false;
};

}  // namespace hr::ui
