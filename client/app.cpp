#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rd_codec.h"
#include "rd_crypto.h"
#include "rd_net.h"
#include "rd_proto.h"

namespace td {

namespace {

constexpr float kMenuW = 580;
const char *kQualityNames[] = {"Lossless", "High", "Medium", "Low"};

std::string ellipsize(const Ui &ui, std::string s, float max_w) {
    if (ui.text_width(s) <= max_w) return s;
    while (!s.empty() && ui.text_width(s + "...") > max_w) s.pop_back();
    return s + "...";
}

bool is_web() {
#ifdef __EMSCRIPTEN__
    return true;
#else
    return false;
#endif
}

}  // namespace

App::App(SDL_Window *window, SDL_Renderer *renderer, Options opts)
    : win_(window), ren_(renderer), opts_(std::move(opts)) {
    ui_.init(ren_);
    f_host_ = opts_.host;
    f_port_ = std::to_string(opts_.port);
    f_name_ = opts_.name;
    f_password_ = opts_.password;
    focus_ = f_password_.empty() ? 3 : 0;
    transport_ = Transport::create();
    show_stats_ = opts_.show_stats;
    SDL_StartTextInput();
    if (opts_.autoconnect && !f_password_.empty()) start_connect();
}

App::~App() {
    if (transport_) transport_->close();
    for (auto &u : uploads_)
        if (u.file) std::fclose(u.file);
    if (tex_) SDL_DestroyTexture(tex_);
    ui_.shutdown();
}

// ------------------------------------------------------------ main tick

bool App::tick() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) handle_event(e);

    const uint32_t now = SDL_GetTicks();
    if (phase_ != Phase::None) {
        std::vector<std::vector<uint8_t>> msgs;
        transport_->poll(msgs);
        for (auto &m : msgs) {
            handle_message(m);
            if (phase_ == Phase::None) break;
        }
        if (phase_ == Phase::Connecting && transport_->state() == Transport::State::Open) {
            rd_buf msg;
            rd_buf_init(&msg);
            rd_buf_put_u8(&msg, RD_C_HELLO);
            rd_buf_put_u16(&msg, RD_PROTO_VERSION);
            rd_buf_put_str(&msg, f_name_.empty() ? "Viewer" : f_name_.c_str());
            send(msg);
            rd_buf_free(&msg);
            phase_ = Phase::Hello;
        }
        if (phase_ != Phase::None && transport_->state() == Transport::State::Closed) on_transport_closed();
    } else if (reconnect_at_ && now >= reconnect_at_) {
        reconnect_at_ = 0;
        start_connect();
    }

    if (phase_ == Phase::Live) {
        if (ptr_dirty_) send_pointer();
        if (now - last_ping_ >= 2000) {
            last_ping_ = now;
            rd_buf msg;
            rd_buf_init(&msg);
            rd_buf_put_u8(&msg, RD_C_PING);
            rd_buf_put_u64(&msg, rd_now_us());
            send(msg);
            rd_buf_free(&msg);
        }
        if (now - last_clip_check_ >= 1000) {
            last_clip_check_ = now;
            check_local_clipboard();
        }
        pump_uploads();
        if (now - stats_t0_ >= 1000) {
            double secs = (now - stats_t0_) / 1000.0;
            fps_shown_ = int(std::lround(frames_ / secs));
            mbps_ = bytes_ * 8 / secs / 1e6;
            frames_ = 0;
            bytes_ = 0;
            stats_t0_ = now;
        }
    }

    render();
    return running_;
}

// ------------------------------------------------------------ connection

void App::start_connect() {
    error_.clear();
    if (f_host_.empty()) {
        error_ = "Enter the host's address";
        return;
    }
    int port = std::atoi(f_port_.c_str());
    if (port <= 0 || port > 65535) {
        error_ = "Port must be 1-65535";
        return;
    }
    user_closed_ = false;
    phase_ = Phase::Connecting;
    connect_started_ = SDL_GetTicks();
    transport_->connect(f_host_, port);
}

void App::disconnect_user() {
    user_closed_ = true;
    release_input();
    transport_->close();
    phase_ = Phase::None;
    reconnect_at_ = 0;
    reconnect_attempts_ = 0;
    screen_ = Screen::Connect;
    menu_open_ = chat_open_ = false;
    for (auto &u : uploads_)
        if (u.file) std::fclose(u.file);
    uploads_.clear();
    SDL_SetWindowTitle(win_, "TetherDesk");
    SDL_StartTextInput();
}

void App::on_transport_closed() {
    std::string why = transport_->error();
    bool was_live = phase_ == Phase::Live;
    phase_ = Phase::None;
    for (auto &u : uploads_)
        if (u.file) std::fclose(u.file);
    uploads_.clear();
    std::fill(std::begin(held_), std::end(held_), false);
    buttons_ = 0;

    // Keep the session screen up and retry for a while if a live session drops.
    if (!user_closed_ && (was_live || (screen_ == Screen::Session && reconnect_attempts_ > 0)) &&
        reconnect_attempts_ < 10) {
        reconnect_attempts_++;
        reconnect_at_ = SDL_GetTicks() + uint32_t(std::min(reconnect_attempts_, 4)) * 1000;
        if (was_live) toast("Connection lost (" + why + ") - reconnecting...", theme::warn);
        return;
    }
    reconnect_attempts_ = 0;
    screen_ = Screen::Connect;
    menu_open_ = chat_open_ = false;
    error_ = why.empty() ? "Disconnected" : why;
    SDL_SetWindowTitle(win_, "TetherDesk");
    SDL_StartTextInput();
}

