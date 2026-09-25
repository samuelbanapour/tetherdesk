// main.cpp - TetherDesk host entry point: options, platform selection,
// startup banner, signal handling.
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "platform.h"
#include "rd_crypto.h"
#include "rd_net.h"
#include "rd_proto.h"
#include "rd_secure.h"
#include "server.h"

#ifndef TD_DEFAULT_RELAY
#define TD_DEFAULT_RELAY "tetherdesk.54-151-75-113.nip.io"
#endif

namespace {

void usage() {
    std::printf(
        "TetherDesk host - share this computer's screen with TetherDesk viewers (native or browser)\n\n"
        "usage: tetherdesk-host [options]\n"
        "  --port N            listen port (default %d)\n"
        "  --bind ADDR         listen address (default :: = all IPv6+IPv4; 127.0.0.1 = this machine only)\n"
        "  --password PW       viewer password (default: a random one is generated)\n"
        "  --view-only         viewers can watch but not control\n"
        "  --no-files          refuse file uploads\n"
        "  --fps N             maximum frame rate, 1-60 (default 30)\n"
        "  --display N         display index to share (default 0)\n"
        "  --max-viewers N     simultaneous viewers (default 8)\n"
        "  --web-root DIR      directory containing the built web viewer\n"
        "  --downloads DIR     where received files are saved\n"
        "  --native-res        capture HiDPI displays at full pixel density (more bandwidth)\n"
        "  --key-file PATH     host identity key (default: in the user config directory)\n"
        "  --relay HOST[:PORT] internet relay to register with (default " TD_DEFAULT_RELAY ")\n"
        "  --no-relay          LAN/port-forwarding only: don't register with the relay\n"
        "  --threads N         encoder threads (default: CPU cores, max 8)\n"
        "  --demo              share a synthetic demo desktop (no OS permissions needed)\n"
        "  --no-console        don't read operator commands from stdin\n"
        "  --list-displays     print displays and exit\n",
        RD_DEFAULT_PORT);
}

std::string random_password() {
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";  // no look-alikes
    uint8_t raw[10];
    rd_random(raw, sizeof raw);
    std::string pw;
    for (int i = 0; i < 10; i++) {
        if (i == 5) pw += '-';
        pw += alphabet[raw[i] % (sizeof alphabet - 1)];
    }
    return pw;
}

std::string exe_dir(const char *argv0) {
    std::string p = argv0;
#ifdef __APPLE__
    char buf[4096];
    uint32_t sz = sizeof buf;
    if (_NSGetExecutablePath(buf, &sz) == 0) p = buf;
#elif defined(__linux__)
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) p.assign(buf, size_t(n));
#endif
    size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

bool dir_has_index(const std::string &dir) {
    FILE *f = std::fopen((dir + "/index.html").c_str(), "rb");
    if (f) std::fclose(f);
    return f != nullptr;
}

std::string home_dir() {
    const char *h = std::getenv("HOME");
#ifdef _WIN32
    if (!h) h = std::getenv("USERPROFILE");
#endif
    return h ? h : ".";
}

void print_lan_urls(int port) {
#ifndef _WIN32
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) return;
    for (ifaddrs *a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if ((a->ifa_flags & IFF_LOOPBACK) || !(a->ifa_flags & IFF_UP)) continue;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(a->ifa_addr)->sin_addr, ip, sizeof ip);
        std::printf("    http://%s:%d/\n", ip, port);
    }
    freeifaddrs(list);
#else
    (void)port;
#endif
}

std::string config_dir() {
#ifdef _WIN32
    const char *appdata = std::getenv("APPDATA");
    return std::string(appdata ? appdata : ".") + "\\TetherDesk";
#elif defined(__APPLE__)
    return home_dir() + "/Library/Application Support/TetherDesk";
#else
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    return (xdg && *xdg ? std::string(xdg) : home_dir() + "/.config") + "/tetherdesk";
#endif
}

// The host's long-term X25519 identity. Viewers pin its fingerprint the first
// time they connect, so it must stay the same across restarts.
bool load_or_create_host_key(const std::string &path, uint8_t priv[32], uint8_t pub[32]) {
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        size_t n = std::fread(priv, 1, 32, f);
        std::fclose(f);
        if (n == 32) {
            rd_x25519_base(pub, priv);
            return true;
        }
    }
    if (rd_x25519_keypair(priv, pub) != 0) return false;
    std::string dir = path.substr(0, path.find_last_of("/\\"));
