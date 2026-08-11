#include "ui/chat.h"

#include <algorithm>

#include "core/input.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace hr::ui {
namespace {

constexpr float kMargin = 16.0f;
// Clears the whole #hud-bottom stack — hotbar, stats column, breath pips — so the
// box never lands on top of the hearts. The hotbar is centred and this is hard
// left, but the stats column is centred too and reaches further up than the bar
// does, and a health bar you cannot read because somebody is talking is worse than
// a chat box sitting a little high.
constexpr float kBottomClearance = 120.0f;
constexpr float kMaxWidth = 620.0f;
constexpr float kInputHeight = 30.0f;
constexpr float kRowHeight = 20.0f;
constexpr float kPad = 8.0f;
constexpr float kTextSize = 13.5f;

Rgba tintFor(Chat::Kind kind) {
  switch (kind) {
    case Chat::Kind::System: return color::chatSystem;
    case Chat::Kind::Whisper: return color::chatWhisper;
    case Chat::Kind::Reply: return color::chatReply;
    case Chat::Kind::Error: return color::danger;
    case Chat::Kind::Say: break;
  }
  return color::text;
}

TextStyle lineStyle() {
  TextStyle style;
  style.size = kTextSize;
  style.color = color::text;
  // Chat is drawn over a live world, which may be a snowfield or a cave. Without a
  // shadow half the lines are unreadable half the time, and which half depends on
  // where you happen to be standing.
  style.withShadow(0, 1, 2, rgba(0, 0, 0, 0.9f));
  return style;
}

float panelWidth(const Ui2D& ui) { return std::min(kMaxWidth, ui.width() * 0.55f); }

}  // namespace

void Chat::push(Kind kind, std::string text) {
  if (text.empty()) return;
  Line line;
  line.kind = kind;
  line.text = std::move(text);
  lines_.push_back(std::move(line));
  while (lines_.size() > kMaxLines) lines_.pop_front();
  // Anything arriving while you are reading back through the scrollback must not
  // pull the view down a line — that is the one moment the box is being read
  // rather than watched.
  if (scroll_ > 0 && scroll_ < lines_.size()) ++scroll_;
}

void Chat::clear() {
  lines_.clear();
  scroll_ = 0;
}

void Chat::open(Input* input, const std::string& prefill) {
  open_ = true;
  scroll_ = 0;
  recallAt_ = 0;
  draft_.clear();
  field_.maxLength = kMaxTyped;
  field_.setText(prefill);
  field_.setFocused(true, input);
  refresh();
}

void Chat::close(Input* input) {
  open_ = false;
  field_.setFocused(false, input);
  field_.clear();
  completion_ = {};
  selected_ = -1;
  lastValid_ = false;
  scroll_ = 0;
}

void Chat::update(double dt) {
  time_ += dt;
  for (Line& line : lines_) line.age += dt;
}

void Chat::refresh() {
  const std::string& line = field_.text();
  const std::size_t caret = field_.caret();
  // Rebuilt only when something moved. complete() walks the item and block
  // registries, and doing that sixty times a second while somebody stares at a
  // finished line is work nobody asked for.
  if (lastValid_ && line == lastLine_ && caret == lastCaret_) return;
  lastLine_ = line;
  lastCaret_ = caret;
  lastValid_ = true;
  completion_ = cmd::complete(line, caret, sources);
  // The highlight does not survive the list changing under it. Keeping an index
  // into a list that has been rebuilt is how Enter takes a word you never read.
  selected_ = -1;
}

bool Chat::moveSelection(int delta) {
  const int count = static_cast<int>(completion_.items.size());
  if (count == 0) return false;
  if (delta > 0) {
    selected_ = (selected_ + 1 >= count) ? 0 : selected_ + 1;
  } else {
    selected_ = (selected_ <= 0) ? count - 1 : selected_ - 1;
  }
  return true;
}

bool Chat::acceptSuggestion() {
  if (completion_.items.empty()) return false;
  const std::size_t index = static_cast<std::size_t>(std::max(selected_, 0));
  if (index >= completion_.items.size()) return false;

  std::string insert = completion_.items[index].text;
  // A trailing space, so accepting one argument leaves the caret ready for the
  // next. Not for the last one — there is nothing after it, and a line ending in a
  // space is one the parser has to trim anyway.
  const bool atEnd = completion_.end >= field_.text().size();
  if (atEnd) insert.push_back(' ');
  field_.replaceRange(completion_.begin, completion_.end, insert);
  refresh();
  return true;
}

