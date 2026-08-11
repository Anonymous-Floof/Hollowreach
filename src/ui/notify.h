// Transient toasts in the top-right, ported from js/ui/notify.js plus the .toast
// rules and the two keyframes in css/style.css:458-470.
//
// The JS appended a <div> and let CSS run `toastin .2s ease` and
// `toastout .3s ease 2.2s forwards`, then removed the element at 2.6s. Here each
// toast carries its own age and the same two animations are evaluated from it, so a
// queue of toasts stacks and expires identically.

#pragma once

#include <deque>
#include <cstdint>
#include <string>

#include "ui/text.h"
#include "ui/ui2d.h"

namespace hr::ui {

// What a toast is for, which decides whether the player can switch it off.
//
// **Routine** is the game narrating itself — "Autosaved", "Screenshot saved",
// "Welcome to Elder". Useful the first few times and wallpaper after that, which
// is the whole reason there is a setting.
//
// **Important** is the answer to something you just did: a refusal or a failure.
// "Could not save: disk full", "You need an Atlas", "That boat is taken". These
// ignore the setting, and that is deliberate — a control that silently does
// nothing is indistinguishable from a broken one, and somebody who turned
// notifications off a month ago will not connect the two. Hiding these would not
// be a quieter game, it would be a game that lies about why it refused.
enum class Toast : std::uint8_t { Routine, Important };

class Notify {
 public:
  // Routine by default, so a new call site has to opt IN to being unmuteable
  // rather than opting out. Wrong-by-default here costs a toast nobody sees;
  // wrong-by-default the other way costs the setting its meaning.
  void push(std::string message, Toast kind = Toast::Routine);
  void update(double dt);
  void draw(Ui2D& ui, Text& text);
  void clear() { toasts_.clear(); }
  bool empty() const { return toasts_.empty(); }

 private:
  // Named Entry rather than Toast: Toast is the public kind above, and a private
  // struct of the same name inside the class shadows it — so `push(std::string,
  // Toast)` declared out here and defined in the .cpp resolved to two different
  // types and would not link.
  struct Entry {
    std::string message;
    double age = 0;
  };
  std::deque<Entry> toasts_;
};

}  // namespace hr::ui