#ifdef _WIN32
    std::system(("mkdir \"" + dir + "\" 2>nul").c_str());
#else
    std::string cur;
    for (size_t i = 0; i <= dir.size(); i++) {
        if (i == dir.size() || dir[i] == '/') {
            if (!cur.empty()) mkdir(cur.c_str(), 0700);
        }
        if (i < dir.size()) cur += dir[i];
    }
#endif
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
#ifndef _WIN32
    chmod(path.c_str(), 0600);
#endif
    bool ok = std::fwrite(priv, 1, 32, f) == 32;
    std::fclose(f);
    return ok;
}

// A stable 9-digit relay ID plus the secret that proves we own it, so people
// can reach this computer by the same ID every time.
bool load_or_create_relay_id(const std::string &path, std::string &id, std::string &key) {
    if (FILE *f = std::fopen(path.c_str(), "r")) {
        char a[32] = "", b[80] = "";
        bool ok = std::fscanf(f, "%31s %79s", a, b) == 2;
        std::fclose(f);
        if (ok && std::strlen(a) == 9 && std::strlen(b) == 32) {
            id = a;
            key = b;
            return true;
        }
    }
    uint8_t r[20];
    if (rd_random(r, sizeof r) != 0) return false;
    uint32_t n = (uint32_t(r[0]) << 24 | uint32_t(r[1]) << 16 | uint32_t(r[2]) << 8 | r[3]) % 900000000u + 100000000u;
    id = std::to_string(n);
    static const char hex[] = "0123456789abcdef";
    key.clear();
    for (int i = 4; i < 20; i++) key += hex[r[i] >> 4], key += hex[r[i] & 15];
    FILE *f = std::fopen(path.c_str(), "w");
    if (!f) return false;
#ifndef _WIN32
    chmod(path.c_str(), 0600);
#endif
    std::fprintf(f, "%s %s\n", id.c_str(), key.c_str());
    std::fclose(f);
    return true;
}

std::string pretty_id(const std::string &id) {
    return id.size() == 9 ? id.substr(0, 3) + " " + id.substr(3, 3) + " " + id.substr(6) : id;
}

void on_signal(int) { td::Server::stop(); }

}  // namespace

