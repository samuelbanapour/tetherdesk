// main.cpp - TetherDesk viewer entry point.
//
// Native:  tetherdesk [host[:port]] [--password PW] [--name NAME] [--fullscreen]
// Web:     the page passes (hostname, port, location.hash) as argv, so the
//          viewer connects back to the host that served it. The hash may
//          carry "#password=...&name=..." to pre-fill the form.
#include <SDL.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#include "app.h"
#include "host_process.h"
#include "host_service.h"
#include "rd_proto.h"

#ifdef TD_EMBED_HOST
int td_host_main(int argc, char **argv);
#endif

namespace {

std::string url_decode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size()) {
            out += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else out += s[i];
    }
    return out;
}

void parse_hash(const std::string &hash, td::Options &o) {
    size_t i = hash.find_first_not_of('#');
    while (i != std::string::npos && i < hash.size()) {
        size_t amp = hash.find('&', i);
        std::string kv = hash.substr(i, amp == std::string::npos ? std::string::npos : amp - i);
        size_t eq = kv.find('=');
        std::string k = kv.substr(0, eq), v = eq == std::string::npos ? "" : url_decode(kv.substr(eq + 1));
        if (k == "password") o.password = v;
        else if (k == "name") o.name = v;
        else if (k == "host") o.host = v;
        else if (k == "port") o.port = std::atoi(v.c_str());
        else if (k == "relay") o.web_relay = true;  // page was served by the internet relay
        else if (k == "id") o.connect_id = v;
        i = amp == std::string::npos ? amp : amp + 1;
    }
    if (!o.password.empty() && !o.web_relay) o.autoconnect = true;
}

#ifndef __EMSCRIPTEN__
void usage() {
    std::printf("usage: tetherdesk [host[:port]] [--password PW] [--name NAME] [--fullscreen] [--stats]\n"
                "                  [--share | --share-demo] [--menu] [--screenshot FILE.bmp [--after SECONDS]]\n");
}
#else
void web_tick(void *arg) {
    static_cast<td::App *>(arg)->tick();
}
#endif

}  // namespace

int main(int argc, char **argv) {
#ifdef TD_EMBED_HOST
    // The app doubles as the host: "Share this PC" runs it with --run-host.
    if (argc > 1 && std::strcmp(argv[1], "--run-host") == 0) return td_host_main(argc - 1, argv + 1);
#endif
#ifdef _WIN32
    if (argc > 1 && std::strcmp(argv[1], "--host-service") == 0) return td::service_watchdog_main(argc - 1, argv + 1);
#endif
    td::Options opts;
#ifdef __EMSCRIPTEN__
    if (argc > 1 && argv[1][0]) opts.host = argv[1];
    if (argc > 2 && argv[2][0]) opts.port = std::atoi(argv[2]);
    opts.web_secure = argc > 4 && std::strcmp(argv[4], "https:") == 0;
    opts.web_host = opts.host;
    opts.web_port = opts.port;
    opts.name = "Browser viewer";
    if (argc > 3) parse_hash(argv[3], opts);
#else
    const char *user = std::getenv("USER");
    if (!user) user = std::getenv("USERNAME");
    opts.name = user ? user : "Viewer";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--password") opts.password = next();
        else if (a == "--name") opts.name = next();
        else if (a == "--port") opts.port = std::atoi(next().c_str());
        else if (a == "--fullscreen") opts.fullscreen = true;
        else if (a == "--stats") opts.show_stats = true;
        else if (a == "--menu") opts.open_menu = true;
        else if (a == "--trust") opts.trust_new_hosts = true;
        else if (a == "--share") opts.start_sharing = true;
        else if (a == "--share-demo") opts.start_sharing = opts.share_demo = true;
        else if (a == "--quick-support") opts.quick_support = true;
        else if (a == "--relay") opts.relay = next();
        else if (a == "--screenshot") opts.screenshot_path = next();
        else if (a == "--after") opts.screenshot_after = std::atof(next().c_str());
        else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a.rfind("tetherdesk://", 0) == 0) {
            // tetherdesk://host:port/#password=...  (handy for launchers)
            std::string rest = a.substr(13);
            size_t hash = rest.find('#');
            if (hash != std::string::npos) parse_hash(rest.substr(hash), opts), rest = rest.substr(0, hash);
            rest = rest.substr(0, rest.find('/'));
            size_t colon = rest.rfind(':');
            opts.host = colon == std::string::npos ? rest : rest.substr(0, colon);
            if (colon != std::string::npos) opts.port = std::atoi(rest.substr(colon + 1).c_str());
        } else if (a[0] != '-') {
            size_t colon = a.rfind(':');
            // host:port (but leave bare IPv6 addresses alone)
            if (colon != std::string::npos && a.find(':') == colon) {
                opts.host = a.substr(0, colon);
                opts.port = std::atoi(a.substr(colon + 1).c_str());
            } else {
                opts.host = a;
            }
            opts.autoconnect = true;
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    if (!opts.password.empty()) opts.autoconnect = true;
    // A copy named "...QuickSupport..." opens straight into sharing: the
    // person being helped just runs it and reads out the ID and password.
    std::string exe = td::executable_path();
    for (auto &ch : exe) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (exe.find("quicksupport") != std::string::npos) opts.quick_support = true;
    if (opts.quick_support) opts.start_sharing = true;
#endif
    if (opts.port <= 0) opts.port = RD_DEFAULT_PORT;

    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
#ifdef __EMSCRIPTEN__
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    int w = 1280, h = 800;
#ifdef __EMSCRIPTEN__
    double cw, ch;
    emscripten_get_element_css_size("#canvas", &cw, &ch);
    w = int(cw);
    h = int(ch);
#endif
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (opts.fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    SDL_Window *win = SDL_CreateWindow("TetherDesk", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!win) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(win, 480, 360);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }

    auto *app = new td::App(win, ren, opts);
#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(web_tick, app, 0, 1);  // never returns
#else
    while (app->tick()) {
        // With vsync the present call paces us; when minimised it doesn't.
        if (SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) SDL_Delay(16);
    }
    delete app;
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
#endif
    return 0;
}