void Chat::recall(int delta) {
  if (sent_.empty()) return;
  const int count = static_cast<int>(sent_.size());
  if (recallAt_ == 0 && delta > 0) draft_ = field_.text();
  recallAt_ = std::clamp(recallAt_ + delta, 0, count);
  if (recallAt_ == 0) {
    field_.setText(draft_);
  } else {
    field_.setText(sent_[static_cast<std::size_t>(count - recallAt_)]);
  }
  refresh();
}

void Chat::submit() {
  std::string line = field_.text();
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
  std::size_t start = 0;
  while (start < line.size() && line[start] == ' ') ++start;
  line.erase(0, start);

  if (!line.empty()) {
    // Remembered before it is handed on, because handling it may push lines of its
    // own and we want this one recallable either way. A repeat of the newest entry
    // is not stored twice: pressing Up should walk through what you have said, not
    // through how many times you said it.
    if (sent_.empty() || sent_.back() != line) sent_.push_back(line);
    while (sent_.size() > kMaxSent) sent_.erase(sent_.begin());
    if (onSubmit) onSubmit(line);
  }
}

bool Chat::handle(const UiEvent& event, Input* input) {
  if (!open_ || !event.input) return open_;
  const Input& in = *event.input;

  if (in.pressed(Key::Escape)) {
    close(input);
    return true;
  }

  // Scrollback. Held separately from the suggestion list because they are answers
  // to different questions — one is "what was said", the other "what could I type".
  if (in.pressed(Key::PageUp)) {
    scroll_ = std::min(scroll_ + kOpenLines / 2, lines_.empty() ? 0 : lines_.size() - 1);
  }
  if (in.pressed(Key::PageDown)) {
    scroll_ -= std::min(scroll_, kOpenLines / 2);
  }
  if (event.wheel != 0.0f) {
    // Browser sign: positive scrolls down, which walks back toward the newest.
    if (event.wheel > 0) {
      scroll_ -= std::min<std::size_t>(scroll_, 1);
    } else if (!lines_.empty() && scroll_ + 1 < lines_.size()) {
      ++scroll_;
    }
  }

  // Up and Down mean the suggestion list when there is one, and the lines you have
  // already sent when there is not. Two jobs for one pair of keys, and the rule for
  // which is simply whether anything is on offer — a command line almost always has
  // something, prose never does, so in practice each key does one thing in each of
  // the two places you use it.
  // Two jobs, one pair of keys. The rule is arrowsWalkHistory(), which is where it
  // is written down and where it is tested; typing ends a recall, which is what
  // keeps the two distinguishable.
  const bool history = arrowsWalkHistory();
  if (in.pressed(Key::Down)) {
    if (history) recall(-1); else moveSelection(1);
  }
  if (in.pressed(Key::Up)) {
    if (history) recall(1); else moveSelection(-1);
  }
  if (in.pressed(Key::Tab)) acceptSuggestion();

  bool submitted = false;
  if (field_.handle(event, submitted)) recallAt_ = 0;
  // Unconditionally, because the field reports only that its TEXT changed and the
  // suggestions depend on the caret too: Left past the end of a word has to stop
  // offering completions for the word after it. refresh() is a no-op when neither
  // has moved, so this costs a string compare.
  refresh();

  if (submitted) {
    if (selected_ >= 0) {
      // Something is highlighted, so this Enter takes it rather than sending. The
      // accept clears the highlight, so the next Enter sends — two presses, and no
      // way to send a word you did not read.
      acceptSuggestion();
    } else {
      submit();
      close(input);
    }
  }
  return true;
}

