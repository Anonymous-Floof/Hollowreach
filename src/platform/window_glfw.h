// Window, GL 3.3 core context, and event pump.
//
// Replaces the browser's <canvas> plus the Pointer Lock API. GLFW is confined
// to this file (and its .cpp) so nothing in src/game, src/world or src/render
// knows what windowing library is underneath.

#pragma once

#include <functional>
#include <string>

#include "core/input.h"

struct GLFWwindow;

namespace hr {

struct WindowConfig {
  int width = 1280;
  int height = 720;
  std::string title = "Hollowreach";
  bool vsync = true;
  // MSAA is deliberately absent: the scene renders into non-multisampled FBOs
  // and reaches the default framebuffer as one fullscreen textured triangle, so
  // a multisampled backbuffer would antialias nothing (js/core/gl.js:5-8).
};

class Window {
 public:
  Window() = default;
  ~Window();
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  // Creates the window and loads GL. Returns false with a message on failure —
  // the equivalent of the web build's "WebGL2 not available" card.
  bool create(const WindowConfig& cfg, std::string& errorOut);
  void destroy();

  void pollEvents();
  void swapBuffers();
  bool shouldClose() const;
  void requestClose();

  int width() const { return width_; }
  int height() const { return height_; }
  float aspect() const { return height_ > 0 ? static_cast<float>(width_) / height_ : 1.0f; }

  // Seconds since startup, monotonic. The web build's performance.now() / 1000.
  double time() const;

  Input& input() { return input_; }
  const Input& input() const { return input_; }

  // Pointer capture. The browser could only request it from a user gesture and
  // rejected rapid re-requests; GLFW has no such rule, so this is simpler than
  // the JS requestLock() and its swallowed promise.
  void setPointerCaptured(bool on);
  bool pointerCaptured() const;

  // Raw motion bypasses OS pointer acceleration. Off by default: the browser's
  // movementX is accelerated, and the ported sensitivity constant is tuned
  // against that, so raw input would feel wrong at the same setting.
  void setRawMouseMotion(bool on);
  bool rawMouseMotionSupported() const;

  void setVsync(bool on);
  void setTitle(const std::string& title);

  bool fullscreen() const { return fullscreen_; }
  void setFullscreen(bool on);
  bool borderless() const { return borderless_; }
  void setBorderless(bool on);

  // Fires after a resize, with the new framebuffer size.
  std::function<void(int, int)> onResize;
  // Fires when the OS takes focus away, so the game can auto-pause the way the
  // web build did on pointerlockchange.
  std::function<void(bool)> onFocusChange;

  // Clipboard, for the text fields (world name, seed, join code).
  std::string clipboardText() const;
  void setClipboardText(const std::string& text);

  GLFWwindow* handle() const { return window_; }

 private:
  GLFWwindow* window_ = nullptr;
  Input input_;
  int width_ = 0, height_ = 0;
  bool fullscreen_ = false;
  bool borderless_ = false;
  // What the player asked for, so a window-mode change can put it back rather
  // than forcing vsync on.
  bool vsync_ = true;
  void applyWindowMode(bool full, bool borderless);
  bool rawSupported_ = false;
  bool rawWanted_ = false;
  double lastMouseX_ = 0.0, lastMouseY_ = 0.0;
  bool haveLastMouse_ = false;
  // Windowed placement remembered across a fullscreen round trip.
  int savedX_ = 0, savedY_ = 0, savedW_ = 0, savedH_ = 0;

  void installCallbacks();
};

}  // namespace hr