void App::send(const rd_buf &msg) { transport_->send(msg.data, msg.len); }

void App::handle_message(const std::vector<uint8_t> &m) {
    rd_reader r;
    rd_reader_init(&r, m.data(), m.size());
    const uint8_t type = rd_get_u8(&r);
    rd_buf out;
    rd_buf_init(&out);

    switch (type) {
    case RD_S_HELLO: {
        uint16_t version = rd_get_u16(&r);
        uint8_t method = rd_get_u8(&r);
        const uint8_t *nonce = rd_get_bytes(&r, RD_NONCE_LEN);
        char name[256], os[256];
        rd_get_str(&r, name, sizeof name);
        rd_get_str(&r, os, sizeof os);
        if (r.err || version != RD_PROTO_VERSION || method != RD_AUTH_HMAC_SHA256 || phase_ != Phase::Hello) {
            error_ = "Incompatible host version";
            user_closed_ = true;
            transport_->close();
            break;
        }
        host_name_ = name;
        host_os_ = os;
        uint8_t mac[32];
        rd_hmac_sha256(f_password_.data(), f_password_.size(), nonce, RD_NONCE_LEN, mac);
        rd_buf_put_u8(&out, RD_C_AUTH);
        rd_buf_put(&out, mac, sizeof mac);
        send(out);
        phase_ = Phase::Auth;
        break;
    }
    case RD_S_AUTH_RESULT: {
        bool ok = rd_get_u8(&r) != 0;
        bool vo = rd_get_u8(&r) != 0;
        char text[512];
        rd_get_str(&r, text, sizeof text);
        if (!ok) {
            user_closed_ = true;  // don't auto-reconnect into a rejection
            reconnect_attempts_ = 0;
            transport_->close();
            phase_ = Phase::None;
            screen_ = Screen::Connect;
            error_ = text;
            SDL_StartTextInput();
            break;
        }
        view_only_ = vo;
        if (phase_ == Phase::Auth) {
            phase_ = Phase::Live;
            screen_ = Screen::Session;
            if (reconnect_attempts_) toast("Reconnected", theme::good);
            else toast(std::string(text) + " to " + host_name_ + " - press F8 for the menu", theme::good);
            reconnect_attempts_ = 0;
            stats_t0_ = last_ping_ = last_clip_check_ = SDL_GetTicks();
            char *clip = SDL_HasClipboardText() ? SDL_GetClipboardText() : nullptr;
            last_local_clip_ = clip ? clip : "";
            SDL_free(clip);
            SDL_StopTextInput();
            send_settings();
            if (opts_.open_menu) menu_open_ = true;
            SDL_SetWindowTitle(win_, ("TetherDesk - " + host_name_).c_str());
        } else {
            toast(text, vo ? theme::warn : theme::good);
            if (vo) release_input();
        }
        break;
    }
    case RD_S_DISPLAY_INFO: {
        int w = rd_get_u16(&r), h = rd_get_u16(&r);
        cur_display_ = rd_get_u8(&r);
        int n = rd_get_u8(&r);
        displays_.clear();
        for (int i = 0; i < n && !r.err; i++) {
            char name[256];
            rd_get_str(&r, name, sizeof name);
            int dw = rd_get_u16(&r), dh = rd_get_u16(&r);
            displays_.push_back({name, dw, dh});
        }
        if (!r.err && w > 0 && h > 0) resize_remote(w, h);
        break;
    }
    case RD_S_FRAME: on_frame(r, m.size()); break;
    case RD_S_CLIPBOARD: {
        uint32_t len;
        const uint8_t *p = rd_get_blob(&r, &len);
        if (r.err) break;
        std::string text(reinterpret_cast<const char *>(p), len);
        last_remote_clip_ = last_local_clip_ = text;
        SDL_SetClipboardText(text.c_str());
        toast("Clipboard updated from host (" + std::to_string(text.size()) + " chars)", theme::dim);
        break;
    }
    case RD_S_CHAT: {
        char from[256], text[4096];
        rd_get_str(&r, from, sizeof from);
        rd_get_str(&r, text, sizeof text);
        if (!r.err) toast(std::string(from) + ": " + text, theme::accent_hover);
        break;
    }
    case RD_S_PONG: {
        uint64_t t = rd_get_u64(&r);
        if (!r.err) rtt_ms_ = double(rd_now_us() - t) / 1000.0;
        break;
    }
    case RD_S_VIEWERS: {
        int n = rd_get_u8(&r);
        viewers_.clear();
        for (int i = 0; i < n && !r.err; i++) {
            char name[256], addr[128];
            rd_get_str(&r, name, sizeof name);
            rd_get_str(&r, addr, sizeof addr);
            bool vo = rd_get_u8(&r) != 0;
            viewers_.push_back({name, addr, vo});
        }
        break;
    }
    case RD_S_NOTICE: {
        char text[1024];
        rd_get_str(&r, text, sizeof text);
        if (!r.err) toast(text, theme::dim);
        break;
    }
    case RD_S_FILE_RESULT: {
        uint32_t id = rd_get_u32(&r);
        bool ok = rd_get_u8(&r) != 0;
        char text[2048];
        rd_get_str(&r, text, sizeof text);
        for (auto it = uploads_.begin(); it != uploads_.end(); ++it)
            if (it->id == id) {
                if (it->file) std::fclose(it->file);
                toast((ok ? "Sent " : "Couldn't send ") + it->name + " - " + text, ok ? theme::good : theme::bad);
                uploads_.erase(it);
                break;
            }
        break;
    }
    default: break;
    }
    rd_buf_free(&out);
}

