// host_service.h - "Always on" sharing: keeps the host running in the
// background, starting at login and restarting after any crash, even when
// the TetherDesk window is closed.
//
//   macOS    a per-user LaunchAgent (launchd: RunAtLoad + KeepAlive)
//   Windows  a per-user startup entry (HKCU\...\Run) that launches a small
//            watchdog ("TetherDesk --host-service"), which restarts the host
//            whenever it exits and whenever the password file changes
//
// The host always reads its password from a 0600 file (--password-file), so
// the password never appears in process listings or the startup entry.
#pragma once

#include <string>
#include <vector>

namespace td {

bool service_supported();
// Installs (or updates) and starts the service running `exe host_args...`
// with output going to `log_path`.
bool service_install(const std::string &exe, const std::vector<std::string> &host_args, const std::string &log_path,
                     std::string &err);
void service_uninstall();
bool service_installed();
// Restarts the host so it picks up a new password or settings.
void service_restart();

#ifdef _WIN32
// Entry point for "TetherDesk --host-service <host args...>" (the watchdog).
int service_watchdog_main(int argc, char **argv);
#endif

}  // namespace td
