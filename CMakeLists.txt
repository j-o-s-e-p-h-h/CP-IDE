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

# ---- cp-ide-wx: native front end on the same core ---------------------------
# A second executable, not a replacement: it links the same storage / runner /
# judge / statement sources and only swaps the UI for wxWidgets + Scintilla.
# Built when wxWidgets is present, skipped otherwise, so the normal build of
# cp-ide never depends on it.
#   Windows (MSYS2): pacman -S mingw-w64-ucrt-x86_64-wxwidgets3.2-msw
#   Debian/Ubuntu:   sudo apt install libwxgtk3.2-dev
#   macOS:           brew install wxwidgets
set(CP_IDE_CORE_SOURCES
  src/storage.cpp
  src/runner.cpp
  src/tools_detect.cpp
  src/tools_detect_posix.cpp
  src/companion.cpp
  src/statement.cpp
  src/cf_api.cpp
  src/http_client.cpp
)
# CMake's FindwxWidgets uses an MSVC-style search on Windows and cannot read an
# MSYS2 install, so ask wx-config instead — every platform's wx ships it.
find_program(WX_CONFIG NAMES wx-config-3.2 wx-config)
set(WX_OK FALSE)
if(WX_CONFIG)
  if(WIN32)
    find_program(SH_EXE NAMES sh HINTS "C:/msys64/usr/bin" "$ENV{MSYSTEM_PREFIX}/../usr/bin")
    set(WX_CMD ${SH_EXE} ${WX_CONFIG})   # wx-config is a shell script
  else()
    set(WX_CMD ${WX_CONFIG})
  endif()
  execute_process(COMMAND ${WX_CMD} --cxxflags RESULT_VARIABLE wx_rc
                  OUTPUT_VARIABLE WX_CXXFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND ${WX_CMD} --libs std,stc
                  OUTPUT_VARIABLE WX_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(wx_rc EQUAL 0 AND WX_CXXFLAGS)
    separate_arguments(WX_CXXFLAGS NATIVE_COMMAND "${WX_CXXFLAGS}")
    separate_arguments(WX_LIBS NATIVE_COMMAND "${WX_LIBS}")
    set(WX_OK TRUE)
  endif()
endif()
if(WX_OK)
  add_executable(cp-ide-wx WIN32 MACOSX_BUNDLE src/wx/wx_main.cpp ${CP_IDE_CORE_SOURCES})
  target_include_directories(cp-ide-wx PRIVATE src "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview/core/include")
  if(WIN32)
    target_include_directories(cp-ide-wx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/include")
  endif()
  target_compile_options(cp-ide-wx PRIVATE ${WX_CXXFLAGS})
  target_link_libraries(cp-ide-wx PRIVATE ${WX_LIBS} nlohmann_json)
  target_compile_definitions(cp-ide-wx PRIVATE NOMINMAX)
  if(WIN32)
    target_compile_definitions(cp-ide-wx PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00)
    target_link_libraries(cp-ide-wx PRIVATE winhttp ws2_32 ole32 shell32 shlwapi version dwmapi)
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(cp-ide-wx PRIVATE Threads::Threads)
  endif()
  set_target_properties(cp-ide-wx PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  message(STATUS "wxWidgets found: building cp-ide-wx alongside cp-ide")
else()
  message(STATUS "wxWidgets not found: skipping the cp-ide-wx target (cp-ide is unaffected)")
endif()

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

# ---- install (Linux: `cmake --install build`; macOS: the .app bundle) ---------
if(UNIX AND NOT APPLE)
  include(GNUInstallDirs)
  install(TARGETS cp-ide RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/ui" "${CMAKE_BINARY_DIR}/bin/tools" DESTINATION ${CMAKE_INSTALL_DATADIR}/cp-ide)
  install(FILES installer/cp-ide.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES ui/assets/icon-256.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps RENAME cp-ide.png)
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
