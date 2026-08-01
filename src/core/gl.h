// Self-contained OpenGL 3.3 core loader.
//
// The web build targets WebGL2, which is GL ES 3.0 — GL 3.3 core is the desktop
// feature level that matches it one-for-one, so every shader and every call in
// js/render/ has a direct equivalent here. We declare only what the renderer
// actually uses rather than pulling in a generated loader: it keeps the surface
// visible, avoids a build-time codegen step, and makes it obvious when a port
// step reaches for something outside the target feature level.
//
// No system GL header is included on purpose. <GL/gl.h> on Windows drags in
// windows.h and only declares GL 1.1, so mixing it with our own declarations
// invites conflicts.

#pragma once

#include <cstddef>
#include <cstdint>

// --- types ------------------------------------------------------------------
using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLbyte = signed char;
using GLshort = short;
using GLint = int;
using GLsizei = int;
using GLubyte = unsigned char;
using GLushort = unsigned short;
using GLuint = unsigned int;
using GLfloat = float;
using GLclampf = float;
using GLdouble = double;
using GLchar = char;
using GLintptr = std::intptr_t;
using GLsizeiptr = std::ptrdiff_t;
using GLuint64 = std::uint64_t;
using GLvoid = void;

// --- constants (only what we use) ------------------------------------------
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_NONE 0
#define GL_ZERO 0
#define GL_ONE 1

#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_LINE_STRIP 0x0003
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005

#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_STENCIL_BUFFER_BIT 0x00000400
#define GL_COLOR_BUFFER_BIT 0x00004000

#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_ALWAYS 0x0207

#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307

#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408

#define GL_CW 0x0900
#define GL_CCW 0x0901

#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_POLYGON_OFFSET_FILL 0x8037
#define GL_MULTISAMPLE 0x809D
#define GL_FRAMEBUFFER_SRGB 0x8DB9

#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05

#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_VIEWPORT_DIMS 0x0D3A
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define GL_MAX_SAMPLES 0x8D57
#define GL_MAX_COLOR_ATTACHMENTS 0x8CDF

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_REPEAT 0x2901

#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_HALF_FLOAT 0x140B

#define GL_RED 0x1903
#define GL_RG 0x8227
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_R8 0x8229
#define GL_RG8 0x822B
#define GL_RGB8 0x8051
#define GL_RGBA8 0x8058
#define GL_R16F 0x822D
#define GL_RGBA16F 0x881A
#define GL_DEPTH_COMPONENT 0x1902
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_COMPONENT32F 0x8CAC

#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STREAM_DRAW 0x88E0
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8

#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_ACTIVE_UNIFORM_MAX_LENGTH 0x8B87

// Texture units are guaranteed consecutive by the spec, but spelling out the ones
// the renderer binds keeps the call sites readable.
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE2 0x84C2
#define GL_TEXTURE3 0x84C3
#define GL_TEXTURE4 0x84C4
#define GL_TEXTURE5 0x84C5
#define GL_TEXTURE6 0x84C6
#define GL_TEXTURE7 0x84C7

#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5

// Timer queries. GL_TIME_ELAPSED brackets a pass and reports nanoseconds of GPU
// time; the result is read back several frames later so asking for it never
// stalls the pipeline the way glFinish or an immediate GL_QUERY_RESULT would.
#define GL_TIME_ELAPSED 0x88BF
#define GL_QUERY_RESULT 0x8866
#define GL_QUERY_RESULT_AVAILABLE 0x8867

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C

// KHR_debug (4.3; optional, used only when present)
#define GL_DEBUG_OUTPUT 0x92E0
#define GL_DEBUG_OUTPUT_SYNCHRONOUS 0x8242
#define GL_DEBUG_SEVERITY_HIGH 0x9146
#define GL_DEBUG_SEVERITY_MEDIUM 0x9147
#define GL_DEBUG_SEVERITY_LOW 0x9148
#define GL_DEBUG_SEVERITY_NOTIFICATION 0x826B

using GLDEBUGPROC = void(
#ifdef _WIN32
    __stdcall
#endif
    *)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
       const GLchar* message, const void* userParam);

// --- the function table -----------------------------------------------------
// One entry per call the renderer makes. GL_FUNCS(X) drives both the pointer
// declarations and the loader loop, so adding a call means adding one line.
#ifdef _WIN32
#define GLAPI_CALL __stdcall
#else
#define GLAPI_CALL
#endif

