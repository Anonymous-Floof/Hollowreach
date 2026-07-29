# Packaging: a zip holding the single executable plus documentation.
#
# With HOLLOWREACH_EMBED_ASSETS on there is nothing else to ship — no assets
# folder, no runtime DLLs (everything links statically apart from the system GL
# driver). The game creates its own data/ folder next to the executable on first
# run, mirroring how the web build kept worlds/ beside server.py.

install(TARGETS hollowreach RUNTIME DESTINATION .)

if(NOT HOLLOWREACH_EMBED_ASSETS)
  install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets/ DESTINATION assets)
endif()

# Documentation sits at the repository root, which since the fork is also the
# CMake source directory — there is no longer a native/ folder to climb out of.
set(_repo_root ${CMAKE_CURRENT_SOURCE_DIR})
foreach(doc LICENSE README.md CHANGELOG.md)
  if(EXISTS ${_repo_root}/${doc})
    install(FILES ${_repo_root}/${doc} DESTINATION .)
  endif()
endforeach()

set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "Hollowreach")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "Hollowreach-v${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}")
set(CPACK_PACKAGE_DIRECTORY ${_repo_root}/dist)
# One top-level folder inside the zip, matching tools/release.py's layout.
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Hollowreach-v${PROJECT_VERSION}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
include(CPack)
