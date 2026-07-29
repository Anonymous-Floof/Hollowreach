#include "core/gl.h"

#include "core/log.h"

#define GL_DEFINE(ret, name, args) PFN_gl##name gl##name = nullptr;
GL_FUNCS(GL_DEFINE)
#undef GL_DEFINE

PFN_glDebugMessageCallback glDebugMessageCallback = nullptr;

namespace hr::gl {

bool load(void* (*loader)(const char*), const char** missingOut) {
  const char* missing = nullptr;

// Load in declaration order and remember the first failure rather than bailing
// immediately, so a driver that is short of several symbols still reports the
// earliest useful one after the whole table has been attempted.
#define GL_LOAD(ret, name, args)                                            \
  gl##name = reinterpret_cast<PFN_gl##name>(loader("gl" #name));            \
  if (!gl##name && !missing) missing = "gl" #name;
  GL_FUNCS(GL_LOAD)
#undef GL_LOAD

  // Optional, so its absence is not a failure.
  glDebugMessageCallback =
      reinterpret_cast<PFN_glDebugMessageCallback>(loader("glDebugMessageCallback"));

  if (missing) {
    if (missingOut) *missingOut = missing;
    return false;
  }
  return true;
}

namespace {

void GLAPI_CALL debugCallback(GLenum /*source*/, GLenum /*type*/, GLuint /*id*/, GLenum severity,
                              GLsizei /*length*/, const GLchar* message, const void* /*user*/) {
  // Notification-level messages are mostly buffer-allocation chatter from the
  // chunk streamer, which would drown everything else.
  if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
  const char* level = severity == GL_DEBUG_SEVERITY_HIGH     ? "high"
                      : severity == GL_DEBUG_SEVERITY_MEDIUM ? "medium"
                                                             : "low";
  log::warn("GL[%s]: %s", level, message);
}

}  // namespace

void enableDebugOutput() {
  if (!glDebugMessageCallback) return;
  glEnable(GL_DEBUG_OUTPUT);
  glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  glDebugMessageCallback(&debugCallback, nullptr);
  log::info("GL debug output enabled");
}

void checkError(const char* tag) {
  GLenum e;
  while ((e = glGetError()) != GL_NO_ERROR) {
    log::error("GL error 0x%04X at %s", e, tag);
  }
}

}  // namespace hr::gl
