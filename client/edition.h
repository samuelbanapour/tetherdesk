#pragma once
// Which app the client sources are being built as.
//
//   TetherDesk         connect to computers, share this one, help people
//                      (chat, view-only sharing, one-off passwords).
//   TetherDesk Remote  (TD_EDITION_REMOTE) only for reaching your own
//                      computers: connect out, plus always-on remote access
//                      to this computer with a password you choose. No
//                      support features.
//
// Each edition keeps its own settings folder, computer ID and background
// service, so both can be installed side by side.

#include <cstddef>

namespace td::edition {

#ifdef TD_EDITION_REMOTE
inline constexpr bool remote = true;
inline constexpr const char *name = "TetherDesk Remote";
inline constexpr const char *tagline = "Your computers, from anywhere";
inline constexpr const char *data_folder = "TetherDesk Remote";
inline constexpr const char *service_label = "com.soloappsstudio.tetherdesk.remote.host";
inline constexpr const wchar_t *service_run_value = L"TetherDesk Remote Always On";
inline constexpr const wchar_t *service_mutex = L"Local\\TetherDeskRemoteHostService";
inline constexpr const wchar_t *service_stop_event = L"Local\\TetherDeskRemoteHostServiceStop";
inline constexpr const wchar_t *service_restart_event = L"Local\\TetherDeskRemoteHostServiceRestart";
#else
inline constexpr bool remote = false;
inline constexpr const char *name = "TetherDesk";
inline constexpr const char *tagline = "Remote Desktop";
inline constexpr const char *data_folder = "TetherDesk";
inline constexpr const char *service_label = "com.soloappsstudio.tetherdesk.host";
inline constexpr const wchar_t *service_run_value = L"TetherDesk Always On";
inline constexpr const wchar_t *service_mutex = L"Local\\TetherDeskHostService";
inline constexpr const wchar_t *service_stop_event = L"Local\\TetherDeskHostServiceStop";
inline constexpr const wchar_t *service_restart_event = L"Local\\TetherDeskHostServiceRestart";
#endif

// Passwords you choose yourself for TetherDesk Remote must be at least this long.
inline constexpr size_t min_password = 8;

}  // namespace td::edition
