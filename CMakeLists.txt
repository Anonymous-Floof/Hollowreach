cmake_minimum_required(VERSION 3.21)

# Hollowreach — a voxel sandbox, ported from the WebGL2 build this repository
# was forked from (github.com/Anonymous-Floof/Hollow-Reach).
#
# Targets OpenGL 3.3 core, which is the desktop feature level matching WebGL2
# one-for-one, so the ported shaders and render passes stay a direct translation.
project(Hollowreach LANGUAGES C CXX VERSION 2.10.1)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# Static CRT on MSVC. The shipped artifact is one executable that a player
# unzips and double-clicks, and the default /MD links against VCRUNTIME140.dll
# and MSVCP140.dll — which are part of the Visual C++ Redistributable, not of
# Windows. Someone who has never installed Visual Studio does not have them, and
# the game does not start: a missing-DLL dialog, before a single line of ours
# runs. Set before the dependencies are declared so GLFW and ENet inherit it;
# mixing runtimes in one binary is its own class of bug.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# One place for the executable regardless of generator, so build.bat and the
# packaging step can both find it without guessing at config subdirectories.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} ${CMAKE_BINARY_DIR}/bin)
endforeach()

option(HOLLOWREACH_EMBED_ASSETS "Bake shaders and fonts into the executable" ON)
option(HOLLOWREACH_WITH_NET "Build multiplayer (fetches ENet)" ON)

include(cmake/deps.cmake)
include(cmake/embed_assets.cmake)

# ---------------------------------------------------------------------------
# Sources
#
# Globbed with CONFIGURE_DEPENDS: this port adds files constantly and an
# explicit list would be pure churn. CMake re-globs whenever the build runs.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE HOLLOWREACH_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
)
file(GLOB_RECURSE HOLLOWREACH_HEADERS CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
)

hollowreach_embed_assets(HOLLOWREACH_EMBEDDED_SOURCES)

add_executable(hollowreach
  ${HOLLOWREACH_SOURCES}
  ${HOLLOWREACH_HEADERS}
  ${HOLLOWREACH_EMBEDDED_SOURCES}
)
set_target_properties(hollowreach PROPERTIES OUTPUT_NAME "Hollowreach")

target_include_directories(hollowreach PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

target_link_libraries(hollowreach PRIVATE hollowreach::deps)

target_compile_definitions(hollowreach PRIVATE
  HR_VERSION="${PROJECT_VERSION}"
  $<$<CONFIG:Debug>:HR_DEBUG>
  $<$<BOOL:${HOLLOWREACH_EMBED_ASSETS}>:HR_EMBED_ASSETS>
  $<$<BOOL:${HOLLOWREACH_WITH_NET}>:HR_WITH_NET>
  # Points a local build at the on-disk assets so shader edits need no rebuild.
  # NOT in Release, which is what ships: this is an absolute path from the machine
  # that compiled it, so it would put the builder's username and directory layout
  # into a public binary for no benefit — a player's copy can never find that path
  # anyway and falls back to the embedded assets. An assets/ folder beside the
  # executable still overrides, in every configuration, which is the hook a player
  # or a resource pack would actually use.
  $<$<NOT:$<CONFIG:Release>>:HR_SOURCE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets">
  # Compiling a release from a different directory must not change the binary.
  # /Zi has already been dropped by the Release flags, taking the .pdb path with
  # it; this covers the __FILE__ that any assert or log line could expand to.
  $<$<CONFIG:Release>:HR_RELEASE_BUILD>
  # We declare the GL 3.3 entry points ourselves in core/gl.h, so GLFW must not
  # drag in the platform <GL/gl.h> (which only has GL 1.1 and collides).
  GLFW_INCLUDE_NONE
  # miniaudio is a device abstraction here and nothing more — every generator,
  # filter and mixer is ours, in src/audio. These must be identical in every TU
  # that includes the header, so they live on the target rather than in a source
  # file. See src/audio/miniaudio_impl.c.
  MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION
  MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE
)

# miniaudio is 96k lines of someone else's C and warns copiously under /W4, and
# stb_vorbis is another 5.5k with the same habit.
set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/miniaudio_impl.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/stb_vorbis_impl.c
  PROPERTIES COMPILE_OPTIONS "$<IF:$<BOOL:${MSVC}>,/W0,-w>")

if(MSVC)
  target_compile_options(hollowreach PRIVATE
    /W4 /permissive- /utf-8 /EHsc /MP
    /wd4244 /wd4267 # narrowing conversions: pervasive and intentional in the ported math
    # Determinism: multiplayer guests generate terrain locally from the seed, so
    # two machines must agree. /fp:fast would reassociate the fbm accumulation
    # and change the terrain. MSVC has no command-line switch to disable FMA
    # contraction, so the noise and worldgen units also carry
    # `#pragma fp_contract(off)` — see src/world/noise.cpp.
    /fp:precise
  )
  target_compile_definitions(hollowreach PRIVATE
    _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
  )
  # GetAdaptersAddresses, for enumerating the machine's own networks. Discovery
  # broadcasts to each interface's own subnet rather than trusting the routing
  # table to pick one — see src/net/interfaces.h.
  target_link_libraries(hollowreach PRIVATE iphlpapi)
  # Console in Debug (log output), windowed in Release.
  set_target_properties(hollowreach PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
  )
else()
  target_compile_options(hollowreach PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -ffp-contract=off
  )
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src FILES
  ${HOLLOWREACH_SOURCES} ${HOLLOWREACH_HEADERS})

include(cmake/package.cmake)

message(STATUS "Hollowreach ${PROJECT_VERSION} — ${CMAKE_BUILD_TYPE}, assets "
               "${HOLLOWREACH_EMBED_ASSETS}, net ${HOLLOWREACH_WITH_NET}")