void App::resize_remote(int w, int h) {
    if (w == rw_ && h == rh_ && tex_) return;
    rw_ = w;
    rh_ = h;
    fb_.assign(size_t(w) * h, 0xFF000000u);
    if (tex_) SDL_DestroyTexture(tex_);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    if (tex_) SDL_UpdateTexture(tex_, nullptr, fb_.data(), w * 4);
}

void App::on_frame(rd_reader &r, size_t wire_bytes) {
    uint32_t id = rd_get_u32(&r);
    rd_get_u8(&r);  // flags
    int n = rd_get_u16(&r);
    if (r.err || !tex_) return;
    bool bad = false;
    int minx = rw_, miny = rh_, maxx = 0, maxy = 0;
    std::vector<SDL_Rect> rects;
    rects.reserve(size_t(n));
    for (int i = 0; i < n; i++) {
        int x = rd_get_u16(&r), y = rd_get_u16(&r), w = rd_get_u16(&r), h = rd_get_u16(&r);
        int enc = rd_get_u8(&r);
        uint32_t len;
        const uint8_t *p = rd_get_blob(&r, &len);
        if (r.err || x + w > rw_ || y + h > rh_ ||
            rd_decode_tile(enc, p, len, w, h, fb_.data() + size_t(y) * rw_ + x, size_t(rw_)) != 0) {
            bad = true;
            break;
        }
        rects.push_back({x, y, w, h});
        minx = std::min(minx, x);
        miny = std::min(miny, y);
        maxx = std::max(maxx, x + w);
        maxy = std::max(maxy, y + h);
    }
    // Upload changed regions: individually when few, one bounding box when many.
    if (rects.size() > 48) {
        SDL_Rect u = {minx, miny, maxx - minx, maxy - miny};
        SDL_UpdateTexture(tex_, &u, fb_.data() + size_t(miny) * rw_ + minx, rw_ * 4);
    } else {
        for (auto &rc : rects) SDL_UpdateTexture(tex_, &rc, fb_.data() + size_t(rc.y) * rw_ + rc.x, rw_ * 4);
    }

    rd_buf ack;
    rd_buf_init(&ack);
    rd_buf_put_u8(&ack, RD_C_FRAME_ACK);
    rd_buf_put_u32(&ack, id);
    send(ack);
    if (bad && SDL_GetTicks() - last_refresh_req_ > 1000) {
        last_refresh_req_ = SDL_GetTicks();
        rd_buf_clear(&ack);
        rd_buf_put_u8(&ack, RD_C_REFRESH);
        send(ack);
    }
    rd_buf_free(&ack);
    frames_++;
    bytes_ += wire_bytes;
    tiles_last_ = n;
}

void App::send_settings() {
    rd_buf msg;
    rd_buf_init(&msg);
    rd_buf_put_u8(&msg, RD_C_SETTINGS);
    rd_buf_put_u8(&msg, uint8_t(quality_));
    rd_buf_put_u8(&msg, uint8_t(fps_));
    rd_buf_put_u8(&msg, uint8_t(cur_display_));
    send(msg);
    rd_buf_free(&msg);
}

void App::send_pointer(int wheel_x, int wheel_y) {
    ptr_dirty_ = false;
    if (view_only_ || phase_ != Phase::Live) return;
    rd_buf msg;
    rd_buf_init(&msg);
    rd_buf_put_u8(&msg, RD_C_POINTER);
    rd_buf_put_u16(&msg, uint16_t(ptr_x_));
    rd_buf_put_u16(&msg, uint16_t(ptr_y_));
    rd_buf_put_u8(&msg, buttons_);
    rd_buf_put_u16(&msg, uint16_t(int16_t(wheel_x)));
    rd_buf_put_u16(&msg, uint16_t(int16_t(wheel_y)));
    send(msg);
    rd_buf_free(&msg);
}

void App::send_key(uint16_t hid, bool down) {
    if (view_only_ || phase_ != Phase::Live || hid >= 256) return;
    held_[hid] = down;
    rd_buf msg;
    rd_buf_init(&msg);
    rd_buf_put_u8(&msg, RD_C_KEY);
    rd_buf_put_u16(&msg, hid);
    rd_buf_put_u8(&msg, down ? 1 : 0);
    send(msg);
    rd_buf_free(&msg);
}

void App::release_input() {
    for (int k = 0; k < 256; k++)
        if (held_[k]) send_key(uint16_t(k), false);
    if (buttons_) {
        buttons_ = 0;
        send_pointer();
    }
}

void App::send_chat() {
    if (chat_text_.empty()) return;
    rd_buf msg;
    rd_buf_init(&msg);
    rd_buf_put_u8(&msg, RD_C_CHAT);
    rd_buf_put_str(&msg, chat_text_.c_str());
    send(msg);
    rd_buf_free(&msg);
    toast("You: " + chat_text_, theme::text);
    chat_text_.clear();
}

