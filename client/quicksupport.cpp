// quicksupport.cpp - TetherDesk QuickSupport: a small, purpose-built app for
// the person *receiving* help. It only shares this computer: no connecting
// out, no saved PCs, no settings, nothing left running after it closes.
//
// On start it creates a fresh one-time password, starts the built-in host
// ("--run-host") registered with the relay, and shows the ID and password to
// read out. Closing the window (or "End session") stops sharing and deletes
// the password.
#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "host_process.h"
#include "rd_crypto.h"
#include "ui.h"

int td_host_main(int argc, char **argv);

namespace {

using namespace td;

std::string random_password() {
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    uint8_t raw[10];
    rd_random(raw, sizeof raw);
    std::string pw;
    for (int i = 0; i < 10; i++) {
        if (i == 5) pw += '-';
        pw += alphabet[raw[i] % (sizeof alphabet - 1)];
    }
    return pw;
}

std::string pretty_id(const std::string &id) {
    return id.size() == 9 ? id.substr(0, 3) + " " + id.substr(3, 3) + " " + id.substr(6) : id;
}

struct Status {
    std::string id, relay, error;
    bool online = false, needs_screen = false, needs_input = false, started = false;
    std::set<std::string> connected;  // viewer names currently connected
    std::vector<std::string> activity;
};

// The host reports everything on stdout; read its log for the status.
void parse_log(const std::string &path, Status &st) {
    std::ifstream f(path);
    std::string line;
    Status n;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("  ID ", 0) == 0) {
            std::string v = line.substr(5);
            v.erase(0, v.find_first_not_of(' '));
            std::string digits;
            for (char c : v.substr(0, 11))
                if (c >= '0' && c <= '9') digits += c;
            n.id = digits;
            size_t via = line.find("via ");
            if (via != std::string::npos) n.relay = line.substr(via + 4, line.find(')', via) - via - 4);
            n.started = true;
        } else if (line.find("relay: online as") != std::string::npos) {
            n.online = true;
        } else if (line.find("relay connection lost") != std::string::npos) {
            n.online = false;
        } else if (line.rfind("error:", 0) == 0) {
            n.error = line.substr(7);
        }
        if (line.find("Screen Recording") != std::string::npos) n.needs_screen = true;
        if (line.find("Accessibility") != std::string::npos) n.needs_input = true;
        // "[hh:mm:ss] Name connected from ..." / "Name (addr): reason" / "Name left"
        if (line.size() > 11 && line[0] == '[' && line[9] == ']' && line.find("relay:") == std::string::npos) {
            std::string msg = line.substr(11);
            size_t p;
            if ((p = msg.find(" connected from ")) != std::string::npos) n.connected.insert(msg.substr(0, p));
            else if ((p = msg.find(" left")) != std::string::npos && p + 5 == msg.size()) n.connected.erase(msg.substr(0, p));
            else if ((p = msg.find(" (")) != std::string::npos && msg.find("): ") != std::string::npos)
                n.connected.erase(msg.substr(0, p));
            if (msg.find("wrong password") == std::string::npos) n.activity.push_back(line);
        }
    }
    if (n.activity.size() > 5) n.activity.erase(n.activity.begin(), n.activity.end() - 5);
    st = n;
}

}  // namespace

