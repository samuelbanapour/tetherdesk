cmake_minimum_required(VERSION 3.16)
project(TetherDesk VERSION 1.0.0 LANGUAGES C CXX)

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
  common/rd_ws.c)

if(MSVC)
  set(TD_WARNINGS /W4)
else()
  set(TD_WARNINGS -Wall -Wextra)
endif()

add_library(td_common STATIC ${COMMON_SOURCES})
target_include_directories(td_common PUBLIC common)
target_compile_options(td_common PRIVATE ${TD_WARNINGS})
if(WIN32)
  target_link_libraries(td_common PUBLIC ws2_32 bcrypt)
endif()

# ---------------------------------------------------------------- viewer
# The viewer is one C++ codebase built two ways: natively against SDL2, and
# with Emscripten into WebAssembly for browsers.
set(CLIENT_SOURCES
  client/main.cpp
  client/app.cpp
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

find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
  endif()
endif()

if(TARGET SDL2::SDL2 OR TARGET PkgConfig::SDL2)
  add_executable(tetherdesk ${CLIENT_SOURCES} client/transport_native.cpp)
  target_compile_options(tetherdesk PRIVATE ${TD_WARNINGS})
  if(TARGET SDL2::SDL2main)
    target_link_libraries(tetherdesk PRIVATE SDL2::SDL2main)
  endif()
  if(TARGET SDL2::SDL2)
    target_link_libraries(tetherdesk PRIVATE td_common SDL2::SDL2)
  else()
    target_link_libraries(tetherdesk PRIVATE td_common PkgConfig::SDL2)
  endif()
else()
  message(WARNING "SDL2 not found: the native viewer will not be built (host still builds)")
endif()

# ---------------------------------------------------------------- host
set(HOST_SOURCES
  host/main.cpp
  host/server.cpp
  host/encoder.cpp
  host/keymap.cpp
  host/platform_demo.cpp)

if(APPLE)
  enable_language(OBJCXX)
  list(APPEND HOST_SOURCES host/platform_mac.mm)
elseif(WIN32)
  list(APPEND HOST_SOURCES host/platform_win.cpp)
else()
  list(APPEND HOST_SOURCES host/platform_x11.cpp)
endif()

add_executable(tetherdesk-host ${HOST_SOURCES})
target_include_directories(tetherdesk-host PRIVATE host)
target_link_libraries(tetherdesk-host PRIVATE td_common)
target_compile_options(tetherdesk-host PRIVATE ${TD_WARNINGS})
find_package(Threads REQUIRED)
target_link_libraries(tetherdesk-host PRIVATE Threads::Threads)

if(APPLE)
  set_source_files_properties(host/platform_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  target_link_libraries(tetherdesk-host PRIVATE
    "-framework ScreenCaptureKit" "-framework CoreGraphics" "-framework CoreMedia"
    "-framework CoreVideo" "-framework AppKit" "-framework ApplicationServices")
elseif(WIN32)
  target_link_libraries(tetherdesk-host PRIVATE user32 gdi32 iphlpapi)
else()
  find_package(X11 REQUIRED)
  target_link_libraries(tetherdesk-host PRIVATE X11::X11 X11::Xtst)
endif()

# Headless client for health checks / end-to-end tests (no SDL needed).
add_executable(tetherdesk-probe tools/td_probe.cpp client/transport_native.cpp)
target_link_libraries(tetherdesk-probe PRIVATE td_common)
target_compile_options(tetherdesk-probe PRIVATE ${TD_WARNINGS})

# ---------------------------------------------------------------- tests
enable_testing()
add_executable(test_common tests/test_common.c)
target_link_libraries(test_common PRIVATE td_common)
add_test(NAME common COMMAND test_common)