void App::send_clipboard(const std::string &text) {
    if (view_only_ || text.size() > RD_MAX_CLIPBOARD) return;
    rd_buf msg;
    rd_buf_init(&msg);
    rd_buf_put_u8(&msg, RD_C_CLIPBOARD);
    rd_buf_put_blob(&msg, text.data(), uint32_t(text.size()));
    send(msg);
    rd_buf_free(&msg);
    last_local_clip_ = text;
}

// Push local clipboard changes to the host automatically (native only: the
// browser build can't read the system clipboard without JavaScript).
void App::check_local_clipboard() {
    if (is_web() || view_only_ || !(SDL_GetWindowFlags(win_) & SDL_WINDOW_INPUT_FOCUS)) return;
    if (!SDL_HasClipboardText()) return;
    char *clip = SDL_GetClipboardText();
    std::string t = clip ? clip : "";
    SDL_free(clip);
    if (!t.empty() && t != last_local_clip_ && t != last_remote_clip_) send_clipboard(t);
}

// ------------------------------------------------------------ files

void App::queue_upload(const char *path) {
    if (phase_ != Phase::Live) return;
    if (view_only_) {
        toast("View-only viewers can't send files", theme::warn);
        return;
    }
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        toast(std::string("Can't open ") + path, theme::bad);
        return;
    }
    std::fseek(f, 0, SEEK_END);
    long long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        toast("Folders can't be sent - drop files instead", theme::warn);
        return;
    }
    Upload u;
    u.id = next_upload_id_++;
    u.path = path;
    const char *slash = std::strrchr(path, '/');
#ifdef _WIN32
    if (const char *bs = std::strrchr(path, '\\'); bs && (!slash || bs > slash)) slash = bs;
#endif
    u.name = slash ? slash + 1 : path;
    u.file = f;
    u.size = uint64_t(size);
    uploads_.push_back(u);
    toast("Sending " + u.name + " to the host...", theme::dim);
}

void App::pump_uploads() {
    if (uploads_.empty()) return;
    Upload *u = nullptr;
    for (auto &c : uploads_)
        if (!c.done_sending) {
            u = &c;
            break;
        }
    if (!u) return;
    rd_buf msg;
    rd_buf_init(&msg);
    if (!u->begun) {
        rd_buf_put_u8(&msg, RD_C_FILE_BEGIN);
        rd_buf_put_u32(&msg, u->id);
        rd_buf_put_u64(&msg, u->size);
        rd_buf_put_str(&msg, u->name.c_str());
        send(msg);
        u->begun = true;
    }
    // Keep at most ~1 MB in flight so screen updates stay responsive.
    std::vector<uint8_t> chunk(RD_FILE_CHUNK);
    while (u->sent < u->size && transport_->buffered() < (1u << 20)) {
        size_t n = std::fread(chunk.data(), 1, chunk.size(), u->file);
        if (n == 0) break;
        rd_buf_clear(&msg);
        rd_buf_put_u8(&msg, RD_C_FILE_CHUNK);
        rd_buf_put_u32(&msg, u->id);
        rd_buf_put(&msg, chunk.data(), n);
        send(msg);
        u->sent += n;
    }
    if (u->sent >= u->size || std::feof(u->file)) {
        rd_buf_clear(&msg);
        rd_buf_put_u8(&msg, RD_C_FILE_END);
        rd_buf_put_u32(&msg, u->id);
        send(msg);
        u->done_sending = true;
        std::fclose(u->file);
        u->file = nullptr;
    }
    rd_buf_free(&msg);
}

// ------------------------------------------------------------ events

void App::handle_event(const SDL_Event &e) {
    switch (e.type) {
    case SDL_QUIT: running_ = false; return;
    case SDL_MOUSEMOTION:
        mouse_x_ = float(e.motion.x);
        mouse_y_ = float(e.motion.y);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        mouse_x_ = float(e.button.x);
        mouse_y_ = float(e.button.y);
        if (e.button.button == SDL_BUTTON_LEFT) {
            mouse_down_ = e.type == SDL_MOUSEBUTTONDOWN;
            ui_.on_mouse(mouse_down_, mouse_x_, mouse_y_);
        }
        break;
    case SDL_DROPFILE:
        queue_upload(e.drop.file);
        SDL_free(e.drop.file);
        return;
    case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) release_input();
        break;
    default: break;
    }
    if (screen_ == Screen::Connect) handle_connect_key(e);
    else handle_session_event(e);
}

void App::handle_connect_key(const SDL_Event &e) {
    std::string *fields[4] = {&f_host_, &f_port_, &f_name_, &f_password_};
    if (e.type == SDL_TEXTINPUT) {
        std::string &f = *fields[focus_];
        if (f.size() < 200) f += e.text.text;
        if (focus_ == 1) f.erase(std::remove_if(f.begin(), f.end(), [](char c) { return c < '0' || c > '9'; }), f.end());
    } else if (e.type == SDL_KEYDOWN) {
        SDL_Keycode k = e.key.keysym.sym;
        bool mod = e.key.keysym.mod & (KMOD_CTRL | KMOD_GUI);
        if (k == SDLK_TAB) focus_ = (focus_ + ((e.key.keysym.mod & KMOD_SHIFT) ? 3 : 1)) % 4;
        else if (k == SDLK_BACKSPACE && !fields[focus_]->empty()) {
            std::string &f = *fields[focus_];
            do f.pop_back();  // drop a whole UTF-8 sequence
            while (!f.empty() && (static_cast<unsigned char>(f.back()) & 0xC0) == 0x80);
        } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
            if (phase_ == Phase::None) start_connect();
        } else if (k == SDLK_ESCAPE && phase_ != Phase::None) {
            disconnect_user();
        } else if (mod && k == SDLK_v) {
            char *clip = SDL_GetClipboardText();
            if (clip) {
                for (char *p = clip; *p; p++)
                    if (*p != '\n' && *p != '\r') *fields[focus_] += *p;
                SDL_free(clip);
            }
        }
    }
}