#define GL_FUNCS(X)                                                                              \
  /* state */                                                                                    \
  X(void, Enable, (GLenum cap))                                                                  \
  X(void, Disable, (GLenum cap))                                                                 \
  X(void, Clear, (GLbitfield mask))                                                              \
  X(void, ClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                               \
  X(void, ClearDepth, (GLdouble d))                                                              \
  X(void, Viewport, (GLint x, GLint y, GLsizei w, GLsizei h))                                     \
  X(void, Scissor, (GLint x, GLint y, GLsizei w, GLsizei h))                                      \
  X(void, DepthMask, (GLboolean on))                                                              \
  X(void, DepthFunc, (GLenum func))                                                               \
  X(void, ColorMask, (GLboolean r, GLboolean g, GLboolean b, GLboolean a))                        \
  X(void, BlendFunc, (GLenum src, GLenum dst))                                                    \
  X(void, BlendFuncSeparate, (GLenum sc, GLenum dc, GLenum sa, GLenum da))                        \
  X(void, CullFace, (GLenum mode))                                                                \
  X(void, FrontFace, (GLenum mode))                                                               \
  X(void, PolygonOffset, (GLfloat factor, GLfloat units))                                          \
  X(void, LineWidth, (GLfloat w))                                                                  \
  X(void, PixelStorei, (GLenum name, GLint param))                                                 \
  X(void, Finish, ())                                                                              \
  X(void, Flush, ())                                                                               \
  X(GLenum, GetError, ())                                                                          \
  X(void, GetIntegerv, (GLenum name, GLint* data))                                                  \
  X(const GLubyte*, GetString, (GLenum name))                                                       \
  X(void, DrawArrays, (GLenum mode, GLint first, GLsizei count))                                     \
  X(void, DrawElements, (GLenum mode, GLsizei count, GLenum type, const void* indices))             \
  X(void, ReadPixels,                                                                              \
    (GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void* pixels))               \
  /* buffers + vertex arrays */                                                                    \
  X(void, GenBuffers, (GLsizei n, GLuint * out))                                                    \
  X(void, DeleteBuffers, (GLsizei n, const GLuint* buf))                                            \
  X(void, BindBuffer, (GLenum target, GLuint buf))                                                  \
  X(void, BufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))             \
  X(void, BufferSubData, (GLenum target, GLintptr off, GLsizeiptr size, const void* data))          \
  X(void, GenVertexArrays, (GLsizei n, GLuint * out))                                               \
  X(void, DeleteVertexArrays, (GLsizei n, const GLuint* arr))                                       \
  X(void, BindVertexArray, (GLuint vao))                                                            \
  X(void, EnableVertexAttribArray, (GLuint index))                                                  \
  X(void, DisableVertexAttribArray, (GLuint index))                                                 \
  X(void, VertexAttribPointer,                                                                     \
    (GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* off))          \
  X(void, VertexAttribIPointer,                                                                    \
    (GLuint i, GLint size, GLenum type, GLsizei stride, const void* off))                           \
  /* shaders + programs */                                                                          \
  X(GLuint, CreateShader, (GLenum type))                                                             \
  X(void, ShaderSource, (GLuint sh, GLsizei n, const GLchar* const* src, const GLint* len))          \
  X(void, CompileShader, (GLuint sh))                                                                \
  X(void, GetShaderiv, (GLuint sh, GLenum name, GLint* out))                                          \
  X(void, GetShaderInfoLog, (GLuint sh, GLsizei max, GLsizei* len, GLchar* log))                      \
  X(void, DeleteShader, (GLuint sh))                                                                  \
  X(GLuint, CreateProgram, ())                                                                        \
  X(void, AttachShader, (GLuint prog, GLuint sh))                                                      \
  X(void, LinkProgram, (GLuint prog))                                                                  \
  X(void, GetProgramiv, (GLuint prog, GLenum name, GLint* out))                                        \
  X(void, GetProgramInfoLog, (GLuint prog, GLsizei max, GLsizei* len, GLchar* log))                    \
  X(void, UseProgram, (GLuint prog))                                                                   \
  X(void, DeleteProgram, (GLuint prog))                                                                \
  X(void, BindAttribLocation, (GLuint prog, GLuint index, const GLchar* name))                         \
  X(GLint, GetUniformLocation, (GLuint prog, const GLchar* name))                                       \
  X(void, GetActiveUniform,                                                                            \
    (GLuint p, GLuint i, GLsizei max, GLsizei* len, GLint* size, GLenum* type, GLchar* name))          \
  X(void, Uniform1i, (GLint loc, GLint v))                                                              \
  X(void, Uniform1f, (GLint loc, GLfloat v))                                                             \
  X(void, Uniform2f, (GLint loc, GLfloat a, GLfloat b))                                                  \
  X(void, Uniform3f, (GLint loc, GLfloat a, GLfloat b, GLfloat c))                                        \
  X(void, Uniform4f, (GLint loc, GLfloat a, GLfloat b, GLfloat c, GLfloat d))                             \
  X(void, Uniform1iv, (GLint loc, GLsizei n, const GLint* v))                                             \
  X(void, Uniform1fv, (GLint loc, GLsizei n, const GLfloat* v))                                            \
  X(void, Uniform2fv, (GLint loc, GLsizei n, const GLfloat* v))                                            \
  X(void, Uniform3fv, (GLint loc, GLsizei n, const GLfloat* v))                                            \
  X(void, Uniform4fv, (GLint loc, GLsizei n, const GLfloat* v))                                            \
  X(void, UniformMatrix4fv, (GLint loc, GLsizei n, GLboolean transpose, const GLfloat* v))                 \
  /* textures */                                                                                          \
  X(void, GenTextures, (GLsizei n, GLuint * out))                                                           \
  X(void, DeleteTextures, (GLsizei n, const GLuint* tex))                                                   \
  X(void, BindTexture, (GLenum target, GLuint tex))                                                         \
  X(void, ActiveTexture, (GLenum unit))                                                                     \
  X(void, TexImage2D,                                                                                      \
    (GLenum t, GLint lvl, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type,          \
     const void* px))                                                                                       \
  X(void, TexSubImage2D,                                                                                   \
    (GLenum t, GLint lvl, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type,                  \
     const void* px))                                                                                       \
  X(void, CopyTexSubImage2D,                                                                                \
    (GLenum t, GLint lvl, GLint x, GLint y, GLint srcX, GLint srcY, GLsizei w, GLsizei h))                   \
  X(void, TexParameteri, (GLenum target, GLenum name, GLint param))                                          \
  X(void, TexParameterf, (GLenum target, GLenum name, GLfloat param))                                        \
  X(void, GenerateMipmap, (GLenum target))                                                                   \
  /* framebuffers */                                                                                        \
  X(void, GenFramebuffers, (GLsizei n, GLuint * out))                                                        \
  X(void, DeleteFramebuffers, (GLsizei n, const GLuint* fb))                                                 \
  X(void, BindFramebuffer, (GLenum target, GLuint fb))                                                       \
  X(void, FramebufferTexture2D,                                                                             \
    (GLenum target, GLenum attach, GLenum texTarget, GLuint tex, GLint level))                               \
  X(void, FramebufferRenderbuffer, (GLenum target, GLenum attach, GLenum rbTarget, GLuint rb))               \
  X(GLenum, CheckFramebufferStatus, (GLenum target))                                                          \
  X(void, GenRenderbuffers, (GLsizei n, GLuint * out))                                                        \
  X(void, DeleteRenderbuffers, (GLsizei n, const GLuint* rb))                                                 \
  X(void, BindRenderbuffer, (GLenum target, GLuint rb))                                                       \
  X(void, RenderbufferStorage, (GLenum target, GLenum ifmt, GLsizei w, GLsizei h))                            \
  X(void, DrawBuffers, (GLsizei n, const GLenum* bufs))                                                       \
  X(void, ReadBuffer, (GLenum src))                                                                           \
  X(void, BlitFramebuffer,                                                                                   \
    (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1,                  \
     GLbitfield mask, GLenum filter))                                                                        \
  /* timer queries (core since 3.3) — see render/gputimer.h */                                               \
  X(void, GenQueries, (GLsizei n, GLuint * out))                                                              \
  X(void, DeleteQueries, (GLsizei n, const GLuint* q))                                                        \
  X(void, BeginQuery, (GLenum target, GLuint q))                                                              \
  X(void, EndQuery, (GLenum target))                                                                          \
  X(void, GetQueryObjectuiv, (GLuint q, GLenum name, GLuint * out))                                           \
  X(void, GetQueryObjectui64v, (GLuint q, GLenum name, GLuint64 * out))