// Entry point shared by the standalone tetherdesk-host executable
// (host_entry.cpp) and the desktop app's "--run-host" mode.
int td_host_main(int argc, char **argv) {
    td::ServerConfig cfg;
    td::PlatformOptions popts;
    bool demo = false, list_only = false;
    std::string key_file;
    std::string relay = TD_DEFAULT_RELAY;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--port") cfg.port = std::atoi(next());
        else if (a == "--bind") cfg.bind = next();
        else if (a == "--password") cfg.password = next();
        else if (a == "--view-only") cfg.view_only = true;
        else if (a == "--no-files") cfg.allow_files = false;
        else if (a == "--fps") cfg.max_fps = std::max(1, std::min(60, std::atoi(next())));
        else if (a == "--display") cfg.display = std::atoi(next());
        else if (a == "--max-viewers") cfg.max_viewers = std::max(1, std::atoi(next()));
        else if (a == "--web-root") cfg.web_root = next();
        else if (a == "--downloads") cfg.downloads_dir = next();
        else if (a == "--native-res") popts.native_resolution = true;
        else if (a == "--key-file") key_file = next();
        else if (a == "--relay") relay = next();
        else if (a == "--no-relay") relay.clear();
        else if (a == "--threads") cfg.encoder_threads = std::max(1, std::min(64, std::atoi(next())));
        else if (a == "--demo") demo = true;
        else if (a == "--no-console") cfg.console = false;
        else if (a == "--list-displays") list_only = true;
        else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n\n", a.c_str());
            usage();
            return 2;
        }
    }

    if (rd_net_init() != 0) {
        std::fprintf(stderr, "error: network initialisation failed\n");
        return 1;
    }
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    td::Platform plat;
    std::string err;
    if (demo) {
        td::create_demo_platform(plat);
    } else if (!td::create_native_platform(plat, popts, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    auto displays = plat.capturer->displays();
    if (list_only) {
        for (size_t i = 0; i < displays.size(); i++)
            std::printf("%zu: %s (%dx%d)\n", i, displays[i].name.c_str(), displays[i].width, displays[i].height);
        return 0;
    }

    if (cfg.web_root.empty()) {
        std::string d = exe_dir(argv[0]);
        for (const std::string &cand : {d + "/web", d + "/../Resources/web", d + "/../web", d + "/../share/tetherdesk/web",
                                         std::string("web")})
            if (dir_has_index(cand)) {
                cfg.web_root = cand;
                break;
            }
    }
    if (cfg.downloads_dir.empty()) cfg.downloads_dir = home_dir() + "/Downloads/TetherDesk";
    if (key_file.empty()) key_file = config_dir() + "/host_key";
    if (!load_or_create_host_key(key_file, cfg.static_priv, cfg.static_pub)) {
        std::fprintf(stderr, "error: cannot read or create the host key at %s\n", key_file.c_str());
        return 1;
    }
    if (!relay.empty()) {
        size_t colon = relay.rfind(':');
        cfg.relay_host = colon == std::string::npos ? relay : relay.substr(0, colon);
        cfg.relay_port = colon == std::string::npos ? 443 : std::atoi(relay.substr(colon + 1).c_str());
        std::string dir = key_file.substr(0, key_file.find_last_of("/\\"));
        if (!load_or_create_relay_id(dir + "/relay_id", cfg.relay_id, cfg.relay_key)) {
            std::fprintf(stderr, "warning: cannot store relay ID - internet access disabled\n");
            cfg.relay_host.clear();
        }
    }
    char fingerprint[40];
    rd_fingerprint(cfg.static_pub, fingerprint);
    const bool loopback = cfg.bind == "127.0.0.1" || cfg.bind == "::1";
    if (!cfg.password.empty() && cfg.password.size() < 8 && !loopback) {
        std::fprintf(stderr, "error: passwords must be at least 8 characters when the host is reachable from the "
                             "network (or use --bind 127.0.0.1)\n");
        return 1;
    }
    bool generated = cfg.password.empty();
    if (generated) cfg.password = random_password();
    char host[256] = "host";
    gethostname(host, sizeof host - 1);
    cfg.host_name = host;

    std::printf("\n  TetherDesk host  -  %s\n", plat.os_name.c_str());
    std::printf("  ------------------------------------------------------------\n");
    if (cfg.display >= 0 && size_t(cfg.display) < displays.size())
        std::printf("  Sharing   %s (%dx%d)\n", displays[size_t(cfg.display)].name.c_str(),
                    displays[size_t(cfg.display)].width, displays[size_t(cfg.display)].height);
    std::printf("  Password  %s%s\n", cfg.password.c_str(), generated ? "   (generated - use --password to choose)" : "");
    if (!cfg.relay_host.empty())
        std::printf("  ID        %s   (connect from anywhere via %s)\n", pretty_id(cfg.relay_id).c_str(),
                    cfg.relay_host.c_str());
    std::printf("  Identity  %s   (viewers see this the first time they connect)\n", fingerprint);
    std::printf("  Mode      %s, up to %d fps, %d viewers max%s\n", cfg.view_only ? "view only" : "full control",
                cfg.max_fps, cfg.max_viewers, cfg.allow_files ? "" : ", file transfer off");
    std::printf("  Web       %s\n", cfg.web_root.empty() ? "(web viewer not built - native viewer only)" : cfg.web_root.c_str());
    std::printf("\n  Connect a browser or `tetherdesk <address>` to:\n    http://localhost:%d/\n", cfg.port);
    if (cfg.bind != "127.0.0.1" && cfg.bind != "::1") print_lan_urls(cfg.port);
    std::printf("\n  Sessions are end-to-end encrypted (X25519 + ChaCha20-Poly1305); the relay only\n"
                "  forwards ciphertext. Local network: port %d.\n",
                cfg.port);
    if (cfg.console) std::printf("  Type /help for operator commands.\n");
    std::printf("\n");
    std::fflush(stdout);

    td::Server server(cfg, plat);
    return server.run();
}
