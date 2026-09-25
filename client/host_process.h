// host_process.h - runs the bundled tetherdesk-host as a child process, so
// the desktop app can offer "Share this PC" without a terminal.
#pragma once

#include <string>
#include <vector>

namespace td {

class HostProcess {
public:
    ~HostProcess() { stop(); }
    // Starts `exe args...` with stdout/stderr written to `log_path`.
    bool start(const std::string &exe, const std::vector<std::string> &args, const std::string &log_path,
               std::string &err);
    void stop();
    bool running();
    // Exit code once the process has ended (-1 if unknown / still running).
    int exit_code() const { return exit_code_; }

private:
#ifdef _WIN32
    void *process_ = nullptr;  // HANDLE
#else
    int pid_ = 0;
#endif
    int exit_code_ = -1;
};

// Full path of the running executable, and its directory (no trailing separator).
std::string executable_path();
std::string executable_dir();

}  // namespace td
