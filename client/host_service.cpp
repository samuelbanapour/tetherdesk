#include "host_service.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "host_process.h"

#if defined(__APPLE__)
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace td {

#if defined(__APPLE__)

static const char *kLabel = "com.soloappsstudio.tetherdesk.host";

static std::string plist_path() {
    const char *home = std::getenv("HOME");
    return std::string(home ? home : "") + "/Library/LaunchAgents/" + kLabel + ".plist";
}

static std::string xml_escape(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else o += c;
    }
    return o;
}

// Runs launchctl and returns its exit status (output discarded).
static int launchctl(const std::vector<std::string> &args) {
    std::vector<char *> argv;
    std::string prog = "/bin/launchctl";
    argv.push_back(const_cast<char *>(prog.c_str()));
    for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    int rc = posix_spawn(&pid, prog.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return -1;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static std::string domain() { return "gui/" + std::to_string(getuid()); }

bool service_supported() { return true; }

bool service_install(const std::string &exe, const std::vector<std::string> &host_args, const std::string &log_path,
                     std::string &err) {
    std::string args = "    <string>" + xml_escape(exe) + "</string>\n";
    for (auto &a : host_args) args += "    <string>" + xml_escape(a) + "</string>\n";
    std::string plist =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "  <key>Label</key><string>" + std::string(kLabel) + "</string>\n"
        "  <key>ProgramArguments</key>\n  <array>\n" + args + "  </array>\n"
        "  <key>RunAtLoad</key><true/>\n"
        "  <key>KeepAlive</key><true/>\n"
        "  <key>ThrottleInterval</key><integer>5</integer>\n"
        "  <key>ProcessType</key><string>Interactive</string>\n"
        "  <key>StandardOutPath</key><string>" + xml_escape(log_path) + "</string>\n"
        "  <key>StandardErrorPath</key><string>" + xml_escape(log_path) + "</string>\n"
        "</dict>\n</plist>\n";
    std::string path = plist_path();
    std::string dir = path.substr(0, path.rfind('/'));
    mkdir(dir.c_str(), 0755);
    FILE *f = std::fopen(path.c_str(), "w");
    if (!f) {
        err = "cannot write " + path;
        return false;
    }
    std::fputs(plist.c_str(), f);
    std::fclose(f);
    std::remove(log_path.c_str());  // fresh log for the status display
    launchctl({"bootout", domain() + "/" + kLabel});  // replace any older version
    if (launchctl({"bootstrap", domain(), path}) != 0) {
        err = "macOS refused to start the background service";
        return false;
    }
    return true;
}

void service_uninstall() {
    launchctl({"bootout", domain() + "/" + kLabel});
    std::remove(plist_path().c_str());
}

bool service_installed() {
    struct stat st;
    return stat(plist_path().c_str(), &st) == 0;
}

void service_restart() { launchctl({"kickstart", "-k", domain() + "/" + kLabel}); }

#elif defined(_WIN32)

static const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t *kRunValue = L"TetherDesk Always On";
static const wchar_t *kMutex = L"Local\\TetherDeskHostService";
static const wchar_t *kStopEvent = L"Local\\TetherDeskHostServiceStop";
static const wchar_t *kRestartEvent = L"Local\\TetherDeskHostServiceRestart";

static std::wstring widen(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static std::wstring quote(const std::wstring &a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring o = L"\"";
    for (wchar_t c : a) {
        if (c == L'"') o += L'\\';
        o += c;
    }
    return o + L"\"";
}

bool service_supported() { return true; }

bool service_install(const std::string &exe, const std::vector<std::string> &host_args, const std::string &log_path,
                     std::string &err) {
    service_uninstall();
    std::wstring cmd = quote(widen(exe)) + L" --host-service " + quote(widen(log_path));
    for (auto &a : host_args) cmd += L" " + quote(widen(a));
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        err = "cannot register TetherDesk to start with Windows";
        return false;
    }
    RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE *>(cmd.c_str()),
                   DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    // Start the watchdog now, detached from this window.
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                        nullptr, &si, &pi)) {
        err = "cannot start the background service";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void service_uninstall() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, kRunValue);
        RegCloseKey(key);
    }
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEvent)) {
        SetEvent(ev);
        CloseHandle(ev);
    }
    Sleep(500);
}

bool service_installed() {
    HKEY key;
    bool found = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        found = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        RegCloseKey(key);
    }
    return found;
}

void service_restart() {
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kRestartEvent)) {
        SetEvent(ev);
        CloseHandle(ev);
    }
}

// The watchdog: runs "<self> --run-host <args>", restarting it whenever it
// exits (after a short pause) or when asked to, until told to stop.
int service_watchdog_main(int argc, char **argv) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutex);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;  // one watchdog per user session
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, kStopEvent);
    HANDLE restart = CreateEventW(nullptr, FALSE, FALSE, kRestartEvent);
    if (argc < 2) return 2;
    std::string log_path = argv[1];
    std::vector<std::string> args = {"--run-host"};
    for (int i = 2; i < argc; i++) args.push_back(argv[i]);
    const std::string exe = executable_path();
    for (;;) {
        HostProcess host;
        std::string err;
        if (host.start(exe, args, log_path, err)) {
            while (host.running()) {
                HANDLE evs[2] = {stop, restart};
                DWORD w = WaitForMultipleObjects(2, evs, FALSE, 1000);
                if (w == WAIT_OBJECT_0) {
                    host.stop();
                    CloseHandle(mutex);
                    return 0;
                }
                if (w == WAIT_OBJECT_0 + 1) host.stop();
            }
        }
        if (WaitForSingleObject(stop, 3000) == WAIT_OBJECT_0) break;
    }
    CloseHandle(mutex);
    return 0;
}

#else

bool service_supported() { return false; }
bool service_install(const std::string &, const std::vector<std::string> &, const std::string &, std::string &err) {
    err = "Always-on sharing isn't available on this system yet";
    return false;
}
void service_uninstall() {}
bool service_installed() { return false; }
void service_restart() {}

#endif

}  // namespace td
