#include "platform/window_glfw.h"

// core/gl.h declares the GL 3.3 entry points, so GLFW must not include the
// platform <GL/gl.h>. Set in CMake too; repeated here so the file is correct on
// its own.
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

#include <unordered_map>

#include "core/gl.h"
#include "core/log.h"

namespace hr {
namespace {

int g_glfwRefs = 0;

Key translateKey(int glfwKey) {
  // Built once; GLFW key codes are sparse, so a table beats a switch for the
  // punctuation range and keeps the mapping readable as data.
  static const std::unordered_map<int, Key> kMap = {
      {GLFW_KEY_A, Key::A},                     {GLFW_KEY_B, Key::B},
      {GLFW_KEY_C, Key::C},                     {GLFW_KEY_D, Key::D},
      {GLFW_KEY_E, Key::E},                     {GLFW_KEY_F, Key::F},
      {GLFW_KEY_G, Key::G},                     {GLFW_KEY_H, Key::H},
      {GLFW_KEY_I, Key::I},                     {GLFW_KEY_J, Key::J},
      {GLFW_KEY_K, Key::K},                     {GLFW_KEY_L, Key::L},
      {GLFW_KEY_M, Key::M},                     {GLFW_KEY_N, Key::N},
      {GLFW_KEY_O, Key::O},                     {GLFW_KEY_P, Key::P},
      {GLFW_KEY_Q, Key::Q},                     {GLFW_KEY_R, Key::R},
      {GLFW_KEY_S, Key::S},                     {GLFW_KEY_T, Key::T},
      {GLFW_KEY_U, Key::U},                     {GLFW_KEY_V, Key::V},
      {GLFW_KEY_W, Key::W},                     {GLFW_KEY_X, Key::X},
      {GLFW_KEY_Y, Key::Y},                     {GLFW_KEY_Z, Key::Z},
      {GLFW_KEY_0, Key::Digit0},                {GLFW_KEY_1, Key::Digit1},
      {GLFW_KEY_2, Key::Digit2},                {GLFW_KEY_3, Key::Digit3},
      {GLFW_KEY_4, Key::Digit4},                {GLFW_KEY_5, Key::Digit5},
      {GLFW_KEY_6, Key::Digit6},                {GLFW_KEY_7, Key::Digit7},
      {GLFW_KEY_8, Key::Digit8},                {GLFW_KEY_9, Key::Digit9},
      {GLFW_KEY_SPACE, Key::Space},             {GLFW_KEY_ENTER, Key::Enter},
      {GLFW_KEY_ESCAPE, Key::Escape},           {GLFW_KEY_BACKSPACE, Key::Backspace},
      {GLFW_KEY_TAB, Key::Tab},                 {GLFW_KEY_DELETE, Key::Delete},
      {GLFW_KEY_INSERT, Key::Insert},           {GLFW_KEY_LEFT, Key::Left},
      {GLFW_KEY_RIGHT, Key::Right},             {GLFW_KEY_UP, Key::Up},
      {GLFW_KEY_DOWN, Key::Down},               {GLFW_KEY_HOME, Key::Home},
      {GLFW_KEY_END, Key::End},                 {GLFW_KEY_PAGE_UP, Key::PageUp},
      {GLFW_KEY_PAGE_DOWN, Key::PageDown},      {GLFW_KEY_LEFT_SHIFT, Key::ShiftLeft},
      {GLFW_KEY_RIGHT_SHIFT, Key::ShiftRight},  {GLFW_KEY_LEFT_CONTROL, Key::ControlLeft},
      {GLFW_KEY_RIGHT_CONTROL, Key::ControlRight}, {GLFW_KEY_LEFT_ALT, Key::AltLeft},
      {GLFW_KEY_RIGHT_ALT, Key::AltRight},      {GLFW_KEY_LEFT_SUPER, Key::SuperLeft},
      {GLFW_KEY_RIGHT_SUPER, Key::SuperRight},  {GLFW_KEY_CAPS_LOCK, Key::CapsLock},
      {GLFW_KEY_MINUS, Key::Minus},             {GLFW_KEY_EQUAL, Key::Equal},
      {GLFW_KEY_LEFT_BRACKET, Key::BracketLeft}, {GLFW_KEY_RIGHT_BRACKET, Key::BracketRight},
      {GLFW_KEY_BACKSLASH, Key::Backslash},     {GLFW_KEY_SEMICOLON, Key::Semicolon},
      {GLFW_KEY_APOSTROPHE, Key::Apostrophe},   {GLFW_KEY_COMMA, Key::Comma},
      {GLFW_KEY_PERIOD, Key::Period},           {GLFW_KEY_SLASH, Key::Slash},
      {GLFW_KEY_GRAVE_ACCENT, Key::Grave},      {GLFW_KEY_F1, Key::F1},
      {GLFW_KEY_F2, Key::F2},                   {GLFW_KEY_F3, Key::F3},
      {GLFW_KEY_F4, Key::F4},                   {GLFW_KEY_F5, Key::F5},
      {GLFW_KEY_F6, Key::F6},                   {GLFW_KEY_F7, Key::F7},
      {GLFW_KEY_F8, Key::F8},                   {GLFW_KEY_F9, Key::F9},
      {GLFW_KEY_F10, Key::F10},                 {GLFW_KEY_F11, Key::F11},
      {GLFW_KEY_F12, Key::F12},                 {GLFW_KEY_KP_ENTER, Key::KeypadEnter},
      {GLFW_KEY_KP_ADD, Key::KeypadAdd},        {GLFW_KEY_KP_SUBTRACT, Key::KeypadSubtract},
  };
  auto it = kMap.find(glfwKey);
  return it == kMap.end() ? Key::Unknown : it->second;
}

void glfwErrorCallback(int code, const char* description) {
  log::error("GLFW error %d: %s", code, description);
}

}  // namespace

Window::~Window() { destroy(); }

bool Window::create(const WindowConfig& cfg, std::string& errorOut) {
  glfwSetErrorCallback(&glfwErrorCallback);

  if (g_glfwRefs == 0 && !glfwInit()) {
    errorOut = "Failed to initialise GLFW.";
    return false;
  }
  ++g_glfwRefs;

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  glfwWindowHint(GLFW_SAMPLES, 0);
  glfwWindowHint(GLFW_ALPHA_BITS, 0);
  glfwWindowHint(GLFW_DEPTH_BITS, 24);
  // The whole pipeline is LDR RGBA8 with no linearisation, so an sRGB default
  // framebuffer would double-encode every pixel.
  glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_FALSE);
#if defined(HR_DEBUG)
  glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#endif

