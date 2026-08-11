#include "ui/confirm.h"

#include <algorithm>
#include <utility>

#include "ui/theme.h"

namespace hr::ui {
namespace {

constexpr float kCardW = 380.0f;
constexpr float kCardH = 150.0f;
constexpr float kPad = 20.0f;
constexpr float kButtonH = 38.0f;
// Two frames, not one. The click that opens a prompt is released a frame or two
// later, and a release landing on the confirm button is still a click as far as
// anything watching edges is concerned.
constexpr int kOpenGuard = 2;

}  // namespace

void ConfirmPrompt::open(std::string message, std::string confirmLabel,
                         std::function<void()> onConfirm) {
  active_ = true;
  message_ = std::move(message);
  confirmLabel_ = std::move(confirmLabel);
  onConfirm_ = std::move(onConfirm);
  hoverCancel_ = hoverConfirm_ = false;
  guard_ = kOpenGuard;
}

void ConfirmPrompt::close() {
  active_ = false;
  message_.clear();
  confirmLabel_.clear();
  onConfirm_ = nullptr;
  hoverCancel_ = hoverConfirm_ = false;
  guard_ = 0;
}

ConfirmPrompt::Layout ConfirmPrompt::layout(float viewW, float viewH) const {
  Layout l;
  const float w = std::min(kCardW, viewW - 40.0f);
  const float h = std::min(kCardH, viewH - 40.0f);
  l.card = {(viewW - w) * 0.5f, (viewH - h) * 0.5f, w, h};

  const float bw = (w - 3 * kPad) * 0.5f;
  const float by = l.card.bottom() - kPad - kButtonH;
  l.cancel = {l.card.x + kPad, by, bw, kButtonH};
  l.confirm = {l.card.x + kPad + bw + kPad, by, bw, kButtonH};
  return l;
}

bool ConfirmPrompt::handle(float viewW, float viewH, const UiEvent& event) {
  if (!active_) return false;

  // Armed but deaf for a moment, and it still returns true — the screen behind
  // must not act on these frames either, or the click that opened the prompt goes
  // on to do whatever it would have done underneath it.
  if (guard_ > 0) {
    --guard_;
    return true;
  }

  const Layout l = layout(viewW, viewH);
  hoverCancel_ = l.cancel.contains(event.mouseX, event.mouseY);
  hoverConfirm_ = l.confirm.contains(event.mouseX, event.mouseY);

  if (event.input && event.input->pressed(Key::Escape)) {
    close();
    return true;
  }
  if (!event.leftClick) return true;

  if (hoverConfirm_) {
    // Copied out and the prompt closed BEFORE the call: the action may open
    // another prompt, or tear down the screen this one belongs to, and either
    // would be writing into an object that is still mid-handle.
    std::function<void()> action = std::move(onConfirm_);
    close();
    if (action) action();
    return true;
  }
  // Anywhere else, including the card itself, is a no. Clicking away from a
  // destructive question should never be the destructive answer.
  close();
  return true;
}

void ConfirmPrompt::draw(Ui2D& ui, Text& text) const {
  if (!active_) return;
  const Layout l = layout(ui.width(), ui.height());

  ui.fillRect({0, 0, ui.width(), ui.height()}, col(Role::WashScreen));
  ui.shadow(l.card, BoxShadow{0, 18, 40, 0, col(Role::Shadow, 0.55f)}, 14.0f);
  ui.fillRect(l.card, col(Role::Panel), 14.0f);
  ui.strokeRect(l.card, col(Role::Edge), 1.0f, 14.0f);

  TextStyle title;
  title.font = FontId::SansBold;
  title.size = 16.0f;
  title.color = col(Role::Text);
  text.drawInBox(ui, {l.card.x + kPad, l.card.y + 26.0f, l.card.w - kPad * 2, 22.0f}, message_,
                 title, TextAlign::Center);

  TextStyle note;
  note.size = 12.5f;
  note.color = col(Role::Muted);
  text.drawInBox(ui, {l.card.x + kPad, l.card.y + 54.0f, l.card.w - kPad * 2, 18.0f},
                 "This cannot be undone.", note, TextAlign::Center);

  const auto button = [&](const Rect& r, const std::string& label, bool hovered, bool danger) {
    const Rgba bg = danger ? (hovered ? col(Role::Danger) : col(Role::DangerEdge))
                           : (hovered ? col(Role::PanelHover) : col(Role::PanelRaised));
    ui.fillRect(r, bg, 9.0f);
    ui.strokeRect(r, danger ? col(Role::Danger) : col(Role::Edge), 2.0f, 9.0f);
    TextStyle ts;
    ts.font = FontId::SansBold;
    ts.size = 14.0f;
    ts.color = col(Role::Text);
    const TextMetrics m = text.metrics(ts);
    text.drawInBox(ui, {r.x, r.y + (r.h - m.lineHeight) * 0.5f, r.w, m.lineHeight}, label, ts,
                   TextAlign::Center);
  };
  button(l.cancel, "Cancel", hoverCancel_, false);
  button(l.confirm, confirmLabel_.empty() ? "Delete" : confirmLabel_, hoverConfirm_, true);
}

}  // namespace hr::ui