bool App::over_ui(float x, float y) const {
    if (menu_open_ && menu_rect_.contains(x, y)) return true;
    return pill_rect_.w > 0 && pill_rect_.contains(x, y);
}

void App::window_to_remote(float wx, float wy, int &rx, int &ry) const {
    float px = wx * scale_, py = wy * scale_;
    rx = int((px - view_x_) / view_s_);
    ry = int((py - view_y_) / view_s_);
    rx = std::max(0, std::min(rw_ - 1, rx));
    ry = std::max(0, std::min(rh_ - 1, ry));
}

void App::handle_session_event(const SDL_Event &e) {
    if (phase_ != Phase::Live) {
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) disconnect_user();
        return;
    }
    switch (e.type) {
    case SDL_TEXTINPUT:
        if (chat_open_ && chat_text_.size() < 900) chat_text_ += e.text.text;
        return;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        const bool down = e.type == SDL_KEYDOWN;
        const SDL_Keycode k = e.key.keysym.sym;
        if (chat_open_) {
            if (!down) return;
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER) send_chat();
            if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_ESCAPE) {
                chat_open_ = false;
                SDL_StopTextInput();
            } else if (k == SDLK_BACKSPACE && !chat_text_.empty()) {
                do chat_text_.pop_back();
                while (!chat_text_.empty() && (static_cast<unsigned char>(chat_text_.back()) & 0xC0) == 0x80);
            }
            return;
        }
        if (k == SDLK_F8) {
            if (down) menu_open_ = !menu_open_;
            return;
        }
        if (k == SDLK_F9) {
            if (down) {
                release_input();
                chat_open_ = true;
                menu_open_ = false;
                SDL_StartTextInput();
            }
            return;
        }
        if (k == SDLK_F11 && !is_web()) {
            if (down) SDL_SetWindowFullscreen(win_, (SDL_GetWindowFlags(win_) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            return;
        }
        if (menu_open_ && k == SDLK_ESCAPE) {
            if (down) menu_open_ = false;
            return;
        }
        if (e.key.repeat) return;  // the host OS generates its own auto-repeat
        SDL_Scancode sc = e.key.keysym.scancode;
        if (sc > 0 && sc < 256 && (down || held_[sc])) send_key(uint16_t(sc), down);
        return;
    }
    case SDL_MOUSEMOTION:
        if (menu_open_ && over_ui(mouse_x_, mouse_y_)) return;
        window_to_remote(mouse_x_, mouse_y_, ptr_x_, ptr_y_);
        ptr_dirty_ = true;
        return;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        const bool down = e.type == SDL_MOUSEBUTTONDOWN;
        uint8_t bit = 0;
        switch (e.button.button) {
        case SDL_BUTTON_LEFT: bit = RD_BTN_LEFT; break;
        case SDL_BUTTON_MIDDLE: bit = RD_BTN_MIDDLE; break;
        case SDL_BUTTON_RIGHT: bit = RD_BTN_RIGHT; break;
        case SDL_BUTTON_X1: bit = RD_BTN_X1; break;
        case SDL_BUTTON_X2: bit = RD_BTN_X2; break;
        }
        // Presses on the overlay belong to the UI; releases always reach the
        // host so a drag that ends over the menu doesn't leave a button stuck.
        if (down && over_ui(mouse_x_, mouse_y_)) return;
        if (down && menu_open_) {
            menu_open_ = false;  // click outside the menu closes it
            return;
        }
        if (!down && !(buttons_ & bit)) return;
        window_to_remote(mouse_x_, mouse_y_, ptr_x_, ptr_y_);
        buttons_ = down ? uint8_t(buttons_ | bit) : uint8_t(buttons_ & ~bit);
        send_pointer();
        return;
    }
    case SDL_MOUSEWHEEL: {
        if (menu_open_) return;
        int wy = e.wheel.y, wx = e.wheel.x;
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) wy = -wy, wx = -wx;
        if (wx || wy) send_pointer(wx, wy);
        return;
    }
    default: return;
    }
}

// ------------------------------------------------------------ drawing

void App::toast(const std::string &text, Color c) {
    toasts_.push_back({text, c, SDL_GetTicks()});
    while (toasts_.size() > 6) toasts_.pop_front();
}