  window_ = glfwCreateWindow(cfg.width, cfg.height, cfg.title.c_str(), nullptr, nullptr);
  if (!window_) {
    errorOut =
        "Could not create an OpenGL 3.3 context. Hollowreach needs a GPU and driver "
        "supporting OpenGL 3.3 core (2010 or later).";
    return false;
  }

  glfwMakeContextCurrent(window_);
  glfwSwapInterval(cfg.vsync ? 1 : 0);

  const char* missing = nullptr;
  if (!gl::load(reinterpret_cast<void* (*)(const char*)>(glfwGetProcAddress), &missing)) {
    errorOut = std::string("The OpenGL driver is missing ") + (missing ? missing : "a core entry point") +
               ", so it does not provide full OpenGL 3.3 core.";
    return false;
  }

  log::info("GL %s", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
  log::info("GPU %s (%s)", reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
            reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
  log::info("GLSL %s", reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)));
#if defined(HR_DEBUG)
  gl::enableDebugOutput();
#endif

  glfwGetFramebufferSize(window_, &width_, &height_);
  rawSupported_ = glfwRawMouseMotionSupported() == GLFW_TRUE;

  glfwSetWindowUserPointer(window_, this);
  installCallbacks();
  return true;
}

void Window::installCallbacks() {
  auto self = [](GLFWwindow* w) {
    return static_cast<Window*>(glfwGetWindowUserPointer(w));
  };

  glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* w, int fw, int fh) {
    auto* me = static_cast<Window*>(glfwGetWindowUserPointer(w));
    me->width_ = fw;
    me->height_ = fh;
    if (me->onResize) me->onResize(fw, fh);
  });

  glfwSetKeyCallback(window_, [](GLFWwindow* w, int key, int, int action, int) {
    auto* me = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (action == GLFW_RELEASE) {
      me->input_.feedKey(translateKey(key), false, false);
      return;
    }
    me->input_.feedKey(translateKey(key), true, action == GLFW_REPEAT);
  });

  glfwSetCharCallback(window_, [](GLFWwindow* w, unsigned int codepoint) {
    static_cast<Window*>(glfwGetWindowUserPointer(w))->input_.feedChar(codepoint);
  });

  glfwSetMouseButtonCallback(window_, [](GLFWwindow* w, int button, int action, int) {
    auto* me = static_cast<Window*>(glfwGetWindowUserPointer(w));
    MouseButton b = button == GLFW_MOUSE_BUTTON_RIGHT    ? MouseButton::Right
                    : button == GLFW_MOUSE_BUTTON_MIDDLE ? MouseButton::Middle
                                                         : MouseButton::Left;
    me->input_.feedMouseButton(b, action == GLFW_PRESS);
  });

  glfwSetCursorPosCallback(window_, [](GLFWwindow* w, double x, double y) {
    auto* me = static_cast<Window*>(glfwGetWindowUserPointer(w));
    double dx = 0.0, dy = 0.0;
    if (me->haveLastMouse_) {
      dx = x - me->lastMouseX_;
      dy = y - me->lastMouseY_;
    }
    me->lastMouseX_ = x;
    me->lastMouseY_ = y;
    me->haveLastMouse_ = true;
    me->input_.feedMouseMove(x, y, dx, dy);
  });

  glfwSetScrollCallback(window_, [](GLFWwindow* w, double, double yoff) {
    // The browser reported wheel deltaY in pixels with the sign inverted
    // relative to GLFW's "up is positive". Negate so a scroll-up still advances
    // the hotbar the same direction as in the web build.
    static_cast<Window*>(glfwGetWindowUserPointer(w))->input_.feedWheel(-yoff);
  });

  glfwSetWindowFocusCallback(window_, [](GLFWwindow* w, int focused) {
    auto* me = static_cast<Window*>(glfwGetWindowUserPointer(w));
    if (!focused) me->input_.clearHeld();
    if (me->onFocusChange) me->onFocusChange(focused == GLFW_TRUE);
  });

  (void)self;
}