int main(int argc, char **argv) {
    // The same executable is the host: the window runs it with --run-host.
    if (argc > 1 && std::strcmp(argv[1], "--run-host") == 0) return td_host_main(argc - 1, argv + 1);

    std::string relay = "tetherdesk.54-151-75-113.nip.io";
    std::string screenshot;
    double shot_after = 5;
    bool demo = false;  // share the synthetic demo desktop (tests, screenshots)
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--relay" && i + 1 < argc) relay = argv[++i];
        else if (a == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
        else if (a == "--after" && i + 1 < argc) shot_after = std::atof(argv[++i]);
        else if (a == "--demo") demo = true;
    }

#ifdef SDL_HINT_WINDOWS_DPI_AWARENESS
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) return 1;
    SDL_Window *win = SDL_CreateWindow("TetherDesk QuickSupport", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 600,
                                       500, SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!ren) return 1;
    Ui ui;
    ui.init(ren);

    // A private folder for this session's log and one-time password.
    std::string dir = ".";
    if (const char *d = std::getenv("TETHERDESK_DATA_DIR"); d && *d) dir = d;
    else if (char *p = SDL_GetPrefPath("TetherDesk", "QuickSupport")) {
        dir = p;
        SDL_free(p);
    }
    if (dir.back() != '/' && dir.back() != '\\') dir += '/';
    const std::string log_path = dir + "session.log", pw_path = dir + "session_password";
    std::string password;
    HostProcess host;
    std::string start_error;

    auto start = [&]() {
        host.stop();
        password = random_password();
        if (FILE *f = std::fopen(pw_path.c_str(), "w")) {
#ifndef _WIN32
            fchmod(fileno(f), 0600);
#endif
            std::fprintf(f, "%s\n", password.c_str());
            std::fclose(f);
        }
        std::remove(log_path.c_str());
        std::string err;
        // Own identity + relay ID, separate from the main app's (which may be
        // sharing this computer "always on" at the same time).
        std::vector<std::string> args = {"--run-host", "--password-file", pw_path, "--no-console", "--no-files",
                                         "--relay", relay, "--key-file", dir + "host_key"};
        if (demo) args.push_back("--demo");
        if (!host.start(executable_path(), args, log_path, err))
            start_error = err;
    };
    start();

    Status st;
    uint32_t last_read = 0;
    bool running = true, mouse_down = false;
    float mx = 0, my = 0;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_MOUSEMOTION) mx = float(e.motion.x), my = float(e.motion.y);
            else if ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) && e.button.button == SDL_BUTTON_LEFT) {
                mx = float(e.button.x), my = float(e.button.y);
                mouse_down = e.type == SDL_MOUSEBUTTONDOWN;
                ui.on_mouse(mouse_down, mx, my);
            }
        }
        const uint32_t now = SDL_GetTicks();
        if (now - last_read > 500) {
            last_read = now;
            parse_log(log_path, st);
            if (!host.running() && start_error.empty() && st.error.empty() && st.started) st.error = "Sharing stopped";
        }

        int ww, wh, ow, oh;
        SDL_GetWindowSize(win, &ww, &wh);
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        const float scale = ww ? float(ow) / ww : 1.f, W = float(ww);
        ui.begin(scale, mx, my, mouse_down);
        SDL_SetRenderDrawColor(ren, theme::bg.r, theme::bg.g, theme::bg.b, 255);
        SDL_RenderClear(ren);

        const float pad = 28;
        float y = 24;
        ui.fill({pad, y + 4, 22, 22}, theme::accent, 5);
        ui.fill({pad + 12, y + 14, 22, 22}, {98, 163, 255, 160}, 5);
        ui.text(pad + 46, y, "TetherDesk QuickSupport", theme::text, 1.35f);
        ui.text(pad + 46, y + 26, "Let someone you trust see and control this computer", theme::dim);
        y += 70;

        // Status line.
        std::string status;
        Color sc = theme::dim;
        const std::string err = !start_error.empty() ? start_error : st.error;
        if (!err.empty()) status = "Not sharing - " + err, sc = theme::bad;
        else if (!st.connected.empty()) status = "Connected: " + *st.connected.begin() + " can see this screen", sc = theme::warn;
        else if (st.online) status = "Ready - waiting for your helper to connect", sc = theme::good;
        else status = "Getting ready...";
        ui.fill({pad, y + 5, 10, 10}, sc, 5);
        ui.text(pad + 18, y, status, sc, 1.05f);
        y += 34;

        // The two things to read out.
        Rect card{pad, y, W - 2 * pad, 150};
        ui.fill(card, theme::panel, 12);
        ui.outline(card, theme::panel_border, 12);
        const float half = (card.w - 36) / 2;
        ui.text(card.x + 18, card.y + 16, "Your ID", theme::dim);
        ui.text(card.x + 18, card.y + 42, st.online && !st.id.empty() ? pretty_id(st.id) : "--- --- ---",
                st.online ? theme::text : theme::dim, 2.0f);
        ui.text(card.x + 18 + half, card.y + 16, "Password", theme::dim);
        ui.text(card.x + 18 + half, card.y + 42, password, theme::text, 2.0f);
        if (ui.button({card.x + 18 + half, card.y + 104, 150, 30}, "New password")) start();
        y += card.h + 16;

        ui.text(pad, y, "Tell these to the person helping you. They connect with the TetherDesk app, or at", theme::dim);
        y += 22;
        ui.text(pad, y, "https://" + (st.relay.empty() ? relay : st.relay) + "/app", theme::accent_hover);
        y += 34;

        if (st.needs_screen || st.needs_input) {
            Rect box{pad, y, W - 2 * pad, st.needs_screen && st.needs_input ? 96.f : 56.f};
            ui.fill(box, {60, 45, 15, 200}, 10);
            float yy = y + 12;
            if (st.needs_screen) {
                ui.text(pad + 14, yy + 2, "Allow Screen Recording, then press New password", theme::warn);
                if (ui.button({box.x + box.w - 176, yy - 2, 164, 28}, "Open settings"))
                    SDL_OpenURL("x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture");
                yy += 40;
            }
            if (st.needs_input) {
                ui.text(pad + 14, yy + 2, "Allow Accessibility so your helper can use the mouse", theme::warn);
                if (ui.button({box.x + box.w - 176, yy - 2, 164, 28}, "Open settings"))
                    SDL_OpenURL("x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility");
            }
            y += box.h + 14;
        }

        for (auto &a : st.activity) {
            ui.text(pad, y, a.size() > 90 ? a.substr(0, 90) + "..." : a, theme::dim);
            y += 20;
        }

        const float H = float(wh);
        ui.fill({0, H - 64, W, 1}, theme::panel_border);
        ui.text(pad, H - 42, "Closing this window ends the session.", theme::dim);
        if (ui.button({W - pad - 140, H - 50, 140, 34}, "End session", true)) running = false;

        ui.end();
        if (!screenshot.empty() && SDL_GetTicks() >= uint32_t(shot_after * 1000)) {
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
            if (s && SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0)
                SDL_SaveBMP(s, screenshot.c_str());
            SDL_FreeSurface(s);
            running = false;
        }
        SDL_RenderPresent(ren);
    }

    // End of session: stop sharing and forget the password.
    host.stop();
    std::remove(pw_path.c_str());
    ui.shutdown();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
