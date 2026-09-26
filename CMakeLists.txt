cmake_minimum_required(VERSION 3.16)
project(TetherDesk VERSION 1.3.0 LANGUAGES C CXX)
if(APPLE)
  # Must happen before any subproject (static SDL) is added.
  enable_language(OBJC OBJCXX)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Source lists are explicit on purpose: globbing picks up macOS "._*"
# AppleDouble files on exFAT/FAT volumes.
set(COMMON_SOURCES
  common/rd_bytes.c
  common/rd_codec.c
  common/rd_crypto.c
  common/rd_net.c
  common/rd_secure.c
  common/rd_ws.c)

option(TD_STATIC_SDL "Link SDL2 statically into the viewer (portable binaries)" OFF)

if(WIN32)
  add_compile_definitions(_WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES
                          _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
endif()

if(MSVC)
  set(TD_WARNINGS /W3)
else()
  set(TD_WARNINGS -Wall -Wextra)
endif()

add_library(td_common STATIC ${COMMON_SOURCES})
target_include_directories(td_common PUBLIC common)
target_compile_options(td_common PRIVATE ${TD_WARNINGS})
if(WIN32)
  target_link_libraries(td_common PUBLIC ws2_32 bcrypt)
endif()

# ---------------------------------------------------------------- relay
# Internet relay (runs on a server; see relay/). TD_RELAY_ONLY builds just it.
option(TD_RELAY_ONLY "Build only the relay server" OFF)
if(NOT EMSCRIPTEN)
  add_executable(tetherdesk-relay relay/relay.cpp)
  target_link_libraries(tetherdesk-relay PRIVATE td_common)
  target_compile_options(tetherdesk-relay PRIVATE ${TD_WARNINGS})
endif()
if(TD_RELAY_ONLY)
  return()
endif()

# ---------------------------------------------------------------- TLS
# Native apps reach the internet relay over HTTPS/WSS using mbedTLS (C),
# built from source so there is no system dependency.
if(NOT EMSCRIPTEN)
  include(FetchContent)
  set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
  set(GEN_FILES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(mbedtls)
  add_library(td_tls STATIC common/rd_tls.c)
  target_link_libraries(td_tls PUBLIC td_common mbedtls mbedx509 mbedcrypto)
  if(APPLE)
    target_link_libraries(td_tls PUBLIC "-framework Security" "-framework CoreFoundation")
  elseif(WIN32)
    target_link_libraries(td_tls PUBLIC crypt32)
  endif()
endif()

if(NOT EMSCRIPTEN)
# ---------------------------------------------------------------- host
set(HOST_SOURCES
  host/main.cpp
  host/server.cpp
  host/encoder.cpp
  host/keymap.cpp
  host/platform_demo.cpp)

if(APPLE)
  list(APPEND HOST_SOURCES host/platform_mac.mm)
elseif(WIN32)
  list(APPEND HOST_SOURCES host/platform_win.cpp)
else()
  list(APPEND HOST_SOURCES host/platform_x11.cpp)
endif()

# The host is a library so it can also be built *into* the desktop app
# ("TetherDesk --run-host"), which makes the app a single self-contained file.
add_library(td_host STATIC ${HOST_SOURCES})
target_include_directories(td_host PUBLIC host)
target_link_libraries(td_host PUBLIC td_common td_tls)
target_compile_options(td_host PRIVATE ${TD_WARNINGS})
find_package(Threads REQUIRED)
target_link_libraries(td_host PUBLIC Threads::Threads)
add_executable(tetherdesk-host host/host_entry.cpp)
target_link_libraries(tetherdesk-host PRIVATE td_host)

if(APPLE)
  set_source_files_properties(host/platform_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  target_link_libraries(td_host PUBLIC
    "-framework ScreenCaptureKit" "-framework CoreGraphics" "-framework CoreMedia"
    "-framework CoreVideo" "-framework AppKit" "-framework ApplicationServices")
elseif(WIN32)
  target_link_libraries(td_host PUBLIC user32 gdi32 iphlpapi)
else()
  find_package(X11 REQUIRED)
  target_link_libraries(td_host PUBLIC X11::X11 X11::Xtst)
endif()

endif()

# ---------------------------------------------------------------- viewer
# The viewer is one C++ codebase built two ways: natively against SDL2, and
# with Emscripten into WebAssembly for browsers.
set(CLIENT_SOURCES
  client/main.cpp
  client/app.cpp
  client/store.cpp
  client/ui.cpp)

if(EMSCRIPTEN)
  add_executable(tetherdesk ${CLIENT_SOURCES} client/transport_web.cpp)
  target_link_libraries(tetherdesk PRIVATE td_common)
  set(TD_EM_FLAGS "-sUSE_SDL=2")
  target_compile_options(tetherdesk PRIVATE ${TD_EM_FLAGS} ${TD_WARNINGS})
  target_compile_options(td_common PRIVATE ${TD_EM_FLAGS})
  target_link_options(tetherdesk PRIVATE
    ${TD_EM_FLAGS}
    -lwebsocket.js
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=64MB
    -sSTACK_SIZE=1MB
    -sENVIRONMENT=web
    -sEXPORTED_RUNTIME_METHODS=[]
    --shell-file ${CMAKE_SOURCE_DIR}/web/shell.html)
  set_target_properties(tetherdesk PROPERTIES OUTPUT_NAME "index" SUFFIX ".html")
  return()
endif()

if(TD_STATIC_SDL)
  # Build official SDL2 from source as a static library so release binaries
  # have no runtime dependency (Homebrew now ships SDL2 as sdl2-compat).
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-2.30.9.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(SDL2)
  set(SDL2_FOUND TRUE)
else()
  find_package(SDL2 CONFIG QUIET)
endif()
if(NOT SDL2_FOUND)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
  endif()
endif()

if(TARGET SDL2::SDL2 OR TARGET SDL2::SDL2-static OR TARGET PkgConfig::SDL2)
  set(TD_APP_EXTRA "")
  if(WIN32)
    set(TD_APP_EXTRA WIN32 packaging/windows/tetherdesk.rc)  # GUI app + icon
  elseif(APPLE)
    set(TD_APP_EXTRA MACOSX_BUNDLE packaging/macos/tetherdesk.icns)
  endif()
  add_executable(tetherdesk ${TD_APP_EXTRA} ${CLIENT_SOURCES} client/transport_native.cpp client/host_process.cpp)
  target_compile_options(tetherdesk PRIVATE ${TD_WARNINGS})
  set_target_properties(tetherdesk PROPERTIES OUTPUT_NAME TetherDesk)
  if(APPLE)
    # TetherDesk.app: the viewer, with the host bundled next to it so the
    # "Share this PC" tab works (see packaging/macos/make_dmg.sh).
    configure_file(packaging/macos/Info.plist.in ${CMAKE_BINARY_DIR}/Info.plist @ONLY)
    set_target_properties(tetherdesk PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/Info.plist)
    set_source_files_properties(packaging/macos/tetherdesk.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  endif()
  if(TARGET SDL2::SDL2main)
    target_link_libraries(tetherdesk PRIVATE SDL2::SDL2main)
  endif()
  target_link_libraries(tetherdesk PRIVATE td_host)
  target_compile_definitions(tetherdesk PRIVATE TD_EMBED_HOST=1)
  if(TD_STATIC_SDL AND TARGET SDL2::SDL2-static)
    target_link_libraries(tetherdesk PRIVATE td_common SDL2::SDL2-static)
  elseif(TARGET SDL2::SDL2)
    target_link_libraries(tetherdesk PRIVATE td_common SDL2::SDL2)
  else()
    target_link_libraries(tetherdesk PRIVATE td_common PkgConfig::SDL2)
  endif()
else()
  message(WARNING "SDL2 not found: the native viewer will not be built (host still builds)")
endif()

if(WIN32)
  target_sources(tetherdesk-host PRIVATE packaging/windows/tetherdesk.rc)
endif()

# Headless client for health checks / end-to-end tests (no SDL needed).
add_executable(tetherdesk-probe tools/td_probe.cpp client/transport_native.cpp)
target_link_libraries(tetherdesk-probe PRIVATE td_common td_tls)
target_compile_options(tetherdesk-probe PRIVATE ${TD_WARNINGS})

# ---------------------------------------------------------------- tests
enable_testing()
add_executable(test_common tests/test_common.c)
target_link_libraries(test_common PRIVATE td_common)
add_test(NAME common COMMAND test_common)
