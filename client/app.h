// app.h - the TetherDesk viewer: one codebase for the native (SDL2) and web
// (Emscripten/WebAssembly) builds.
#pragma once

#include <SDL.h>

#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "rd_bytes.h"
#include "transport.h"
#include "ui.h"

namespace td {

struct Options {
    std::string host = "localhost";
    int port = 5980;
    std::string password;
    std::string name;
    bool autoconnect = false;
    bool fullscreen = false;
    bool show_stats = false;
    bool open_menu = false;         // open the session menu once connected
    std::string screenshot_path;    // save the rendered window, then quit
    double screenshot_after = 3.0;  // seconds after start
};

class App {
public:
    App(SDL_Window *window, SDL_Renderer *renderer, Options opts);
    ~App();
    // Processes events and network traffic, then draws one frame.
    // Returns false once the user has asked to quit.
    bool tick();

private:
    enum class Screen { Connect, Session };
    enum class Phase { None, Connecting, Hello, Auth, Live };
    enum class ScaleMode { Fit, Native };

    struct Toast {
        std::string text;
        Color color;
        uint32_t t0;
    };
    struct Upload {
        uint32_t id = 0;
        std::string name, path;
        FILE *file = nullptr;
        uint64_t size = 0, sent = 0;
        bool begun = false, done_sending = false;
    };
    struct DisplayEntry {
        std::string name;
        int w, h;
    };
    struct Viewer {
        std::string name, addr;
        bool view_only;
    };

    // connection & protocol
    void start_connect();
    void disconnect_user();
    void on_transport_closed();
    void handle_message(const std::vector<uint8_t> &m);
    void on_frame(rd_reader &r, size_t wire_bytes);
    void resize_remote(int w, int h);
    void send(const rd_buf &msg);
    void send_settings();
    void send_pointer(int wheel_x = 0, int wheel_y = 0);
    void send_key(uint16_t hid, bool down);
    void release_input();
    void send_chat();
    void send_clipboard(const std::string &text);
    void check_local_clipboard();

    // input
    void handle_event(const SDL_Event &e);
    void handle_connect_key(const SDL_Event &e);
    void handle_session_event(const SDL_Event &e);
    bool over_ui(float x, float y) const;
    void window_to_remote(float wx, float wy, int &rx, int &ry) const;

    // uploads
    void queue_upload(const char *path);
    void pump_uploads();

    // drawing
    void render();
    void draw_connect();
    void draw_session();
    void draw_menu();
    void draw_hud();
    void draw_toasts();
    void toast(const std::string &text, Color c = theme::text);
    void update_view();

    SDL_Window *win_;
    SDL_Renderer *ren_;
    Options opts_;
    Ui ui_;
    bool running_ = true;
    float scale_ = 1;  // output pixels per window point
    int out_w_ = 0, out_h_ = 0;

    // connect screen
    Screen screen_ = Screen::Connect;
    std::string f_host_, f_port_, f_name_, f_password_;
    int focus_ = 3;
    std::string error_;

    // connection
    std::unique_ptr<Transport> transport_;
    Phase phase_ = Phase::None;
    bool user_closed_ = false;
    int reconnect_attempts_ = 0;
    uint32_t reconnect_at_ = 0;
    uint32_t connect_started_ = 0;

    // remote screen
    int rw_ = 0, rh_ = 0;
    std::vector<uint32_t> fb_;
    SDL_Texture *tex_ = nullptr;
    std::vector<DisplayEntry> displays_;
    int cur_display_ = 0;
    std::string host_name_, host_os_;
    bool view_only_ = false;
    std::vector<Viewer> viewers_;
    uint32_t last_refresh_req_ = 0;
    float view_x_ = 0, view_y_ = 0, view_s_ = 1;  // remote -> output pixel transform

    // settings
    int quality_ = 255;  // 255 = auto
    int fps_ = 30;
    ScaleMode scale_mode_ = ScaleMode::Fit;
    bool show_stats_ = false;

    // overlays
    bool menu_open_ = false;
    Rect menu_rect_{0, 0, 0, 0};
    Rect pill_rect_{0, 0, 0, 0};
    bool chat_open_ = false;
    std::string chat_text_;
    std::deque<Toast> toasts_;

    // input state
    float mouse_x_ = 0, mouse_y_ = 0;
    bool mouse_down_ = false;
    int ptr_x_ = 0, ptr_y_ = 0;
    uint8_t buttons_ = 0;
    bool ptr_dirty_ = false;
    bool held_[256] = {};

    // stats
    uint32_t stats_t0_ = 0, last_ping_ = 0;
    int frames_ = 0, fps_shown_ = 0, tiles_last_ = 0;
    uint64_t bytes_ = 0;
    double mbps_ = 0;
    double rtt_ms_ = -1;

    // clipboard & files
    uint32_t last_clip_check_ = 0;
    std::string last_local_clip_, last_remote_clip_;
    std::deque<Upload> uploads_;
    uint32_t next_upload_id_ = 1;
};

}  // namespace td
