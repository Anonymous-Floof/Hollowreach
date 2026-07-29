// Transient toasts in the top-right, ported from js/ui/notify.js plus the .toast
// rules and the two keyframes in css/style.css:458-470.
//
// The JS appended a <div> and let CSS run `toastin .2s ease` and
// `toastout .3s ease 2.2s forwards`, then removed the element at 2.6s. Here each
// toast carries its own age and the same two animations are evaluated from it, so a
// queue of toasts stacks and expires identically.

#pragma once

#include <deque>
#include <string>

#include "ui/text.h"
#include "ui/ui2d.h"

namespace hr::ui {

class Notify {
 public:
  void push(std::string message);
  void update(double dt);
  void draw(Ui2D& ui, Text& text);
  void clear() { toasts_.clear(); }
  bool empty() const { return toasts_.empty(); }

 private:
  struct Toast {
    std::string message;
    double age = 0;
  };
  std::deque<Toast> toasts_;
};

}  // namespace hr::ui