void Window::destroy() {
  if (window_) {
    glfwDestroyWindow(window_);
    window_ = nullptr;
  }
  if (g_glfwRefs > 0 && --g_glfwRefs == 0) glfwTerminate();
}

void Window::pollEvents() {
  // Deltas are derived from consecutive cursor positions, so a jump caused by
  // the cursor being re-centred on capture must not become a look delta.
  glfwPollEvents();
}

void Window::swapBuffers() { glfwSwapBuffers(window_); }
bool Window::shouldClose() const { return glfwWindowShouldClose(window_) == GLFW_TRUE; }
void Window::requestClose() { glfwSetWindowShouldClose(window_, GLFW_TRUE); }
double Window::time() const { return glfwGetTime(); }

void Window::setPointerCaptured(bool on) {
  if (!window_) return;
  glfwSetInputMode(window_, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
  if (rawSupported_) {
    glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION,
                     (on && rawWanted_) ? GLFW_TRUE : GLFW_FALSE);
  }
  // Forget the previous cursor position so the teleport GLFW performs when the
  // mode changes does not surface as one huge look delta.
  haveLastMouse_ = false;
  input_.setCaptured(on);
}

bool Window::pointerCaptured() const {
  return window_ && glfwGetInputMode(window_, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;
}

void Window::setRawMouseMotion(bool on) {
  rawWanted_ = on;
  if (window_ && rawSupported_ && pointerCaptured()) {
    glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, on ? GLFW_TRUE : GLFW_FALSE);
  }
}

bool Window::rawMouseMotionSupported() const { return rawSupported_; }

void Window::setVsync(bool on) { glfwSwapInterval(on ? 1 : 0); }

void Window::setTitle(const std::string& title) {
  if (window_) glfwSetWindowTitle(window_, title.c_str());
}

void Window::setFullscreen(bool on) {
  if (!window_ || on == fullscreen_) return;
  if (on) {
    glfwGetWindowPos(window_, &savedX_, &savedY_);
    glfwGetWindowSize(window_, &savedW_, &savedH_);
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    glfwSetWindowMonitor(window_, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
  } else {
    glfwSetWindowMonitor(window_, nullptr, savedX_, savedY_, savedW_, savedH_, GLFW_DONT_CARE);
  }
  fullscreen_ = on;
  // glfwSetWindowMonitor resets the swap interval on some drivers.
  glfwSwapInterval(1);
}

std::string Window::clipboardText() const {
  const char* text = glfwGetClipboardString(window_);
  return text ? std::string(text) : std::string();
}

void Window::setClipboardText(const std::string& text) {
  if (window_) glfwSetClipboardString(window_, text.c_str());
}

}  // namespace hr