void App::update_view() {
    if (!rw_ || !rh_) return;
    if (scale_mode_ == ScaleMode::Fit) {
        view_s_ = std::min(float(out_w_) / rw_, float(out_h_) / rh_);
        view_x_ = (out_w_ - rw_ * view_s_) / 2;
        view_y_ = (out_h_ - rh_ * view_s_) / 2;
    } else {
        // 1:1 pixels; if the remote screen is bigger than the window, pan by
        // following the mouse (edge of window = edge of remote screen).
        view_s_ = 1;
        float mx = std::max(0.f, std::min(1.f, mouse_x_ * scale_ / std::max(1, out_w_)));
        float my = std::max(0.f, std::min(1.f, mouse_y_ * scale_ / std::max(1, out_h_)));
        view_x_ = rw_ <= out_w_ ? (out_w_ - rw_) / 2.f : -std::round(mx * (rw_ - out_w_));
        view_y_ = rh_ <= out_h_ ? (out_h_ - rh_) / 2.f : -std::round(my * (rh_ - out_h_));
    }
}

void App::render() {
    int ww, wh;
    SDL_GetWindowSize(win_, &ww, &wh);
    SDL_GetRendererOutputSize(ren_, &out_w_, &out_h_);
    scale_ = ww > 0 ? float(out_w_) / ww : 1.f;
    ui_.begin(scale_, mouse_x_, mouse_y_, mouse_down_);

    SDL_SetRenderDrawColor(ren_, theme::bg.r, theme::bg.g, theme::bg.b, 255);
    SDL_RenderClear(ren_);
    if (screen_ == Screen::Connect) draw_connect();
    else draw_session();
    draw_toasts();

    ui_.end();
    if (!opts_.screenshot_path.empty() && SDL_GetTicks() >= uint32_t(opts_.screenshot_after * 1000)) {
        SDL_Surface *shot = SDL_CreateRGBSurfaceWithFormat(0, out_w_, out_h_, 32, SDL_PIXELFORMAT_ARGB8888);
        if (shot && SDL_RenderReadPixels(ren_, nullptr, SDL_PIXELFORMAT_ARGB8888, shot->pixels, shot->pitch) == 0 &&
            SDL_SaveBMP(shot, opts_.screenshot_path.c_str()) == 0)
            SDL_Log("saved %s", opts_.screenshot_path.c_str());
        SDL_FreeSurface(shot);
        opts_.screenshot_path.clear();
        running_ = false;
    }
    SDL_RenderPresent(ren_);
}

void App::draw_connect() {
    const float W = out_w_ / scale_, H = out_h_ / scale_;
    // Subtle backdrop grid.
    for (float x = 0; x < W; x += 48) ui_.fill({x, 0, 1, H}, {255, 255, 255, 6});
    for (float y = 0; y < H; y += 48) ui_.fill({0, y, W, 1}, {255, 255, 255, 6});

    const float cw = std::min(420.f, W - 32), ch = 440;
    const Rect card{(W - cw) / 2, std::max(16.f, (H - ch) / 2), cw, ch};
    ui_.fill({card.x, card.y + 6, card.w, card.h}, {0, 0, 0, 90}, 14);
    ui_.fill(card, theme::panel, 14);
    ui_.outline(card, theme::panel_border, 14);

    float y = card.y + 28;
    // Logo: two linked squares.
    ui_.fill({card.x + 28, y + 4, 22, 22}, theme::accent, 5);
    ui_.fill({card.x + 40, y + 14, 22, 22}, {98, 163, 255, 160}, 5);
    ui_.text(card.x + 76, y, "TetherDesk", theme::text, 1.6f);
    ui_.text(card.x + 76, y + 26, is_web() ? "Remote desktop - web viewer" : "Remote desktop viewer", theme::dim);
    y += 76;

    const char *labels[4] = {"Host", "Port", "Your name", "Password"};
    std::string *vals[4] = {&f_host_, &f_port_, &f_name_, &f_password_};
    const float fx = card.x + 28, fw = card.w - 56;
    Rect rects[4] = {{fx, y, fw * 0.68f, 36}, {fx + fw * 0.72f, y, fw * 0.28f, 36}, {fx, y + 64, fw, 36}, {fx, y + 128, fw, 36}};
    for (int i = 0; i < 4; i++) {
        ui_.field(rects[i], labels[i], *vals[i], focus_ == i && phase_ == Phase::None, i == 3);
        if (ui_.clicked(rects[i])) focus_ = i;
    }
    y += 184;

    const bool busy = phase_ != Phase::None || reconnect_at_;
    Rect btn{fx, y, fw, 40};
    if (busy) {
        uint32_t dots = (SDL_GetTicks() / 400) % 4;
        const char *what = phase_ == Phase::Auth ? "Authenticating" : "Connecting";
        if (ui_.button(btn, std::string(what) + std::string(dots, '.') + "   (Esc to cancel)", false)) disconnect_user();
    } else if (ui_.button(btn, "Connect", true)) {
        start_connect();
    }
    y += 52;
    if (!error_.empty()) ui_.text(fx, y, ellipsize(ui_, error_, fw), theme::bad);
    else ui_.text(fx, y, "Tab to move between fields, Enter to connect", theme::dim);
}

