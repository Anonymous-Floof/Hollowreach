# Bakes everything under assets/ into a generated translation unit so the
# shipped artifact is a single executable.
#
# Generation happens at build time with DEPENDS on every asset file, so editing
# a shader rebuilds only the generated unit. In Debug the runtime prefers the
# on-disk copy under HR_SOURCE_ASSET_DIR anyway, which is why shader edits there
# need no rebuild at all.

set(HOLLOWREACH_ASSET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets)
set(HOLLOWREACH_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)

function(hollowreach_embed_assets OUT_VAR)
  file(MAKE_DIRECTORY ${HOLLOWREACH_GENERATED_DIR})
  set(generated ${HOLLOWREACH_GENERATED_DIR}/embedded_assets.cpp)

  file(GLOB_RECURSE asset_files CONFIGURE_DEPENDS
    ${HOLLOWREACH_ASSET_DIR}/*
  )
  # Directories and editor leftovers would only bloat the binary.
  list(FILTER asset_files EXCLUDE REGEX "/\\.")

  if(HOLLOWREACH_EMBED_ASSETS)
    add_custom_command(
      OUTPUT ${generated}
      COMMAND ${CMAKE_COMMAND}
              -DASSET_DIR=${HOLLOWREACH_ASSET_DIR}
              -DOUTPUT=${generated}
              -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_assets.cmake
      DEPENDS ${asset_files} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_assets.cmake
      COMMENT "Embedding ${HOLLOWREACH_ASSET_DIR} -> embedded_assets.cpp"
      VERBATIM
    )
  else()
    # Empty table: the runtime falls back to reading assets from disk.
    file(WRITE ${generated}
"// Generated: asset embedding disabled (HOLLOWREACH_EMBED_ASSETS=OFF).\n"
"#include \"core/assets.h\"\n"
"namespace hr::assets {\n"
"const Entry kEmbedded[] = { { nullptr, nullptr, 0 } };\n"
"const unsigned kEmbeddedCount = 0;\n"
"}  // namespace hr::assets\n")
  endif()

  set(${OUT_VAR} ${generated} PARENT_SCOPE)
endfunction()
