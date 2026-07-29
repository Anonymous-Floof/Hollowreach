// Keyboard and mouse state, ported from js/core/input.js.
//
// Same model as the web build: a held-key set, a just-pressed set cleared once
// per frame by endFrame(), mouse deltas and wheel accumulated only while the
// pointer is captured, and separate held/edge state for the mouse buttons.
//
// Two departures. Keys are an enum rather than DOM `event.code` strings, which
// the platform layer translates so GLFW stays out of game code. And text entry
// is an explicit mode: the JS could ask the DOM whether an <input> had focus,
// so beginTextCapture()/endTextCapture() takes that role and also suppresses
// gameplay keybinds while a field is focused.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hr {

enum class Key : std::uint16_t {
  Unknown = 0,
  A, B, C, D, E, F, G, H, I, J, K, L, M,
  N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
  Digit0, Digit1, Digit2, Digit3, Digit4,
  Digit5, Digit6, Digit7, Digit8, Digit9,
  Space, Enter, Escape, Backspace, Tab, Delete, Insert,
  Left, Right, Up, Down, Home, End, PageUp, PageDown,
  ShiftLeft, ShiftRight, ControlLeft, ControlRight, AltLeft, AltRight,
  SuperLeft, SuperRight, CapsLock,
  Minus, Equal, BracketLeft, BracketRight, Backslash,
  Semicolon, Apostrophe, Comma, Period, Slash, Grave,
  F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
  KeypadEnter, KeypadAdd, KeypadSubtract,
  Count
};

enum class MouseButton : std::uint8_t { Left = 0, Right = 1, Middle = 2, Count };

class Input {
 public:
  // --- keyboard ---
  bool down(Key k) const { return keys_[idx(k)]; }
  bool pressed(Key k) const { return justPressed_[idx(k)]; }
  bool released(Key k) const { return justReleased_[idx(k)]; }

  // Either shift/ctrl/alt, matching the JS checks that only cared about the
  // modifier being held at all.
  bool shift() const { return down(Key::ShiftLeft) || down(Key::ShiftRight); }
  bool ctrl() const { return down(Key::ControlLeft) || down(Key::ControlRight); }
  bool alt() const { return down(Key::AltLeft) || down(Key::AltRight); }

  // --- mouse ---
  bool buttonDown(MouseButton b) const { return buttons_[static_cast<int>(b)]; }
  bool clicked(MouseButton b) const { return clicks_[static_cast<int>(b)]; }
  bool releasedButton(MouseButton b) const { return buttonReleases_[static_cast<int>(b)]; }

  double mouseX() const { return mouseX_; }
  double mouseY() const { return mouseY_; }

  // Accumulated look delta since the last call; consuming it zeroes it, exactly
  // like the JS takeMouse(). Only accumulates while the pointer is captured.
  void takeMouse(double& dx, double& dy) {
    dx = mouseDX_;
    dy = mouseDY_;
    mouseDX_ = mouseDY_ = 0.0;
  }

  double takeWheel() {
    double w = wheel_;
    wheel_ = 0.0;
    return w;
  }

  // The wheel without consuming it. The interface reads it this way because an open
  // screen and the hotbar must not both act on one notch, and which of them gets it is
  // decided by the caller rather than by whoever asks first.
  double wheelPeek() const { return wheel_; }

  // --- pointer capture ---
  bool captured() const { return captured_; }

  // --- text entry ---
  // The JS could ask the DOM whether an <input> had focus; this takes that role.
  // While a field is focused, gameplay keybinds are suppressed by the caller and
  // typed characters accumulate here instead.
  void beginTextCapture();
  void endTextCapture();
  bool capturingText() const { return capturingText_; }

  // Code points typed since the last endFrame(), in order. The field that owns the
  // focus consumes these; Input deliberately does not own an edit buffer, because
  // there are eight text fields in the interface and each needs its own caret,
  // selection and maximum length.
  const std::vector<std::uint32_t>& typedText() const { return typed_; }

  // Clears per-frame edges. Call once at the end of every frame.
  void endFrame();

  // Drops all held state. Used when focus or pointer capture is lost, so a key
  // held at that moment does not stay stuck down.
  void clearHeld();

  // --- feed, called by the platform layer only ---
  void feedKey(Key k, bool downNow, bool repeat);
  void feedChar(std::uint32_t codepoint);
  void feedMouseButton(MouseButton b, bool downNow);
  void feedMouseMove(double x, double y, double dx, double dy);
  void feedWheel(double delta);
  void setCaptured(bool on);

 private:
  static constexpr int kKeyCount = static_cast<int>(Key::Count);
  static int idx(Key k) { return static_cast<int>(k); }

  std::array<bool, kKeyCount> keys_ {};
  std::array<bool, kKeyCount> justPressed_ {};
  std::array<bool, kKeyCount> justReleased_ {};

  std::array<bool, 3> buttons_ {};
  std::array<bool, 3> clicks_ {};
  std::array<bool, 3> buttonReleases_ {};

  double mouseX_ = 0.0, mouseY_ = 0.0;
  double mouseDX_ = 0.0, mouseDY_ = 0.0;
  double wheel_ = 0.0;
  bool captured_ = false;

  bool capturingText_ = false;
  std::vector<std::uint32_t> typed_;
};

}  // namespace hr