void Chat::draw(Ui2D& ui, Text& text) {
  if (lines_.empty() && !open_) return;

  const float width = panelWidth(ui);
  const float left = kMargin;
  const float bottom = ui.height() - kBottomClearance;
  const TextStyle base = lineStyle();
  const TextMetrics metrics = text.metrics(base);
  const float lineHeight = std::max(metrics.lineHeight, kTextSize + 3.0f);
  const std::size_t rows = open_ ? kOpenLines : kRestingLines;
  const float innerWidth = width - kPad * 2;

  // Laid out first, painted in three passes, and the ORDER is the whole point: the
  // popup has to land on top of the history. Drawn in one pass with the popup
  // first, the history painted straight back over it and both were legible at
  // once — a screenshot of two overlapping lists neither of which could be read.
  const Rect input {left, bottom - kInputHeight, width, kInputHeight};
  const Rect panel {left, bottom - kInputHeight - 4.0f - lineHeight * rows - kPad * 2, width,
                    lineHeight * rows + kPad * 2};
  float historyBottom = bottom;

  // 1. The backgrounds. Only while it is open: closed, the lines float over the
  // world with nothing but their own shadow, because a permanent dark rectangle in
  // the corner of every screenshot is not what a mostly-empty log should cost.
  if (open_) {
    ui.fillRect(panel, rgba(0, 0, 0, 0.42f), 8.0f);
    ui.fillRect(input, rgba(0, 0, 0, 0.72f), 8.0f);
    ui.strokeRect(input, color::accentDark, 1.5f, 8.0f);

    TextStyle typedStyle = base;
    typedStyle.color = color::text;
    // Inset sideways only: the field centres on its own line box, and squeezing the
    // height as well would push the caret off the bottom of a 30px row.
    field_.draw(ui, text, {input.x + kPad, input.y, input.w - kPad * 2, input.h}, typedStyle,
                time_);
    historyBottom = panel.bottom() - kPad;
    // Clipped to the panel so a line that wraps to three rows cannot climb out of
    // the top of it and float over the sky.
    ui.pushClip(panel);
  }

  // 2. The lines, newest at the bottom, wrapping upward. Bottom-up rather than
  // top-down because the bottom is the edge that is fixed: a line that wraps to
  // three rows has to push the older ones up, not itself down off the box.
  float y = historyBottom;
  const float top = historyBottom - lineHeight * rows;
  std::size_t index = lines_.size();
  index -= std::min(scroll_, index);

  while (index > 0 && y > top) {
    --index;
    const Line& line = lines_[index];

    float alpha = 1.0f;
    if (!open_) {
      if (line.age > kLingerSeconds + kFadeSeconds) continue;
      if (line.age > kLingerSeconds) {
        alpha = 1.0f - static_cast<float>((line.age - kLingerSeconds) / kFadeSeconds);
      }
    }

    TextStyle style = base;
    style.color = fade(tintFor(line.kind), alpha);
    for (int s = 0; s < style.shadowCount; ++s) {
      style.shadows[s].color = fade(style.shadows[s].color, alpha);
    }

    // Wrapped only when it has to be. Text::wrap splits on spaces and rejoins, so
    // it drops the leading ones — and the indented rows of a /help or a /list are
    // the one thing in this box whose leading spaces carry meaning. A line that
    // fits is drawn exactly as it was written.
    if (text.measure(line.text, style) <= innerWidth) {
      y -= lineHeight;
      text.drawInBox(ui, {left + kPad, y, innerWidth, lineHeight}, line.text, style);
      continue;
    }
    const std::vector<std::string> wrapped = text.wrap(line.text, style, innerWidth);
    for (std::size_t w = wrapped.size(); w > 0 && y > top; --w) {
      y -= lineHeight;
      text.drawInBox(ui, {left + kPad, y, innerWidth, lineHeight}, wrapped[w - 1], style);
    }
  }

  if (!open_) return;
  ui.popClip();

  if (scroll_ > 0) {
    TextStyle note = base;
    note.size = 11.5f;
    note.color = color::muted;
    text.drawInBox(ui, {left + kPad, panel.bottom(), innerWidth, 14.0f},
                   "\xE2\x86\x91 scrolled back \xC2\xB7 PageDown to return", note,
                   TextAlign::Right);
  }

  // 3. The completion list, last, so it lands on top of the history rather than
  // under it. It covers what was said, which is the right trade while typing: the
  // list is the thing being read.
  if (completion_.items.empty()) return;
  const std::size_t shown = completion_.items.size();
  const float popupH = kRowHeight * shown + kPad;
  const Rect popup {left, input.y - 4.0f - popupH, width, popupH};
  // Fully opaque, not merely nearly. At 0.97 the history behind it ghosted through
  // between the rows, which is exactly the unreadable overlap that drawing it in
  // the wrong order caused in the first place, only fainter.
  ui.fillRect(popup, rgba(4, 6, 10, 1.0f), 8.0f);
  ui.strokeRect(popup, color::accentDark, 1.0f, 8.0f);

  TextStyle label = base;
  label.shadowCount = 0;
  TextStyle hint = label;
  hint.size = 12.0f;

  for (std::size_t i = 0; i < shown; ++i) {
    const Rect row {popup.x + 4.0f, popup.y + kPad * 0.5f + kRowHeight * i, popup.w - 8.0f,
                    kRowHeight};
    const bool active = static_cast<int>(i) == selected_;
    if (active) ui.fillRect(row, color::accentDark, 5.0f);
    label.color = active ? color::text : color::muted;
    text.drawInBox(ui, row.inset(6.0f), completion_.items[i].label, label);
    if (!completion_.items[i].hint.empty()) {
      hint.color = active ? color::kbdText : color::slot;
      text.drawInBox(ui, row.inset(6.0f), completion_.items[i].hint, hint, TextAlign::Right);
    }
  }
}

}  // namespace hr::ui
