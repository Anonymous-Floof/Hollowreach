#include "ui/notify.h"

#include <algorithm>

#include "ui/settings.h"
#include "ui/widgets.h"

namespace hr::ui {
namespace {

// .toast { padding: 8px 12px; border-radius: 6px; border-left: 3px solid accent }
constexpr float kPadX = 12, kPadY = 8, kRadius = 6, kAccentBar = 3, kGap = 6;

}  // namespace

void Notify::push(std::string message, Toast kind) {
  if (message.empty()) return;
  // Asked here rather than at each of the fifty-odd call sites, so a toast added
  // later cannot forget the setting exists. The cost is one map lookup per toast,
  // against a thing that happens a few times a minute at most.
  //
  // EVERY toast, not only the routine ones. The split existed because silencing a
  // refusal ("you need a hoe for that") would make a control look broken rather than
  // quiet — but in practice the refusals are the ones that repeat, and a player who
  // turns notifications off has asked for silence, not for a curated subset of it.
  // `kind` still exists and still drives how a toast is styled.
  if (!settings().flag("notifications")) return;
  // A hard cap the JS did not need: the DOM would scroll off screen, whereas here a
  // runaway notifier would just paint over the whole right-hand side.
  if (toasts_.size() >= 8) toasts_.pop_front();
  toasts_.push_back({std::move(message), 0.0});
}

void Notify::update(double dt) {
  for (Entry& t : toasts_) t.age += dt;
  while (!toasts_.empty() && toasts_.front().age >= kToastLife) toasts_.pop_front();
}

void Notify::draw(Ui2D& ui, Text& text) {
  if (toasts_.empty()) return;
  const TextStyle ts = widget::toast();
  const TextMetrics m = text.metrics(ts);

  // #notify { right: 14px; top: 196px; align-items: flex-end; gap: 6px }
  float y = px(Scalar::ToastTop);
  for (const Entry& t : toasts_) {
    // @keyframes toastin { from { opacity: 0; transform: translateX(20px) } }
    // @keyframes toastout { to { opacity: 0; transform: translateX(20px) } }
    float alpha = 1.0f;
    float slide = 0.0f;
    if (t.age < kToastIn) {
      const float p = easeDefault(static_cast<float>(t.age / kToastIn));
      alpha = p;
      slide = (1.0f - p) * 20.0f;
    } else if (t.age > kToastHold) {
      const float p =
          easeDefault(std::min(1.0f, static_cast<float>((t.age - kToastHold) / kToastOut)));
      alpha = 1.0f - p;
      slide = p * 20.0f;
    }
    if (alpha <= 0.004f) continue;

    const float w = text.measure(t.message, ts) + kPadX * 2 + kAccentBar;
    const float h = m.lineHeight + kPadY * 2;
    const Rect box {ui.width() - px(Scalar::ToastRight) - w + slide, y, w, h};

    ui.fillRect(box, col(Role::ToastBg, alpha), kRadius);
    // border-left: 3px solid var(--accent) — a square-cornered bar over the rounded
    // background's left edge, which is how the browser renders a one-sided border on
    // a rounded box.
    ui.fillRect({box.x, box.y, kAccentBar, box.h}, fade(col(Role::Accent), alpha), 0);

    TextStyle drawStyle = ts;
    drawStyle.color = fade(ts.color, alpha);
    text.drawInBox(ui, {box.x + kAccentBar + kPadX, box.y + kPadY, box.w, m.lineHeight},
                   t.message, drawStyle);
    y += h + kGap;
  }
}

}  // namespace hr::ui