void App::draw_session() {
    update_view();
    if (tex_) {
        SDL_FRect dst = {view_x_, view_y_, rw_ * view_s_, rh_ * view_s_};
        SDL_RenderCopyF(ren_, tex_, nullptr, &dst);
    }
    const float W = out_w_ / scale_, H = out_h_ / scale_;

    // Pull-down tab at the top centre; always reachable with the mouse.
    const bool near_top = mouse_y_ < 48 && mouse_x_ > W / 2 - 160 && mouse_x_ < W / 2 + 160;
    pill_rect_ = {0, 0, 0, 0};
    if ((near_top || !tex_) && !menu_open_) {
        pill_rect_ = {W / 2 - 110, 6, 220, 30};
        ui_.fill(pill_rect_, {22, 27, 36, 225}, 15);
        ui_.text_centered(pill_rect_, "TetherDesk menu  (F8)", theme::text);
        if (ui_.clicked(pill_rect_)) menu_open_ = true;
    }

    if (show_stats_) draw_hud();
    if (menu_open_) draw_menu();
    else menu_rect_ = {0, 0, 0, 0};

    if (chat_open_) {
        Rect bar{16, H - 56, W - 32, 40};
        ui_.fill(bar, {22, 27, 36, 240}, 8);
        ui_.outline(bar, theme::accent, 8);
        float w = ui_.text(bar.x + 12, bar.y + 11, "Say:", theme::dim);
        std::string shown = chat_text_;
        while (!shown.empty() && ui_.text_width(shown) > bar.w - w - 40) shown.erase(0, 1);
        float tw = ui_.text(bar.x + 20 + w, bar.y + 11, shown, theme::text);
        if ((SDL_GetTicks() / 530) % 2 == 0) ui_.fill({bar.x + 21 + w + tw, bar.y + 12, 1.5f, 17}, theme::text);
    }

    // Upload progress.
    float uy = H - (chat_open_ ? 104 : 56);
    for (auto it = uploads_.rbegin(); it != uploads_.rend(); ++it) {
        Rect box{W - 316, uy, 300, 40};
        ui_.fill(box, {22, 27, 36, 235}, 8);
        float frac = it->size ? float(it->sent) / float(it->size) : 1.f;
        ui_.fill({box.x + 10, box.y + 30, (box.w - 20) * frac, 3}, theme::accent);
        ui_.text(box.x + 10, box.y + 7, ellipsize(ui_, it->name, 200), theme::text);
        std::string pct = it->done_sending ? "saving" : std::to_string(int(frac * 100)) + "%";
        ui_.text(box.x + box.w - 10 - ui_.text_width(pct), box.y + 7, pct, theme::dim);
        uy -= 48;
    }

    if (phase_ != Phase::Live) {
        ui_.fill({0, 0, W, H}, {0, 0, 0, 150});
        Rect box{W / 2 - 170, H / 2 - 40, 340, 80};
        ui_.fill(box, theme::panel, 12);
        ui_.text_centered({box.x, box.y + 12, box.w, 24}, "Reconnecting...", theme::text, 1.2f);
        ui_.text_centered({box.x, box.y + 44, box.w, 20},
                          "attempt " + std::to_string(reconnect_attempts_) + " of 10 - Esc to give up", theme::dim);
    }
}

void App::draw_hud() {
    char l1[128], l2[128];
    std::snprintf(l1, sizeof l1, "%d fps   %.1f Mbit/s   %s", fps_shown_, mbps_,
                  rtt_ms_ >= 0 ? (std::to_string(int(rtt_ms_ + 0.5)) + " ms").c_str() : "- ms");
    std::snprintf(l2, sizeof l2, "%dx%d   %s   %d tiles", rw_, rh_,
                  quality_ == 255 ? "auto quality" : kQualityNames[quality_], tiles_last_);
    const float w = std::max(ui_.text_width(l1), ui_.text_width(l2)) + 24;
    Rect box{12, 12, w, 54};
    ui_.fill(box, {0, 0, 0, 170}, 8);
    ui_.text(box.x + 12, box.y + 8, l1, theme::text);
    ui_.text(box.x + 12, box.y + 28, l2, theme::dim);
}

