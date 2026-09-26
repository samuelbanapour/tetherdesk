// app.h - the TetherDesk viewer: one codebase for the native (SDL2) and web
// (Emscripten/WebAssembly) builds.
//
// Native builds open on a Remote Desktop-style home screen (saved PCs with
// thumbnails, quick connect); the web build shows a single connect form for
// the host that served the page.
#pragma once

#include <SDL.h>

#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "host_process.h"
#include "rd_bytes.h"
#include "rd_secure.h"
#include "store.h"
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
    bool trust_new_hosts = false;   // skip the first-connection fingerprint prompt
    bool start_sharing = false;     // open "Share this PC" and turn sharing on
    bool share_demo = false;        // ...sharing the demo desktop (not saved)
    bool quick_support = false;     // minimal "get help" mode: just the Share screen
    std::string relay = "tetherdesk.54-151-75-113.nip.io";  // for connecting by ID (native)
    // Web builds: where the page came from.
    bool web_secure = false, web_relay = false;
    std::string web_host, connect_id;
    int web_port = 80;
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
    enum class Screen { Home, Session };
    enum class Phase { None, Connecting, Hello, Verify, Auth, Live };
    enum class Dialog { None, EditPc, Password, Verify, Connecting };
    enum class ScaleMode { Fit, Native };
    enum class HomeTab { Connect, Share };

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
    struct Field {
        std::string *value;
        bool secret, digits;
    };

    // connection & protocol
    void begin_connect(const SavedPc &pc);  // asks for a password if needed
    void start_connect();
    void send_auth();
    void disconnect_user();
    void on_transport_closed();
    void fail_connect(const std::string &why);
    void handle_message(std::vector<uint8_t> &m);
    void on_frame(rd_reader &r, size_t wire_bytes);
    void resize_remote(int w, int h);
    void send(const rd_buf &msg);
    void send_simple(uint8_t type);
    void send_settings();
    void send_pointer(int wheel_x = 0, int wheel_y = 0);
    void send_key(uint16_t hid, bool down);
    void release_input();
    void send_chat();
    void send_clipboard(const std::string &text);
    void check_local_clipboard();
    void set_fullscreen(bool on);
    bool fullscreen() const;

    // input
    void handle_event(const SDL_Event &e);
    void handle_form_event(const SDL_Event &e);
    void handle_session_event(const SDL_Event &e);
    bool over_ui(float x, float y) const;
    void window_to_remote(float wx, float wy, int &rx, int &ry) const;

    // uploads
    void queue_upload(const char *path);
    void pump_uploads();

    // thumbnails
    void save_thumbnail();
    SDL_Texture *thumbnail(const std::string &id);

    // drawing
    void render();
    void draw_home();
    void draw_share(float top);
    void start_sharing();
    void stop_sharing();
    void poll_share_log();
    void draw_web_connect();
    void draw_dialog();
    void draw_session();
    void draw_connection_bar();
    void draw_menu();
    void draw_hud();
    void draw_toasts();
    void toast(const std::string &text, Color c = theme::text);
    void switch_screen(int index);
    void update_view();
    Rect dialog_frame(float w, float h, const std::string &title);
    void form_field(const Rect &r, const std::string &label, std::string &value, bool secret = false,
                    bool digits = false);

    SDL_Window *win_;
    SDL_Renderer *ren_;
    Options opts_;
    Ui ui_;
    Store store_;
    bool running_ = true;
    float scale_ = 1;  // output pixels per window point
    int out_w_ = 0, out_h_ = 0;

    // home screen / dialogs
    Screen screen_ = Screen::Home;
    Dialog dialog_ = Dialog::None;
    std::vector<Field> form_;       // fields drawn this frame, in tab order
    std::vector<Field> last_form_;  // fields drawn last frame (receive keystrokes)
    int focus_ = 0;
    bool submit_ = false, cancel_ = false;
    std::string quick_host_;
    SavedPc edit_;
    std::string edit_port_;
    bool edit_is_new_ = false;
    std::string pw_input_;
    bool pw_remember_ = false;
    std::string dialog_error_;
    std::map<std::string, SDL_Texture *> thumbs_;
    std::string pending_delete_;
    float home_scroll_ = 0;
    HomeTab tab_ = HomeTab::Connect;

    // "Share this PC": the bundled host runs as a child process.
#ifndef __EMSCRIPTEN__
    HostProcess host_;
#endif
    bool sharing_ = false;
    std::string share_log_path_, share_identity_, share_error_, share_id_, share_relay_;
    bool share_relay_online_ = false;
    std::vector<std::string> share_urls_, share_activity_;
    bool share_needs_screen_perm_ = false, share_needs_input_perm_ = false;
    bool share_show_pw_ = false;
    uint32_t share_log_read_ = 0;

    // web connect form
    std::string f_host_, f_port_, f_name_, f_password_;

    // connection
    std::unique_ptr<Transport> transport_;
    SavedPc target_;
    Phase phase_ = Phase::None;
    bool user_closed_ = false;
    int reconnect_attempts_ = 0;
    uint32_t reconnect_at_ = 0;
    std::string error_;
    uint8_t ce_priv_[32] = {}, ce_[32] = {};
    uint8_t transcript_[32] = {};
    rd_channel ch_{};
    std::string host_fp_, verify_old_fp_;
    std::vector<uint8_t> plain_;
    rd_buf sealed_{};

    // remote screen
    int rw_ = 0, rh_ = 0;
    std::vector<uint32_t> fb_;
    SDL_Texture *tex_ = nullptr;
    std::vector<DisplayEntry> displays_;
    int cur_display_ = 0;
    std::string host_name_, host_os_;
    bool view_only_ = false;
    std::vector<Viewer> viewers_;
    uint32_t last_refresh_req_ = 0, last_thumb_ = 0;
    float view_x_ = 0, view_y_ = 0, view_s_ = 1;  // remote -> output pixel transform

    // settings
    int quality_ = 255;  // 255 = auto
    bool hires_supported_ = false, hires_on_ = false;
    int resolution_pref_ = 255;  // 0 sharp, 1 fast, 255 = leave as the host has it
    int fps_ = 30;
    ScaleMode scale_mode_ = ScaleMode::Fit;
    bool show_stats_ = false;

    // overlays
    bool menu_open_ = false;
    Rect menu_rect_{0, 0, 0, 0};
    Rect bar_rect_{0, 0, 0, 0};
    bool bar_pinned_ = false;
    uint32_t bar_until_ = 0;
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
