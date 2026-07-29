# Third-party wiring.
#
# Header-only libraries are vendored under third_party/ so a clean build needs
# no network. GLFW and ENet are fetched, with HOLLOWREACH_DEPS_DIR available to
# point at local clones for offline builds.

include(FetchContent)

set(HOLLOWREACH_DEPS_DIR "" CACHE PATH
    "Directory holding local glfw/ and enet/ checkouts (skips downloading)")

if(HOLLOWREACH_DEPS_DIR)
  set(FETCHCONTENT_SOURCE_DIR_GLFW ${HOLLOWREACH_DEPS_DIR}/glfw CACHE PATH "" FORCE)
  set(FETCHCONTENT_SOURCE_DIR_ENET ${HOLLOWREACH_DEPS_DIR}/enet CACHE PATH "" FORCE)
endif()

# --- GLFW ------------------------------------------------------------------
# Window, GL context, input, and pointer lock (GLFW_CURSOR_DISABLED is the
# native equivalent of the browser's Pointer Lock API).
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(glfw)

# --- ENet ------------------------------------------------------------------
# Reliable-ordered + unreliable channels over UDP, replacing the two WebRTC
# data channels ("rel" and "fast") of the web build.
#
# Built from sources here rather than via ENet's own CMakeLists, which declares
# cmake_minimum_required(VERSION 2.8) — deprecated in CMake 3.31 and removed in
# 4.0. SOURCE_SUBDIR points at a directory without a CMakeLists.txt so
# MakeAvailable populates without configuring.
if(HOLLOWREACH_WITH_NET)
  FetchContent_Declare(enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG v1.3.18
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
    SOURCE_SUBDIR include
  )
  FetchContent_MakeAvailable(enet)

  add_library(enet STATIC
    ${enet_SOURCE_DIR}/callbacks.c
    ${enet_SOURCE_DIR}/compress.c
    ${enet_SOURCE_DIR}/host.c
    ${enet_SOURCE_DIR}/list.c
    ${enet_SOURCE_DIR}/packet.c
    ${enet_SOURCE_DIR}/peer.c
    ${enet_SOURCE_DIR}/protocol.c
    ${enet_SOURCE_DIR}/unix.c
    ${enet_SOURCE_DIR}/win32.c
  )
  target_include_directories(enet PUBLIC ${enet_SOURCE_DIR}/include)
  if(WIN32)
    target_link_libraries(enet PUBLIC ws2_32 winmm)
    target_compile_definitions(enet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS)
  endif()
  if(MSVC)
    target_compile_options(enet PRIVATE /W0)
  else()
    target_compile_options(enet PRIVATE -w)
  endif()
endif()

# --- aggregate -------------------------------------------------------------
add_library(hollowreach_deps INTERFACE)
add_library(hollowreach::deps ALIAS hollowreach_deps)

target_link_libraries(hollowreach_deps INTERFACE glfw)
if(HOLLOWREACH_WITH_NET)
  target_link_libraries(hollowreach_deps INTERFACE enet)
endif()

find_package(Threads REQUIRED)
target_link_libraries(hollowreach_deps INTERFACE Threads::Threads)

if(WIN32)
  # GL entry points are resolved through glfwGetProcAddress, but the loader
  # still needs opengl32 present for the GL 1.x symbols it falls back to.
  target_link_libraries(hollowreach_deps INTERFACE opengl32)
elseif(APPLE)
  target_link_libraries(hollowreach_deps INTERFACE "-framework OpenGL")
else()
  find_package(OpenGL REQUIRED)
  target_link_libraries(hollowreach_deps INTERFACE OpenGL::GL dl)
endif()
