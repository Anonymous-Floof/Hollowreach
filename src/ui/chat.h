// The chat box: what everyone said, and the line you are typing.
//
// NOT a Screen. Every other interface in this game takes the world away from you —
// the inventory, the map, the pause menu all stop being able to see past
// themselves. Chat has to be the opposite: the reason you are typing is usually
// something you are looking at, and a command whose effect you cannot watch happen
// is a command you have to run twice to believe. So this draws over a live world,
// and App keeps the world stepping while it is open.
//
// What it does take is the keyboard, which is the whole reason it needs a mode at
// all: `W` while chatting is a letter, not a step forward.
//
// THE SUGGESTION POPUP, and the one decision in it worth stating. The highlight
// starts at *nothing selected*, not at the first row. If it started at row 0 then
// Enter would always accept a completion and never send the line, and the common
// case — typing a command you already know in full and pressing Enter — would put
// somebody else's word in your line. So:
//
//   * nothing selected: Enter sends, Tab takes the best match.
//   * something selected (you pressed Down): Enter takes it, and selects nothing
//     again, so a second Enter sends. Two presses to accept and send, and no way to
//     send something you did not read.
//
// Typing anything clears the selection, which keeps that promise true after the
// list underneath it has changed.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "cmd/complete.h"
#include "ui/text.h"
#include "ui/ui2d.h"
#include "ui/widgets.h"

namespace hr {
class Input;
}

namespace hr::ui {

class Chat {
 public:
  // What a line is, which decides its colour and nothing else. Kept narrow on
  // purpose: a line's meaning belongs to whoever wrote it, and a chat box that
  // classifies text into nine kinds is a chat box nobody can read at a glance.
  enum class Kind : std::uint8_t {
    Say,      // somebody talking
    System,   // the world talking: joins, leaves, announcements
    Whisper,  // to or from one person
    Reply,    // the answer to a command you ran
    Error,    // a refusal
  };

  // How many lines are kept. Older ones are dropped from the front, so a long
  // session cannot grow this without bound.
  static constexpr std::size_t kMaxLines = 200;
  // How many are shown while closed, and how long they linger before fading. Both
  // match the feel of standing in a world with the box shut: enough to catch what
  // was said, not so much that the corner of the screen becomes a wall of text.
  static constexpr std::size_t kRestingLines = 8;
  static constexpr double kLingerSeconds = 10.0;
  static constexpr double kFadeSeconds = 1.5;
  // How many the open box shows at once, and how many past lines the input
  // remembers.
  static constexpr std::size_t kOpenLines = 14;
  static constexpr std::size_t kMaxSent = 50;
  // A line long enough to say anything and short enough that one person cannot
  // fill everybody's screen. Matches net::kMaxChat, which is what the wire allows.
  static constexpr int kMaxTyped = 256;

  void push(Kind kind, std::string text);
  void clear();
  bool empty() const { return lines_.empty(); }

  // `prefill` is "/" when opened with the slash key, empty when opened with T.
  void open(Input* input, const std::string& prefill);
  void close(Input* input);
  bool isOpen() const { return open_; }

  // Live facts the completer cannot know: who is here, and what the typist is
  // allowed to do. Set by App before handle() every frame.
  cmd::Sources sources;
  // A finished line, already trimmed and never empty.
  std::function<void(const std::string&)> onSubmit;

  void update(double dt);
  // Consumes the frame's keyboard while open, and reports that it did so the rest
  // of the interface leaves it alone.
  bool handle(const UiEvent& event, Input* input);
  void draw(Ui2D& ui, Text& text);

  // --- for the tests ---------------------------------------------------------
  // The overlay needs a GL context to draw and an Input to type into, so the
  // behaviour worth testing is exposed directly: which row is highlighted, what is
  // on offer, and what accepting one does to the line.
  const std::vector<cmd::Suggestion>& suggestions() const { return completion_.items; }
  int selected() const { return selected_; }
  const std::string& typed() const { return field_.text(); }
  void setTyped(const std::string& text) {
    field_.setText(text);
    refresh();
  }
  // Moves the highlight the way Down and Up do. Returns false when there is
  // nothing to move through, which is when the arrow keys walk the sent history
  // instead.
  bool moveSelection(int delta);
  // Walks the lines already sent, newest first. Positive goes back in time.
  void recallSent(int delta) { recall(delta); }
  int recallDepth() const { return recallAt_; }
  // Sends what is in the box, exactly as Enter does with nothing highlighted.
  void sendTyped() { submit(); }

  // Which of their two jobs the arrow keys are doing right now. Named rather than
  // written inline in handle() because it IS the rule, and a rule buried in a key
  // handler is one no test can reach: the suggestion list and the sent-line
  // history are answers to different questions sharing one pair of keys.
  //
  // Once a recall has started it continues, even though a recalled command brings
  // a suggestion list back with it. Otherwise the second press of Up silently
  // stops going back in time and starts moving through that list instead.
  bool arrowsWalkHistory() const { return recallAt_ > 0 || completion_.items.empty(); }
  // Puts the highlighted suggestion — or the best one, when none is highlighted —
  // into the line. False when there was nothing to take.
  bool acceptSuggestion();
  std::size_t lineCount() const { return lines_.size(); }

 private:
  struct Line {
    Kind kind = Kind::Say;
    std::string text;
    double age = 0;
  };

  // Rebuilds the suggestion list from the current line and caret. Cheap enough to
  // call on every edit, and deliberately NOT called every frame: it walks the item
  // and block registries, and the answer cannot change while nothing is typed.
  void refresh();
  void submit();
  void recall(int delta);

  std::deque<Line> lines_;
  std::vector<std::string> sent_;
  // Where in `sent_` the arrow keys have walked, counted back from the end. 0 is
  // "not recalling anything".
  int recallAt_ = 0;
  // What was being typed before the first recall, so walking back down past the
  // newest entry returns the half-written line rather than an empty box.
  std::string draft_;

  TextField field_;
  cmd::Completion completion_;
  int selected_ = -1;  // -1 is "nothing highlighted"; see the header note
  // What refresh() last ran on, so it only runs when one of them has moved.
  std::string lastLine_;
  std::size_t lastCaret_ = 0;
  bool lastValid_ = false;

  // Its own clock, the way every other screen with a text field keeps one — the
  // caret blink is the only thing that needs it and nothing hands one down.
  double time_ = 0;
  bool open_ = false;
  // How far up the scrollback the box is looking, in whole lines from the newest.
  std::size_t scroll_ = 0;
};

}  // namespace hr::ui
