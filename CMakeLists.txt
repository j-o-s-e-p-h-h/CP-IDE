cmake_minimum_required(VERSION 3.21)
if(WIN32)
  project(cp_ide VERSION 0.1.0 LANGUAGES CXX RC)
else()
  project(cp_ide VERSION 0.1.0 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Windows: WebView2 (Edge). Linux: WebKitGTK (libwebkit2gtk-4.1-dev or webkitgtk-6.0).
# macOS: WKWebView. The webview library's own CMake picks the backend.

# ---- vendored dependencies ---------------------------------------------------
# WebView2 SDK headers (vendored so `git clone && cmake` needs no network).
set(MSWebView2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2")
set(WEBVIEW_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_STATIC_LIBRARY OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_CHECKS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_PACKAGING OFF CACHE BOOL "" FORCE)
set(WEBVIEW_USE_COMPAT_MINGW ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/webview EXCLUDE_FROM_ALL)

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# ---- application ------------------------------------------------------------
set(CP_IDE_SOURCES
  src/main.cpp
  src/app.cpp
  src/http_server.cpp
  src/http_client.cpp
  src/storage.cpp
  src/runner.cpp
  src/tools_detect.cpp
  src/tools_detect_posix.cpp
  src/companion.cpp
  src/statement.cpp
  src/cf_api.cpp
  src/stress.cpp
  src/debugger.cpp
  src/judges/judge.cpp
  src/judges/codeforces.cpp
  src/judges/judge_web.cpp
  src/judges/drivers.cpp
)

if(WIN32)
  list(APPEND CP_IDE_SOURCES src/res/app.rc)
  # Recompile the resource when the icon file itself changes.
  set_source_files_properties(src/res/app.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/res/cp-ide.ico")
  add_executable(cp-ide WIN32 ${CP_IDE_SOURCES})
  target_link_libraries(cp-ide PRIVATE webview::core nlohmann_json winhttp ws2_32 ole32 shell32 shlwapi user32 advapi32 version comctl32)
  target_compile_definitions(cp-ide PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  if(MINGW)
    # Standalone exe: no libstdc++/libgcc/winpthread DLL dependency on msys64.
    target_link_options(cp-ide PRIVATE -static -static-libgcc -static-libstdc++ -municode -s)  # -s strips symbols: ~3 MB instead of ~5 MB
  endif()
elseif(APPLE)
  add_executable(cp-ide MACOSX_BUNDLE ${CP_IDE_SOURCES})
  target_link_libraries(cp-ide PRIVATE webview::core nlohmann_json "-framework Cocoa" "-framework WebKit" objc)
  set_target_properties(cp-ide PROPERTIES MACOSX_BUNDLE_BUNDLE_NAME "CP IDE" MACOSX_BUNDLE_GUI_IDENTIFIER "dev.cpide.app"
                        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}" MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
else()
  add_executable(cp-ide ${CP_IDE_SOURCES})
  find_package(Threads REQUIRED)
  target_link_libraries(cp-ide PRIVATE webview::core nlohmann_json Threads::Threads)
endif()
target_include_directories(cp-ide PRIVATE src)
target_compile_options(cp-ide PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type)
set_target_properties(cp-ide PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---- runtime assets next to the exe -----------------------------------------
# Runs on every build so edits in ui/ and tools/ are picked up without relinking.
add_custom_target(cp_ide_assets ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_CURRENT_SOURCE_DIR}/ui" "${CMAKE_BINARY_DIR}/bin/ui"
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/monaco/vs" "${CMAKE_BINARY_DIR}/bin/ui/vs"
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/katex" "${CMAKE_BINARY_DIR}/bin/ui/katex"
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${CMAKE_CURRENT_SOURCE_DIR}/tools" "${CMAKE_BINARY_DIR}/bin/tools"
  COMMENT "Copying UI, Monaco and tools next to the executable")
if(WIN32)
  add_custom_command(TARGET cp_ide_assets POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/x64/WebView2Loader.dll" "${CMAKE_BINARY_DIR}/bin/WebView2Loader.dll")
endif()
add_dependencies(cp-ide cp_ide_assets)

# A .app finds its files at Contents/Resources (findUiDir's "../Resources/ui"), so the
# assets have to be copied inside the bundle as well as next to it — otherwise the
# bundle runs but shows a blank window.
if(APPLE)
  set(CP_IDE_RES "$<TARGET_BUNDLE_CONTENT_DIR:cp-ide>/Resources")
  add_custom_command(TARGET cp-ide POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/bin/ui" "${CP_IDE_RES}/ui"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_BINARY_DIR}/bin/tools" "${CP_IDE_RES}/tools"
    COMMENT "Copying UI and tools into the .app bundle")
endif()

# ---- install (Linux: `cmake --install build`; macOS: the .app bundle) ---------
if(UNIX AND NOT APPLE)
  include(GNUInstallDirs)
  install(TARGETS cp-ide RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/ui" "${CMAKE_BINARY_DIR}/bin/tools" DESTINATION ${CMAKE_INSTALL_DATADIR}/cp-ide)
  install(FILES installer/cp-ide.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES ui/assets/icon-256.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps RENAME cp-ide.png)
endif()
if(APPLE)
  install(TARGETS cp-ide BUNDLE DESTINATION .)
endif()

# ---- packages (CPack): `cpack` in the build dir after a build ----------------
# Linux  -> cp-ide_<version>_amd64.deb and a .tar.gz
# macOS  -> CP-IDE-<version>-Darwin.dmg  (drag to Applications)
# Windows keeps the Inno Setup target below instead: it handles the WebView2
# runtime and the shortcuts, which a generic generator would not.
if(NOT WIN32)
  set(CPACK_PACKAGE_NAME "cp-ide")
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_PACKAGE_VENDOR "CP IDE contributors")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Desktop IDE for competitive programming")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "cp-ide")
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
  set(CPACK_STRIP_FILES TRUE)
  if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "CP IDE ${PROJECT_VERSION}")
    # Unsigned and un-notarized: Gatekeeper blocks it until the user right-clicks
    # and picks Open. Signing needs a paid Apple Developer account.
  else()
    set(CPACK_GENERATOR "DEB;TGZ")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "CP IDE contributors")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/j-o-s-e-p-h-h/Competitive-programming-IDE-project")
    # WebKitGTK and GTK are linked, not vendored, so the package must ask for them.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libwebkit2gtk-4.1-0, libgtk-3-0")
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "g++, python3, nodejs")
  endif()
  include(CPack)
endif()

# ---- installer (Inno Setup 6) ----------------------------------------------
# `cmake --build build --target installer` -> installer/out/CP-IDE-Setup-<version>.exe
if(WIN32)
  find_program(ISCC_EXE NAMES ISCC iscc PATHS "$ENV{ProgramFiles\(x86\)}/Inno Setup 6" "$ENV{ProgramFiles}/Inno Setup 6" "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6")
endif()
if(ISCC_EXE)
  add_custom_target(installer
    COMMAND "${ISCC_EXE}" /Q "/DAppVersion=${PROJECT_VERSION}" "${CMAKE_CURRENT_SOURCE_DIR}/installer/cp-ide.iss"
    DEPENDS cp-ide
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/installer"
    COMMENT "Building the Windows installer with Inno Setup")
elseif(WIN32)
  message(STATUS "Inno Setup (ISCC.exe) not found: the `installer` target is unavailable. winget install JRSoftware.InnoSetup")
endif()
