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
    case Chat::Kind::System: return col(Role::ChatSystem);
    case Chat::Kind::Whisper: return col(Role::ChatWhisper);
    case Chat::Kind::Reply: return col(Role::ChatReply);
    case Chat::Kind::Error: return col(Role::Danger);
    case Chat::Kind::Say: break;
  }
  return col(Role::Text);
}

TextStyle lineStyle() {
  TextStyle style;
  style.size = kTextSize;
  style.color = col(Role::Text);
  // Chat is drawn over a live world, which may be a snowfield or a cave. Without a
  // shadow half the lines are unreadable half the time, and which half depends on
  // where you happen to be standing.
  style.withShadow(0, 1, 2, col(Role::Shadow, 0.90f));
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
  clearSelection();
  rows_.clear();
  inputBox_ = Rect{};
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

// ---- selecting the log with the mouse ---------------------------------------

std::size_t Chat::byteAt(Text& text, const Row& row, float x) const {
  const TextStyle style = lineStyle();
  const float local = x - row.rect.x;
  if (local <= 0.0f) return 0;
  // Walk code points, measuring the prefix, and take the boundary the click is
  // nearest to — the same rule TextField uses, and the same one a browser uses for
  // a hit test inside a run.
  std::size_t best = 0;
  float bestDistance = std::fabs(local);
  std::size_t i = 0;
  while (i < row.text.size()) {
    decodeUtf8(row.text, i);
    const float width = text.measure(row.text.substr(0, i), style);
    const float distance = std::fabs(local - width);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

Chat::Spot Chat::spotAt(Text& text, float x, float y) const {
  if (rows_.empty()) return {};
  // Clamped to the nearest row rather than requiring a hit, so dragging off the
  // top or bottom of the box extends the selection instead of dropping it.
  int nearest = 0;
  float bestDistance = 1e9f;
  for (std::size_t i = 0; i < rows_.size(); ++i) {
    const Rect& r = rows_[i].rect;
    const float distance = y < r.y ? r.y - y : (y > r.bottom() ? y - r.bottom() : 0.0f);
    if (distance < bestDistance) {
      bestDistance = distance;
      nearest = static_cast<int>(i);
    }
  }
  Spot spot;
  spot.row = nearest;
  spot.byte = byteAt(text, rows_[static_cast<std::size_t>(nearest)], x);
  return spot;
}

bool Chat::hasSelection() const {
  return selA_.valid() && selB_.valid() && !(selA_.row == selB_.row && selA_.byte == selB_.byte);
}

void Chat::clearSelection() {
  selA_ = selB_ = Spot{};
  dragging_ = false;
}

std::string Chat::extractRange(const std::vector<std::string>& rows, int rowA,
                               std::size_t byteA, int rowB, std::size_t byteB) {
  if (rows.empty() || rowA < 0 || rowB < 0) return {};
  // A drag runs whichever way the hand went; the text always comes back in
  // reading order.
  if (rowB < rowA || (rowA == rowB && byteB < byteA)) {
    std::swap(rowA, rowB);
    std::swap(byteA, byteB);
  }
  const int last = static_cast<int>(rows.size()) - 1;
  // Clamped rather than refused. The rows are last frame's layout and the log may
  // have scrolled since, so a range can legitimately point past the end — and
  // returning nothing at all would mean a copy that silently did nothing.
  rowA = std::min(rowA, last);
  rowB = std::min(rowB, last);

  std::string out;
  for (int r = rowA; r <= rowB; ++r) {
    const std::string& line = rows[static_cast<std::size_t>(r)];
    const std::size_t begin = r == rowA ? std::min(byteA, line.size()) : 0;
    const std::size_t end = r == rowB ? std::min(byteB, line.size()) : line.size();
    if (r != rowA) out.push_back('\n');
    if (end > begin) out += line.substr(begin, end - begin);
  }
  return out;
}

std::string Chat::selectedText(Text& text) const {
  (void)text;
  if (!hasSelection()) return {};
  std::vector<std::string> lines;
  lines.reserve(rows_.size());
  for (const Row& row : rows_) lines.push_back(row.text);
  return extractRange(lines, selA_.row, selA_.byte, selB_.row, selB_.byte);
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

bool Chat::handle(const UiEvent& event, Input* input, Text& text) {
  if (!open_ || !event.input) return open_;
  const Input& in = *event.input;

  if (in.pressed(Key::Escape)) {
    close(input);
    return true;
  }

  // --- the mouse: selecting the log, and placing the caret in the box ---------
  if (event.leftClick) {
    if (inputBox_.contains(event.mouseX, event.mouseY)) {
      clearSelection();
      TextStyle typed = lineStyle();
      typed.color = col(Role::Text);
      field_.placeCaretAt(text, {inputBox_.x + kPad, inputBox_.y, inputBox_.w - kPad * 2,
                                 inputBox_.h},
                          typed, event.mouseX);
    } else {
      // A press anywhere else starts a fresh selection. Starting one outside the
      // rows is deliberate: dragging from the empty space above the log down
      // through it is the natural way to grab the last few lines.
      clearSelection();
      selA_ = selB_ = spotAt(text, event.mouseX, event.mouseY);
      dragging_ = selA_.valid();
    }
  }
  if (dragging_ && event.leftDown) selB_ = spotAt(text, event.mouseX, event.mouseY);
  if (event.leftRelease) dragging_ = false;

  // --- the clipboard ---------------------------------------------------------
  // The log first: a selection there is what the mouse just made, and is what
  // somebody pressing Ctrl+C is looking at. Otherwise the box, so a typed line can
  // be copied like any other field.
  if (event.ctrl && in.pressed(Key::C)) {
    const std::string copied = hasSelection() ? selectedText(text) : field_.selectedText();
    if (!copied.empty() && onCopy) onCopy(copied);
  }
  if (event.ctrl && in.pressed(Key::X)) {
    const std::string cut = field_.selectedText();
    if (!cut.empty()) {
      if (onCopy) onCopy(cut);
      field_.eraseSelection();
      refresh();
    }
  }
  if (event.ctrl && in.pressed(Key::V) && onPaste) {
    const std::string pasted = onPaste();
    if (!pasted.empty()) {
      field_.insert(pasted);
      recallAt_ = 0;
      refresh();
    }
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
    ui.fillRect(panel, col(Role::WashPanel), 8.0f);
    ui.fillRect(input, col(Role::Scrim, 0.72f), 8.0f);
    ui.strokeRect(input, col(Role::AccentDeep), 1.5f, 8.0f);

    TextStyle typedStyle = base;
    typedStyle.color = col(Role::Text);
    // Inset sideways only: the field centres on its own line box, and squeezing the
    // height as well would push the caret off the bottom of a 30px row.
    field_.draw(ui, text, {input.x + kPad, input.y, input.w - kPad * 2, input.h}, typedStyle,
                time_);
    inputBox_ = input;  // so next frame's click can place the caret in it
    historyBottom = panel.bottom() - kPad;
    // Clipped to the panel so a line that wraps to three rows cannot climb out of
    // the top of it and float over the sky.
    ui.pushClip(panel);
  }

  // 2. The lines, newest at the bottom, wrapping upward. Bottom-up rather than
  // top-down because the bottom is the edge that is fixed: a line that wraps to
  // three rows has to push the older ones up, not itself down off the box.
  //
  // Laid out into rows_ first and painted afterwards, because the selection
  // highlight has to go UNDER the glyphs and the glyphs are produced bottom-up. It
  // is also what the mouse hit-tests against next frame — see Chat::Row.
  rows_.clear();
  std::vector<TextStyle> styles;
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
    const bool fits = text.measure(line.text, style) <= innerWidth;
    const std::vector<std::string> wrapped =
        fits ? std::vector<std::string>{line.text} : text.wrap(line.text, style, innerWidth);
    for (std::size_t w = wrapped.size(); w > 0 && y > top; --w) {
      y -= lineHeight;
      rows_.push_back(Row{{left + kPad, y, innerWidth, lineHeight}, wrapped[w - 1]});
      styles.push_back(style);
    }
  }
  // Built bottom-up; a Spot is ordered top-down, the way reading is.
  std::reverse(rows_.begin(), rows_.end());
  std::reverse(styles.begin(), styles.end());

  if (open_ && hasSelection()) {
    Spot from = selA_, to = selB_;
    if (before(to, from)) std::swap(from, to);
    for (int r = std::max(from.row, 0);
         r <= to.row && r < static_cast<int>(rows_.size()); ++r) {
      const Row& row = rows_[static_cast<std::size_t>(r)];
      const std::size_t begin = r == from.row ? std::min(from.byte, row.text.size()) : 0;
      const std::size_t end = r == to.row ? std::min(to.byte, row.text.size()) : row.text.size();
      if (end <= begin) continue;
      const float x0 = row.rect.x + text.measure(row.text.substr(0, begin), styles[r]);
      const float x1 = row.rect.x + text.measure(row.text.substr(0, end), styles[r]);
      ui.fillRect({x0, row.rect.y, std::max(x1 - x0, 2.0f), row.rect.h}, col(Role::AccentDeep), 2.0f);
    }
  }

  for (std::size_t r = 0; r < rows_.size(); ++r) {
    text.drawInBox(ui, rows_[r].rect, rows_[r].text, styles[r]);
  }

  if (!open_) return;
  ui.popClip();

  if (scroll_ > 0) {
    TextStyle note = base;
    note.size = 11.5f;
    note.color = col(Role::Muted);
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
  ui.fillRect(popup, col(Role::WashPopup), 8.0f);
  ui.strokeRect(popup, col(Role::AccentDeep), 1.0f, 8.0f);

  TextStyle label = base;
  label.shadowCount = 0;
  TextStyle hint = label;
  hint.size = 12.0f;

  for (std::size_t i = 0; i < shown; ++i) {
    const Rect row {popup.x + 4.0f, popup.y + kPad * 0.5f + kRowHeight * i, popup.w - 8.0f,
                    kRowHeight};
    const bool active = static_cast<int>(i) == selected_;
    if (active) ui.fillRect(row, col(Role::AccentDeep), 5.0f);
    label.color = active ? col(Role::Text) : col(Role::Muted);
    text.drawInBox(ui, row.inset(6.0f), completion_.items[i].label, label);
    if (!completion_.items[i].hint.empty()) {
      hint.color = active ? col(Role::InkKbd) : col(Role::SlotFill);
      text.drawInBox(ui, row.inset(6.0f), completion_.items[i].hint, hint, TextAlign::Right);
    }
  }
}

}  // namespace hr::ui