#define GL_DECL(ret, name, args) using PFN_gl##name = ret(GLAPI_CALL*) args;
GL_FUNCS(GL_DECL)
#undef GL_DECL

// The entry points live at global scope under their usual gl* spelling, so a
// ported call site reads `glDrawArrays(...)` exactly where the JS read
// `gl.drawArrays(...)`.
#define GL_EXTERN(ret, name, args) extern PFN_gl##name gl##name;
GL_FUNCS(GL_EXTERN)
#undef GL_EXTERN

// KHR_debug (4.3). Non-null only when the driver exposes it.
using PFN_glDebugMessageCallback = void(GLAPI_CALL*)(GLDEBUGPROC cb, const void* user);
extern PFN_glDebugMessageCallback glDebugMessageCallback;

namespace hr::gl {

// Resolves every entry point through `loader` (glfwGetProcAddress). Returns
// false and reports the first missing symbol if the driver is short of GL 3.3.
bool load(void* (*loader)(const char*), const char** missingOut);

// Installs a debug callback when KHR_debug is present; no-op otherwise.
void enableDebugOutput();

// Logs any pending GL error with the given tag. Debug builds only: the ported
// renderer issues thousands of calls per frame and glGetError forces a sync.
void checkError(const char* tag);

}  // namespace hr::gl

#if defined(HR_DEBUG)
#define HR_GL_CHECK(tag) ::hr::gl::checkError(tag)
#else
#define HR_GL_CHECK(tag) ((void)0)
#endif
