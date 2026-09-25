#include "host_process.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>
extern char **environ;
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace td {

std::string executable_dir() {
    std::string p = executable_path();
    size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

std::string executable_path() {
    std::string p;
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof buf / sizeof buf[0]));
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, int(n), nullptr, 0, nullptr, nullptr);
    p.resize(size_t(len));
    WideCharToMultiByte(CP_UTF8, 0, buf, int(n), p.data(), len, nullptr, nullptr);
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t sz = sizeof buf;
    if (_NSGetExecutablePath(buf, &sz) == 0) p = buf;
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) p.assign(buf, size_t(n));
#endif
    return p;
}

#ifdef _WIN32

static std::wstring widen(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// Quote one argument per the MSVCRT command-line rules.
static std::wstring quote_arg(const std::wstring &a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring o = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            bs++;
        } else if (c == L'"') {
            o.append(bs * 2 + 1, L'\\');
            o += c;
            bs = 0;
        } else {
            o.append(bs, L'\\');
            o += c;
            bs = 0;
        }
    }
    o.append(bs * 2, L'\\');
    return o + L"\"";
}

bool HostProcess::start(const std::string &exe, const std::vector<std::string> &args, const std::string &log_path,
                        std::string &err) {
    stop();
    exit_code_ = -1;
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE log = CreateFileW(widen(log_path).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        err = "cannot create " + log_path;
        return false;
    }
    std::wstring cmd = quote_arg(widen(exe));
    for (auto &a : args) cmd += L" " + quote_arg(widen(a));
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = si.hStdError = log;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(log);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        err = "cannot start " + exe + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    return true;
}

bool HostProcess::running() {
    if (!process_) return false;
    if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) return true;
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    exit_code_ = int(code);
    CloseHandle(process_);
    process_ = nullptr;
    return false;
}

void HostProcess::stop() {
    if (!process_) return;
    TerminateProcess(process_, 0);
    WaitForSingleObject(process_, 3000);
    CloseHandle(process_);
    process_ = nullptr;
}

#else

bool HostProcess::start(const std::string &exe, const std::vector<std::string> &args, const std::string &log_path,
                        std::string &err) {
    stop();
    exit_code_ = -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(exe.c_str()));
    for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    int rc = posix_spawn(&pid_, exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        pid_ = 0;
        err = "cannot start " + exe + " (error " + std::to_string(rc) + ")";
        return false;
    }
    return true;
}

bool HostProcess::running() {
    if (pid_ <= 0) return false;
    int status = 0;
    pid_t r = waitpid(pid_, &status, WNOHANG);
    if (r == 0) return true;
    exit_code_ = (r == pid_ && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    pid_ = 0;
    return false;
}

void HostProcess::stop() {
    if (pid_ <= 0) return;
    kill(pid_, SIGTERM);  // the host shuts down cleanly on SIGTERM
    for (int i = 0; i < 30; i++) {
        if (waitpid(pid_, nullptr, WNOHANG) == pid_) {
            pid_ = 0;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    kill(pid_, SIGKILL);
    waitpid(pid_, nullptr, 0);
    pid_ = 0;
}

#endif

}  // namespace td
