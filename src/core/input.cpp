#include "core/input.h"

namespace hr {

void Input::feedKey(Key k, bool downNow, bool repeat) {
  const int i = idx(k);
  if (i <= 0 || i >= kKeyCount) return;

  if (downNow) {
    // Auto-repeat must not re-fire justPressed: the JS relied on the browser
    // firing keydown repeatedly but only ever tested membership in a Set, so an
    // edge-triggered action (open inventory, drop item) fired once per press.
    if (!repeat && !keys_[i]) justPressed_[i] = true;
    keys_[i] = true;
  } else {
    if (keys_[i]) justReleased_[i] = true;
    keys_[i] = false;
  }
}

void Input::feedChar(std::uint32_t codepoint) {
  if (!capturingText_) return;
  // Control characters arrive as key events, not text; the delete key in
  // particular sends 0x7F on some platforms.
  if (codepoint < 0x20 || codepoint == 0x7F) return;
  typed_.push_back(codepoint);
}

void Input::feedMouseButton(MouseButton b, bool downNow) {
  const int i = static_cast<int>(b);
  if (i < 0 || i >= 3) return;
  if (downNow) {
    if (!buttons_[i]) clicks_[i] = true;
    buttons_[i] = true;
  } else {
    if (buttons_[i]) buttonReleases_[i] = true;
    buttons_[i] = false;
  }
}

void Input::feedMouseMove(double x, double y, double dx, double dy) {
  mouseX_ = x;
  mouseY_ = y;
  // Look deltas are only meaningful while captured; when the cursor is visible
  // the position is what the UI wants instead.
  if (captured_) {
    mouseDX_ += dx;
    mouseDY_ += dy;
  }
}

void Input::feedWheel(double delta) { wheel_ += delta; }

void Input::setCaptured(bool on) {
  if (captured_ == on) return;
  captured_ = on;
  // Losing capture mid-drag would otherwise leave the button stuck down, which
  // in the web build showed up as mining continuing after alt-tab.
  buttons_ = {};
  mouseDX_ = mouseDY_ = 0.0;
}

void Input::beginTextCapture() {
  capturingText_ = true;
  typed_.clear();
}

void Input::endTextCapture() {
  capturingText_ = false;
  typed_.clear();
}

void Input::endFrame() {
  justPressed_ = {};
  justReleased_ = {};
  clicks_ = {};
  buttonReleases_ = {};
  wheel_ = 0.0;
  typed_.clear();
}

void Input::clearHeld() {
  keys_ = {};
  justPressed_ = {};
  justReleased_ = {};
  buttons_ = {};
  clicks_ = {};
  buttonReleases_ = {};
  mouseDX_ = mouseDY_ = 0.0;
  wheel_ = 0.0;
  typed_.clear();
}

}  // namespace hr