void App::draw_menu() {
    const float W = out_w_ / scale_, H = out_h_ / scale_;
    const float mw = std::min(kMenuW, W - 24);
    const float x = (W - mw) / 2, pad = 18, bx = x + 120, bw_avail = mw - 120 - pad;
    float y = 10;
    // Layout is computed on the fly; the panel is drawn first using last
    // frame's height so buttons appear on top of it.
    static float last_h = 420;
    menu_rect_ = {x, y, mw, last_h};
    ui_.fill({x, y + 4, mw, last_h}, {0, 0, 0, 90}, 12);
    ui_.fill(menu_rect_, {22, 27, 36, 246}, 12);
    ui_.outline(menu_rect_, theme::panel_border, 12);

    y += 16;
    ui_.text(x + pad, y, ellipsize(ui_, host_name_.empty() ? "Remote host" : host_name_, mw - 160), theme::text, 1.25f);
    if (ui_.button({x + mw - pad - 30, y - 2, 30, 26}, "x")) menu_open_ = false;
    y += 26;
    std::string sub = host_os_ + "  -  " + std::to_string(rw_) + "x" + std::to_string(rh_) + "  -  " +
                      (view_only_ ? "view only" : "you have control");
    ui_.text(x + pad, y, ellipsize(ui_, sub, mw - 2 * pad), view_only_ ? theme::warn : theme::dim);
    y += 32;

    auto row_label = [&](const char *label) { ui_.text(x + pad, y + 6, label, theme::dim); };
    auto buttons_row = [&](const std::vector<std::string> &labels, int active, auto on_click) {
        float bw = std::min(96.f, (bw_avail - 6 * (labels.size() - 1)) / labels.size());
        for (size_t i = 0; i < labels.size(); i++) {
            Rect r{bx + i * (bw + 6), y, bw, 28};
            if (ui_.button(r, ellipsize(ui_, labels[i], bw - 10), int(i) == active)) on_click(int(i));
        }
        y += 36;
    };

    row_label("Quality");
    buttons_row({"Auto", "Lossless", "High", "Medium", "Low"}, quality_ == 255 ? 0 : quality_ + 1, [&](int i) {
        quality_ = i == 0 ? 255 : i - 1;
        send_settings();
    });
    row_label("Frame rate");
    static const int kFps[] = {5, 15, 30, 60};
    int fps_idx = int(std::find(std::begin(kFps), std::end(kFps), fps_) - std::begin(kFps));
    buttons_row({"5", "15", "30", "60"}, fps_idx, [&](int i) {
        fps_ = kFps[i];
        send_settings();
    });
    if (displays_.size() > 1) {
        row_label("Display");
        std::vector<std::string> names;
        for (size_t i = 0; i < displays_.size(); i++) names.push_back(std::to_string(i + 1) + ". " + displays_[i].name);
        buttons_row(names, cur_display_, [&](int i) {
            if (i != cur_display_) {
                cur_display_ = i;
                send_settings();
            }
        });
    }
    row_label("View");
    {
        const float bw = 96;
        if (ui_.button({bx, y, bw, 28}, "Fit", scale_mode_ == ScaleMode::Fit)) scale_mode_ = ScaleMode::Fit;
        if (ui_.button({bx + bw + 6, y, bw, 28}, "1:1 pixels", scale_mode_ == ScaleMode::Native))
            scale_mode_ = ScaleMode::Native;
        if (ui_.button({bx + 2 * (bw + 6), y, bw, 28}, "Stats", show_stats_)) show_stats_ = !show_stats_;
        if (!is_web()) {
            bool fs = SDL_GetWindowFlags(win_) & SDL_WINDOW_FULLSCREEN_DESKTOP;
            if (ui_.button({bx + 3 * (bw + 6), y, bw, 28}, "Fullscreen", fs))
                SDL_SetWindowFullscreen(win_, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        }
        y += 36;
    }
    row_label("Actions");
    {
        const float bw = (bw_avail - 12) / 3;
        if (ui_.button({bx, y, bw, 28}, "Ctrl+Alt+Del", false, !view_only_)) {
            for (uint16_t k : {RD_HID_LCTRL, RD_HID_LALT, RD_HID_DELETE}) send_key(k, true);
            for (uint16_t k : {RD_HID_DELETE, RD_HID_LALT, RD_HID_LCTRL}) send_key(k, false);
        }
        if (ui_.button({bx + bw + 6, y, bw, 28}, "Refresh")) {
            rd_buf msg;
            rd_buf_init(&msg);
            rd_buf_put_u8(&msg, RD_C_REFRESH);
            send(msg);
            rd_buf_free(&msg);
        }
        if (ui_.button({bx + 2 * (bw + 6), y, bw, 28}, "Chat  (F9)")) {
            release_input();
            chat_open_ = true;
            menu_open_ = false;
            SDL_StartTextInput();
        }
        y += 34;
        if (!is_web() && ui_.button({bx, y, bw, 28}, "Send clipboard", false, !view_only_)) {
            char *clip = SDL_GetClipboardText();
            if (clip && *clip) {
                send_clipboard(clip);
                toast("Clipboard sent to host", theme::dim);
            }
            SDL_free(clip);
        }
        if (ui_.button({bx + (is_web() ? 0 : bw + 6), y, bw, 28}, "Disconnect")) disconnect_user();
        y += 40;
    }

    ui_.fill({x + pad, y, mw - 2 * pad, 1}, theme::panel_border);
    y += 12;
    ui_.text(x + pad, y, "Viewers (" + std::to_string(viewers_.size()) + ")", theme::dim);
    y += 22;
    for (auto &v : viewers_) {
        ui_.fill({x + pad + 2, y + 6, 8, 8}, v.view_only ? theme::warn : theme::good, 4);
        ui_.text(x + pad + 18, y, ellipsize(ui_, v.name + "   " + v.addr + (v.view_only ? "   view only" : ""), mw - 60),
                 theme::text);
        y += 20;
    }
    y += 10;
    ui_.text(x + pad, y,
             is_web() ? "F8 menu  -  F9 chat" : "F8 menu  -  F9 chat  -  F11 fullscreen  -  drop files to send them",
             theme::dim);
    y += 30;
    last_h = std::min(y - 10, H - 20);
}

void App::draw_toasts() {
    const float H = out_h_ / scale_;
    const uint32_t now = SDL_GetTicks();
    while (!toasts_.empty() && now - toasts_.front().t0 > 7000) toasts_.pop_front();
    float y = H - (chat_open_ ? 72 : 16);
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        uint32_t age = now - it->t0;
        uint8_t a = age > 6000 ? uint8_t(255 * (7000 - age) / 1000) : 255;
        std::string t = ellipsize(ui_, it->text, std::min(640.f, out_w_ / scale_ - 64));
        float w = ui_.text_width(t) + 24;
        y -= 34;
        ui_.fill({16, y, w, 28}, {10, 13, 18, uint8_t(a * 0.85f)}, 7);
        ui_.text(28, y + 5, t, {it->color.r, it->color.g, it->color.b, a});
    }
}

}  // namespace td
