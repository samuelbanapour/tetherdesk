#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>

#include "rd_codec.h"
#include "rd_crypto.h"
#include "rd_net.h"
#include "rd_proto.h"

namespace td {

namespace {

constexpr float kMenuW = 580;
const char *kQualityNames[] = {"Lossless", "High", "Medium", "Low"};

std::string ellipsize(const Ui &ui, std::string s, float max_w, float size = 1.0f) {
    if (ui.text_width(s, size) <= max_w) return s;
    while (!s.empty() && ui.text_width(s + "...", size) > max_w) s.pop_back();
    return s + "...";
}

bool is_web() {
#ifdef __EMSCRIPTEN__
    return true;
#else
    return false;
#endif
}

std::string relative_time(int64_t t) {
    if (!t) return "never connected";
    int64_t d = int64_t(std::time(nullptr)) - t;
    if (d < 60) return "just now";
    if (d < 3600) return std::to_string(d / 60) + " min ago";
    if (d < 86400) return std::to_string(d / 3600) + " h ago";
    return std::to_string(d / 86400) + " days ago";
}

// "728 857 467", "728-857-467" or "728857467" -> "728857467"; else "".
std::string relay_id_of(const std::string &s) {
    std::string d;
    for (char c : s) {
        if (c >= '0' && c <= '9') d += c;
        else if (c != ' ' && c != '-') return "";
    }
    return d.size() == 9 ? d : "";
}

std::string pretty_id(const std::string &id) {
    return id.size() == 9 ? id.substr(0, 3) + " " + id.substr(3, 3) + " " + id.substr(6) : id;
}

std::string display_address(const std::string &host, int port) {
    std::string id = relay_id_of(host);
    if (!id.empty()) return "ID " + pretty_id(id);
    return host + (port != RD_DEFAULT_PORT ? ":" + std::to_string(port) : "");
}

void pop_utf8(std::string &s) {
    if (s.empty()) return;
    do s.pop_back();
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80);
}

}  // namespace

App::App(SDL_Window *window, SDL_Renderer *renderer, Options opts)
    : win_(window), ren_(renderer), opts_(std::move(opts)) {
    ui_.init(ren_);
    rd_buf_init(&sealed_);
    store_.load();
    transport_ = Transport::create();
    show_stats_ = opts_.show_stats;
    f_host_ = opts_.web_relay ? opts_.connect_id : opts_.host;
    f_port_ = std::to_string(opts_.port);
    f_name_ = opts_.name;
    f_password_ = opts_.password;
    focus_ = (is_web() && f_password_.empty()) ? 3 : 0;
    SDL_StartTextInput();

    if (opts_.start_sharing && !is_web()) {
        tab_ = HomeTab::Share;
        start_sharing();
    }
    if (opts_.autoconnect) {
        SavedPc pc;
        pc.label = opts_.host;
        pc.host = opts_.host;
        pc.port = opts_.port;
        pc.user_name = opts_.name;
        pc.fullscreen = opts_.fullscreen;
        // Reuse a matching saved PC (keeps its id, thumbnail and settings).
        for (auto &s : store_.pcs)
            if (s.host == pc.host && s.port == pc.port) pc = s;
        if (!opts_.password.empty()) pc.password = opts_.password;
        if (opts_.fullscreen) pc.fullscreen = true;
        begin_connect(pc);
    }
}

App::~App() {
    if (transport_) transport_->close();
    for (auto &u : uploads_)
        if (u.file) std::fclose(u.file);
    for (auto &t : thumbs_)
        if (t.second) SDL_DestroyTexture(t.second);
    if (tex_) SDL_DestroyTexture(tex_);
    rd_buf_free(&sealed_);
    rd_wipe(&ch_, sizeof ch_);
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
            rd_buf_put_str(&msg, target_.user_name.empty() ? "Viewer" : target_.user_name.c_str());
            rd_buf_put(&msg, ce_, 32);
            send(msg);
            rd_buf_free(&msg);
            phase_ = Phase::Hello;
        }
        if (phase_ != Phase::None && transport_->state() == Transport::State::Closed) on_transport_closed();
    } else if (reconnect_at_ && now >= reconnect_at_) {
        reconnect_at_ = 0;
        start_connect();
    }

    if (!is_web() && now - share_log_read_ >= 500) {
        share_log_read_ = now;
        poll_share_log();
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
        if (now - last_thumb_ >= 30000) save_thumbnail();
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

void App::begin_connect(const SavedPc &pc) {
    target_ = pc;
    if (target_.user_name.empty()) target_.user_name = opts_.name;
    if (target_.password.empty()) {
        pw_input_.clear();
        pw_remember_ = pc.remember;
        dialog_error_.clear();
        dialog_ = Dialog::Password;
        focus_ = 0;
        return;
    }
    start_connect();
}

void App::start_connect() {
    error_.clear();
    dialog_error_.clear();
    if (target_.host.empty()) return fail_connect("Enter the computer's ID or address");
    const std::string id = relay_id_of(target_.host);
    if (!id.empty()) target_.host = id;  // store IDs in one canonical form
    else if (target_.port <= 0 || target_.port > 65535) return fail_connect("Port must be 1-65535");
    user_closed_ = false;
    phase_ = Phase::Connecting;
    rd_wipe(&ch_, sizeof ch_);
    rd_x25519_keypair(ce_priv_, ce_);
    if (screen_ != Screen::Session) dialog_ = Dialog::Connecting;
    if (!id.empty()) {
        // Through the internet relay: both computers only connect outwards.
        if (is_web()) {
            if (opts_.web_relay) transport_->connect(opts_.web_host, opts_.web_port, "/v/" + id, opts_.web_secure);
            else transport_->connect(opts_.relay.substr(0, opts_.relay.rfind(':')), 443, "/v/" + id, true);
        } else {
            size_t c = opts_.relay.rfind(':');
            std::string rh = c == std::string::npos ? opts_.relay : opts_.relay.substr(0, c);
            int rp = c == std::string::npos ? 443 : std::atoi(opts_.relay.substr(c + 1).c_str());
            transport_->connect(rh, rp, "/v/" + id, rp == 443);  // TLS to the relay on 443
        }
    } else {
        transport_->connect(target_.host, target_.port);
    }
}

void App::send_auth() {
    uint8_t mac[32];
    rd_hs_auth_mac(mac, target_.password.data(), target_.password.size(), transcript_);
    rd_buf msg;
    rd_buf_init(&msg);
    rd_buf_put_u8(&msg, RD_C_AUTH);
    rd_buf_put(&msg, mac, sizeof mac);
    send(msg);
    rd_buf_free(&msg);
    rd_wipe(mac, sizeof mac);
    phase_ = Phase::Auth;
    if (dialog_ == Dialog::Verify) dialog_ = screen_ == Screen::Session ? Dialog::None : Dialog::Connecting;
}

void App::fail_connect(const std::string &why) {
    phase_ = Phase::None;
    user_closed_ = true;
    transport_->close();
    if (is_web()) {
        error_ = why;
        dialog_ = Dialog::None;
    } else {
        dialog_error_ = why;
        dialog_ = Dialog::Connecting;
    }
}

void App::set_fullscreen(bool on) {
    if (is_web()) return;
    SDL_SetWindowFullscreen(win_, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

bool App::fullscreen() const { return SDL_GetWindowFlags(win_) & SDL_WINDOW_FULLSCREEN_DESKTOP; }

void App::disconnect_user() {
    if (phase_ == Phase::Live) save_thumbnail();
    user_closed_ = true;
    release_input();
    transport_->close();
    phase_ = Phase::None;
    reconnect_at_ = 0;
    reconnect_attempts_ = 0;
    screen_ = Screen::Home;
    dialog_ = Dialog::None;
    menu_open_ = chat_open_ = false;
    for (auto &u : uploads_)
        if (u.file) std::fclose(u.file);
    uploads_.clear();
    set_fullscreen(false);
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
    if (screen_ == Screen::Session) {
        screen_ = Screen::Home;
        set_fullscreen(false);
    }
    menu_open_ = chat_open_ = false;
    SDL_SetWindowTitle(win_, "TetherDesk");
    SDL_StartTextInput();
    if (!user_closed_) fail_connect(why.empty() ? "Disconnected" : why);
}

void App::send(const rd_buf &msg) {
    if (!ch_.active) {
        transport_->send(msg.data, msg.len);
        return;
    }
    rd_buf_clear(&sealed_);
    rd_channel_seal(&ch_, msg.data, msg.len, &sealed_);
    transport_->send(sealed_.data, sealed_.len);
}

void App::send_simple(uint8_t type) {
    rd_buf msg;
    rd_buf_init(&msg);
    rd_buf_put_u8(&msg, type);
    send(msg);
    rd_buf_free(&msg);
}

void App::handle_message(std::vector<uint8_t> &m) {
    if (ch_.active) {
        plain_.resize(m.size());
        long pl = rd_channel_open(&ch_, m.data(), m.size(), plain_.data());
        if (pl < 0) return fail_connect("Decryption failed - the connection may have been tampered with");
        plain_.resize(size_t(pl));
        m.swap(plain_);
    }
    rd_reader r;
    rd_reader_init(&r, m.data(), m.size());
    const uint8_t type = rd_get_u8(&r);

    switch (type) {
    case RD_S_HELLO: {
        uint16_t version = rd_get_u16(&r);
        uint8_t method = rd_get_u8(&r);
        const uint8_t *nonce = rd_get_bytes(&r, RD_NONCE_LEN);
        char name[256], os[256];
        rd_get_str(&r, name, sizeof name);
        rd_get_str(&r, os, sizeof os);
        const uint8_t *ss = rd_get_bytes(&r, 32), *se = rd_get_bytes(&r, 32);
        if (r.err || version != RD_PROTO_VERSION || method != RD_AUTH_X25519_CHACHA || phase_ != Phase::Hello)
            return fail_connect(version != RD_PROTO_VERSION ? "The host runs an incompatible TetherDesk version"
                                                            : "Unexpected handshake from host");
        uint8_t dh1[32], dh2[32];
        if (rd_x25519(dh1, ce_priv_, se) != 0 || rd_x25519(dh2, ce_priv_, ss) != 0)
            return fail_connect("The host sent an invalid key");
        rd_hs_transcript(transcript_, ce_, ss, se, nonce);
        rd_hs_keys(&ch_, dh1, dh2, transcript_, 0);
        rd_wipe(dh1, sizeof dh1);
        rd_wipe(dh2, sizeof dh2);
        rd_wipe(ce_priv_, sizeof ce_priv_);
        host_name_ = name;
        host_os_ = os;
        char fp[40];
        rd_fingerprint(ss, fp);
        host_fp_ = fp;

        // Trust on first use, like SSH. The web viewer was served by this very
        // host, so it has nothing better to compare against.
        verify_old_fp_ = store_.known_fingerprint(target_.host, target_.port);
        if (is_web() || opts_.trust_new_hosts || verify_old_fp_ == host_fp_) {
            if (!is_web() && verify_old_fp_.empty()) {
                store_.trust(target_.host, target_.port, host_fp_);
                store_.save();
            }
            send_auth();
        } else {
            phase_ = Phase::Verify;
            dialog_ = Dialog::Verify;
        }
        break;
    }
    case RD_S_AUTH_RESULT: {
        bool ok = rd_get_u8(&r) != 0;
        bool vo = rd_get_u8(&r) != 0;
        char text[512];
        rd_get_str(&r, text, sizeof text);
        if (!ok) {
            reconnect_attempts_ = 0;
            if (screen_ == Screen::Session) {
                screen_ = Screen::Home;
                set_fullscreen(false);
            }
            if (std::strstr(text, "password") && !is_web()) {
                // Ask again; forget a remembered password that no longer works.
                if (SavedPc *pc = store_.find(target_.id); pc && pc->remember) {
                    pc->password.clear();
                    store_.save();
                }
                fail_connect(text);
                target_.password.clear();
                pw_input_.clear();
                dialog_error_ = text;
                dialog_ = Dialog::Password;
            } else {
                fail_connect(text);
            }
            SDL_StartTextInput();
            break;
        }
        view_only_ = vo;
        if (phase_ == Phase::Auth) {
            phase_ = Phase::Live;
            dialog_ = Dialog::None;
            const bool first = screen_ != Screen::Session;
            screen_ = Screen::Session;
            if (reconnect_attempts_) toast("Reconnected", theme::good);
            else toast(std::string(text) + " to " + host_name_ + (is_web() ? " - F8 for the menu" : ""), theme::good);
            reconnect_attempts_ = 0;
            stats_t0_ = last_ping_ = last_clip_check_ = SDL_GetTicks();
            last_thumb_ = SDL_GetTicks() - 25000;  // first thumbnail a few seconds in
            char *clip = SDL_HasClipboardText() ? SDL_GetClipboardText() : nullptr;
            last_local_clip_ = clip ? clip : "";
            SDL_free(clip);
            SDL_StopTextInput();
            send_settings();
            if (opts_.open_menu) menu_open_ = true;
            if (first) {
                bar_until_ = SDL_GetTicks() + 4000;
                if (!is_web()) {
                    // Remember this PC (quick connects are saved automatically).
                    SavedPc pc = target_;
                    if (pc.id.empty()) pc.id = store_.new_id(), pc.label = pc.host;
                    if (!pc.remember) pc.password.clear();
                    pc.last_used = int64_t(std::time(nullptr));
                    store_.upsert(pc);
                    store_.save();
                    target_.id = pc.id;
                    target_.label = pc.label;
                    if (target_.fullscreen) set_fullscreen(true);
                }
            }
            SDL_SetWindowTitle(win_, ("TetherDesk - " + (target_.label.empty() ? host_name_ : target_.label)).c_str());
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
        if (rd_remaining(&r)) {
            uint8_t flags = rd_get_u8(&r);
            hires_supported_ = flags & 1;
            hires_on_ = flags & 2;
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
    rd_buf_free(&ack);
    if (bad && SDL_GetTicks() - last_refresh_req_ > 1000) {
        last_refresh_req_ = SDL_GetTicks();
        send_simple(RD_C_REFRESH);
    }
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
    rd_buf_put_u8(&msg, uint8_t(resolution_pref_));
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

// ------------------------------------------------------------ thumbnails

void App::save_thumbnail() {
    last_thumb_ = SDL_GetTicks();
    if (!store_.persistent() || target_.id.empty() || !rw_ || fb_.empty()) return;
    SDL_Surface *src = SDL_CreateRGBSurfaceWithFormatFrom(fb_.data(), rw_, rh_, 32, rw_ * 4, SDL_PIXELFORMAT_ARGB8888);
    const int tw = 400, th = std::max(1, 400 * rh_ / rw_);
    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, tw, th, 32, SDL_PIXELFORMAT_ARGB8888);
    if (src && dst) {
        SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
        SDL_BlitScaled(src, nullptr, dst, nullptr);
        SDL_SaveBMP(dst, store_.thumbnail_path(target_.id).c_str());
        auto it = thumbs_.find(target_.id);
        if (it != thumbs_.end()) {
            if (it->second) SDL_DestroyTexture(it->second);
            thumbs_.erase(it);
        }
    }
    SDL_FreeSurface(src);
    SDL_FreeSurface(dst);
}

SDL_Texture *App::thumbnail(const std::string &id) {
    auto it = thumbs_.find(id);
    if (it != thumbs_.end()) return it->second;
    SDL_Texture *t = nullptr;
    if (SDL_Surface *s = SDL_LoadBMP(store_.thumbnail_path(id).c_str())) {
        t = SDL_CreateTextureFromSurface(ren_, s);
        SDL_FreeSurface(s);
    }
    thumbs_[id] = t;
    return t;
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
    case SDL_QUIT:
        if (phase_ == Phase::Live) save_thumbnail();
        running_ = false;
        return;
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
    if (screen_ == Screen::Session && dialog_ == Dialog::None) handle_session_event(e);
    else handle_form_event(e);
}

void App::handle_form_event(const SDL_Event &e) {
    if (e.type == SDL_MOUSEWHEEL && screen_ == Screen::Home && dialog_ == Dialog::None) {
        home_scroll_ = std::max(0.f, home_scroll_ - e.wheel.y * 40.f);
        return;
    }
    Field *f = focus_ >= 0 && focus_ < int(last_form_.size()) ? &last_form_[size_t(focus_)] : nullptr;
    if (e.type == SDL_TEXTINPUT && f) {
        if (f->value->size() < 200) *f->value += e.text.text;
        if (f->digits)
            f->value->erase(std::remove_if(f->value->begin(), f->value->end(), [](char c) { return c < '0' || c > '9'; }),
                            f->value->end());
    } else if (e.type == SDL_KEYDOWN) {
        SDL_Keycode k = e.key.keysym.sym;
        bool mod = e.key.keysym.mod & (KMOD_CTRL | KMOD_GUI);
        const int n = int(last_form_.size());
        if (k == SDLK_TAB && n) focus_ = (focus_ + ((e.key.keysym.mod & KMOD_SHIFT) ? n - 1 : 1)) % n;
        else if (k == SDLK_BACKSPACE && f) pop_utf8(*f->value);
        else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) submit_ = true;
        else if (k == SDLK_ESCAPE) cancel_ = true;
        else if (mod && k == SDLK_v && f) {
            if (char *clip = SDL_GetClipboardText()) {
                for (char *p = clip; *p; p++)
                    if (*p != '\n' && *p != '\r') *f->value += *p;
                SDL_free(clip);
            }
        }
    }
}

bool App::over_ui(float x, float y) const {
    if (menu_open_ && menu_rect_.contains(x, y)) return true;
    return bar_rect_.w > 0 && bar_rect_.contains(x, y);
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
            } else if (k == SDLK_BACKSPACE) {
                pop_utf8(chat_text_);
            }
            return;
        }
        if (k == SDLK_F8) {
            if (down) menu_open_ = !menu_open_;
            return;
        }
        if (k == SDLK_F7) {
            if (down && displays_.size() > 1) switch_screen((cur_display_ + 1) % int(displays_.size()));
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
            if (down) set_fullscreen(!fullscreen());
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
        if (over_ui(mouse_x_, mouse_y_)) return;
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
        // host so a drag that ends over the bar doesn't leave a button stuck.
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

void App::switch_screen(int index) {
    if (index < 0 || index >= int(displays_.size())) return;
    if (view_only_) {
        toast("View-only viewers can't switch screens", theme::warn);
        return;
    }
    cur_display_ = index;
    release_input();  // don't carry a held button/key across screens
    send_settings();
    const DisplayEntry &d = displays_[size_t(index)];
    toast("Screen " + std::to_string(index + 1) + " of " + std::to_string(displays_.size()) + ": " + d.name + "  (" +
              std::to_string(d.w) + "x" + std::to_string(d.h) + ")",
          theme::dim);
}

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
    form_.clear();

    SDL_SetRenderDrawColor(ren_, theme::bg.r, theme::bg.g, theme::bg.b, 255);
    SDL_RenderClear(ren_);
    if (screen_ == Screen::Session) draw_session();
    else if (is_web()) draw_web_connect();
    else draw_home();
    if (dialog_ != Dialog::None) draw_dialog();
    draw_toasts();

    last_form_ = form_;
    if (focus_ >= int(last_form_.size())) focus_ = 0;
    submit_ = cancel_ = false;
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

void App::form_field(const Rect &r, const std::string &label, std::string &value, bool secret, bool digits) {
    const int index = int(form_.size());
    form_.push_back({&value, secret, digits});
    ui_.field(r, label, value, focus_ == index, secret);
    if (ui_.clicked(r)) focus_ = index;
}

static void draw_logo(Ui &ui, float x, float y, float s) {
    ui.fill({x, y + 4 * s, 22 * s, 22 * s}, theme::accent, 5 * s);
    ui.fill({x + 12 * s, y + 14 * s, 22 * s, 22 * s}, {98, 163, 255, 160}, 5 * s);
}

void App::draw_home() {
    const float W = out_w_ / scale_, H = out_h_ / scale_;
    const bool modal = dialog_ != Dialog::None;
    const float pad = std::max(24.f, std::min(56.f, W * 0.05f));

    // Header.
    ui_.fill({0, 0, W, 72}, {17, 21, 28});
    ui_.fill({0, 72, W, 1}, theme::panel_border);
    draw_logo(ui_, pad, 16, 1);
    ui_.text(pad + 46, 16, "TetherDesk", theme::text, 1.45f);
    ui_.text(pad + 46, 42, "Remote Desktop", theme::dim);
    if (opts_.quick_support) {
        draw_share(96);
        return;
    }
    // Tabs, Remote Desktop style: connect out, or let others connect in.
    {
        const float tw = 150, tx = std::max(pad + 230, W / 2 - tw);
        if (ui_.button({tx, 20, tw - 4, 34}, "Connect to a PC", tab_ == HomeTab::Connect) && !modal)
            tab_ = HomeTab::Connect;
        std::string share_label = sharing_ ? "Share this PC  (on)" : "Share this PC";
        if (ui_.button({tx + tw, 20, tw + 20, 34}, share_label, tab_ == HomeTab::Share) && !modal) tab_ = HomeTab::Share;
    }
    if (tab_ == HomeTab::Share) {
        draw_share(96);
        return;
    }
    Rect add{W - pad - 120, 20, 120, 34};
    if (ui_.button(add, "+  Add PC", true) && !modal) {
        edit_ = SavedPc();
        edit_.user_name = opts_.name;
        edit_is_new_ = true;
        edit_port_ = std::to_string(edit_.port);
        dialog_error_.clear();
        dialog_ = Dialog::EditPc;
        focus_ = 0;
    }

    // Quick connect.
    float y = 96;
    const float qw = std::min(560.f, W - 2 * pad - 130);
    if (!modal) {
        form_field({pad, y + 22, qw, 38}, "Quick connect - computer ID (e.g. 728 857 467) or address", quick_host_);
    } else {
        ui_.field({pad, y + 22, qw, 38}, "Quick connect - computer ID (e.g. 728 857 467) or address", quick_host_, false,
                  false);
    }
    bool go = ui_.button({pad + qw + 10, y + 22, 110, 38}, "Connect", !quick_host_.empty(), !quick_host_.empty());
    if (!modal && submit_ && focus_ == 0 && !quick_host_.empty()) go = true;
    if (go && !modal) {
        SavedPc pc;
        std::string h = quick_host_;
        size_t colon = h.rfind(':');
        pc.port = RD_DEFAULT_PORT;
        if (!relay_id_of(h).empty()) {
            h = relay_id_of(h);
        } else if (colon != std::string::npos && h.find(':') == colon) {
            pc.port = std::atoi(h.substr(colon + 1).c_str());
            h = h.substr(0, colon);
        }
        pc.host = h;
        pc.label = relay_id_of(h).empty() ? h : "Computer " + pretty_id(h);
        pc.user_name = opts_.name;
        for (auto &s : store_.pcs)
            if (s.host == pc.host && s.port == pc.port) pc = s;
        begin_connect(pc);
    }
    y += 90;

    // Saved PCs grid.
    ui_.text(pad, y, "Saved PCs", theme::text, 1.15f);
    ui_.text(pad + ui_.text_width("Saved PCs", 1.15f) + 12, y + 3,
             std::to_string(store_.pcs.size()) + (store_.pcs.size() == 1 ? " PC" : " PCs"), theme::dim);
    y += 34;
    std::vector<SavedPc *> pcs;
    for (auto &p : store_.pcs) pcs.push_back(&p);
    std::sort(pcs.begin(), pcs.end(), [](SavedPc *a, SavedPc *b) { return a->last_used > b->last_used; });

    if (pcs.empty()) {
        Rect box{pad, y, W - 2 * pad, 150};
        ui_.outline(box, theme::panel_border, 12);
        ui_.text_centered({box.x, box.y + 40, box.w, 24}, "No saved PCs yet", theme::text, 1.1f);
        ui_.text_centered({box.x, box.y + 72, box.w, 20},
                          "Use Quick connect or \"+ Add PC\". PCs you connect to are saved here with a preview.",
                          theme::dim);
        return;
    }

    const float cw = 260, chh = 222, gap = 20;
    const int cols = std::max(1, int((W - 2 * pad + gap) / (cw + gap)));
    const int rows = int((pcs.size() + size_t(cols) - 1) / size_t(cols));
    const float max_scroll = std::max(0.f, y + rows * (chh + gap) + 20 - H);
    home_scroll_ = std::min(home_scroll_, max_scroll);
    const float top = y;
    for (size_t i = 0; i < pcs.size(); i++) {
        SavedPc &pc = *pcs[i];
        const float cx = pad + float(i % size_t(cols)) * (cw + gap);
        const float cy = top + float(i / size_t(cols)) * (chh + gap) - home_scroll_;
        if (cy + chh < 74 || cy > H) continue;
        Rect card{cx, cy, cw, chh};
        const bool hover = !modal && ui_.mouse_over(card) && ui_.mouse_y() > 74;
        ui_.fill({card.x, card.y + 3, card.w, card.h}, {0, 0, 0, 70}, 12);
        ui_.fill(card, hover ? Color{30, 36, 48} : theme::panel, 12);
        ui_.outline(card, hover ? theme::accent : theme::panel_border, 12);

        Rect thumb{cx + 10, cy + 10, cw - 20, 142};
        ui_.fill(thumb, {10, 13, 18}, 8);
        if (SDL_Texture *t = thumbnail(pc.id)) {
            int tw, th;
            SDL_QueryTexture(t, nullptr, nullptr, &tw, &th);
            float s = std::min(thumb.w / tw, thumb.h / th);
            SDL_FRect dst = {(thumb.x + (thumb.w - tw * s) / 2) * scale_, (thumb.y + (thumb.h - th * s) / 2) * scale_,
                             tw * s * scale_, th * s * scale_};
            SDL_RenderCopyF(ren_, t, nullptr, &dst);
        } else {
            // Placeholder: a little monitor.
            const float mx = thumb.x + thumb.w / 2, my = thumb.y + thumb.h / 2;
            ui_.outline({mx - 34, my - 30, 68, 44}, theme::dim, 4);
            ui_.fill({mx - 3, my + 14, 6, 10}, theme::dim);
            ui_.fill({mx - 16, my + 24, 32, 3}, theme::dim, 1);
        }
        ui_.text(cx + 14, cy + 160, ellipsize(ui_, pc.label, cw - 28, 1.1f), theme::text, 1.1f);
        std::string sub = display_address(pc.host, pc.port);
        ui_.text(cx + 14, cy + 184, ellipsize(ui_, sub, cw - 110), theme::dim);
        std::string when = relative_time(pc.last_used);
        ui_.text(cx + cw - 14 - ui_.text_width(when), cy + 184, when, theme::dim);

        if (hover || pending_delete_ == pc.id) {
            Rect edit{thumb.x + thumb.w - 124, thumb.y + 8, 56, 26}, del{thumb.x + thumb.w - 64, thumb.y + 8, 56, 26};
            if (pending_delete_ == pc.id) {
                if (ui_.button({del.x - 70, del.y, 66, 26}, "Keep")) pending_delete_.clear();
                if (ui_.button(del, "Delete", true)) {
                    auto it = thumbs_.find(pc.id);
                    if (it != thumbs_.end()) {
                        if (it->second) SDL_DestroyTexture(it->second);
                        thumbs_.erase(it);
                    }
                    std::string id = pc.id;
                    pending_delete_.clear();
                    store_.remove(id);
                    store_.save();
                    return;  // list changed; redraw next frame
                }
            } else {
                if (ui_.button(edit, "Edit")) {
                    edit_ = pc;
                    edit_is_new_ = false;
                    edit_port_ = std::to_string(edit_.port);
                    dialog_error_.clear();
                    dialog_ = Dialog::EditPc;
                    focus_ = 0;
                }
                if (ui_.button(del, "Remove")) pending_delete_ = pc.id;
            }
        }
        if (!modal && !ui_.consumed_click() && ui_.clicked(card) && ui_.mouse_y() > 74 && pending_delete_.empty())
            begin_connect(pc);
    }
}

// ------------------------------------------------------------ share this PC

static std::string random_share_password() {
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

void App::start_sharing() {
#ifndef __EMSCRIPTEN__
    if (store_.share_password.empty()) {
        store_.share_password = random_share_password();
        store_.save();
    }
    // The host is built into this same executable ("--run-host").
    std::string exe = executable_path();
    share_log_path_ = (store_.dir().empty() ? std::string(".") + "/" : store_.dir()) + "host.log";
    std::vector<std::string> args = {"--run-host", "--password", store_.share_password, "--no-console",
                                     "--relay", opts_.relay};
    if (store_.share_view_only) args.push_back("--view-only");
    if (store_.share_demo || opts_.share_demo) args.push_back("--demo");
    share_error_.clear();
    share_identity_.clear();
    share_id_.clear();
    share_relay_online_ = false;
    share_urls_.clear();
    share_activity_.clear();
    share_needs_screen_perm_ = share_needs_input_perm_ = false;
    std::string err;
    sharing_ = host_.start(exe, args, share_log_path_, err);
    if (!sharing_) share_error_ = err;
#endif
}

void App::stop_sharing() {
#ifndef __EMSCRIPTEN__
    host_.stop();
#endif
    sharing_ = false;
    share_urls_.clear();
}

// The host reports everything on stdout; read its log to show status.
void App::poll_share_log() {
#ifndef __EMSCRIPTEN__
    if (share_log_path_.empty()) return;
    bool alive = host_.running();
    std::ifstream f(share_log_path_);
    std::string line;
    std::vector<std::string> urls, activity;
    std::string error;
    bool screen_perm = false, input_perm = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t p;
        if (line.rfind("  ID ", 0) == 0) {
            std::string v = line.substr(5);
            v.erase(0, v.find_first_not_of(' '));
            share_id_ = relay_id_of(v.substr(0, 11));
            size_t via = line.find("via ");
            if (via != std::string::npos) share_relay_ = line.substr(via + 4, line.find(')', via) - via - 4);
        } else if (line.find("relay: online as") != std::string::npos) {
            share_relay_online_ = true;
        } else if (line.find("relay connection lost") != std::string::npos) {
            share_relay_online_ = false;
        } else if ((p = line.find("Identity")) != std::string::npos && p < 6) {
            std::string v = line.substr(p + 8);
            v.erase(0, v.find_first_not_of(' '));
            share_identity_ = v.substr(0, v.find(' '));
        } else if ((p = line.find("http://")) != std::string::npos && line.find("localhost") == std::string::npos) {
            std::string u = line.substr(p);
            urls.push_back(u.substr(0, u.find_first_of(" \t")));
        } else if (line.size() > 11 && line[0] == '[' && line[9] == ']' && line.find("relay:") == std::string::npos) {
            activity.push_back(line);
        } else if (line.rfind("error:", 0) == 0) {
            error = line.substr(7);
        }
        if (line.find("Screen Recording") != std::string::npos) screen_perm = true;
        if (line.find("Accessibility") != std::string::npos) input_perm = true;
    }
    share_urls_ = urls;
    if (activity.size() > 8) activity.erase(activity.begin(), activity.end() - 8);
    share_activity_ = activity;
    share_needs_screen_perm_ = screen_perm;
    share_needs_input_perm_ = input_perm;
    if (sharing_ && !alive) {
        sharing_ = false;
        share_error_ = error.empty() ? "The host stopped unexpectedly" : error;
    }
#endif
}

void App::draw_share(float top) {
    const float W = out_w_ / scale_;
    const float pad = std::max(24.f, std::min(56.f, W * 0.05f));
    const float cw = std::min(720.f, W - 2 * pad);
    const float x = pad;
    float y = top + 8;
    const bool qs = opts_.quick_support;

    ui_.text(x, y, qs ? "Get help with this computer" : "Share this PC", theme::text, 1.3f);
    y += 34;
    ui_.text(x, y,
             qs ? "Tell the person helping you the ID and password below. Close this app to end the session."
                : "Let someone connect to this computer from anywhere - with the TetherDesk app or a web browser.",
             theme::dim);
    y += 36;

    // Big on/off switch.
    Rect sw{x, y, 64, 32};
    ui_.fill(sw, sharing_ ? theme::good : theme::button, 16);
    ui_.fill({sharing_ ? sw.x + 36 : sw.x + 4, sw.y + 4, 24, 24}, theme::text, 12);
    const bool starting = sharing_ && share_identity_.empty();
    std::string status = !sharing_ ? "Off - nobody can connect"
                         : starting ? "Starting..."
                         : share_relay_online_ ? "On - reachable from anywhere"
                         : share_id_.empty() ? "On - local network only"
                                             : "On - connecting to the internet relay...";
    ui_.text(x + 80, y + 6, status, sharing_ ? theme::good : theme::dim, 1.1f);
    if (ui_.clicked({sw.x, sw.y, 80 + ui_.text_width(status, 1.1f), sw.h})) {
        if (sharing_) stop_sharing();
        else start_sharing();
    }
    y += 50;
    if (!share_error_.empty()) {
        ui_.text(x, y, ellipsize(ui_, share_error_, cw), theme::bad);
        y += 26;
    }

    // Permission guidance (macOS asks the first time).
    if (share_needs_screen_perm_ || share_needs_input_perm_) {
        Rect box{x, y, cw, share_needs_screen_perm_ && share_needs_input_perm_ ? 124.f : 84.f};
        ui_.fill(box, {60, 45, 15, 200}, 10);
        float yy = y + 12;
        if (share_needs_screen_perm_) {
            ui_.text(x + 14, yy, "Allow Screen Recording for TetherDesk, then turn sharing off and on.", theme::warn);
            if (ui_.button({x + cw - 190, yy - 4, 176, 28}, "Open Screen Recording"))
                SDL_OpenURL("x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture");
            yy += 40;
        }
        if (share_needs_input_perm_) {
            ui_.text(x + 14, yy, "Allow Accessibility so the helper can use the mouse and keyboard.", theme::warn);
            if (ui_.button({x + cw - 190, yy - 4, 176, 28}, "Open Accessibility"))
                SDL_OpenURL("x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility");
        }
        y += box.h + 14;
    }

    // The two things to read out: ID and password.
    Rect card{x, y, cw, 150};
    ui_.fill(card, theme::panel, 12);
    ui_.outline(card, theme::panel_border, 12);
    const float half = (cw - 36) / 2;
    ui_.text(x + 18, y + 16, "Your ID", theme::dim);
    ui_.text(x + 18, y + 40, sharing_ && !share_id_.empty() ? pretty_id(share_id_) : "--- --- ---",
             share_relay_online_ ? theme::text : theme::dim, 2.0f);
    ui_.text(x + 18 + half, y + 16, "Password", theme::dim);
    std::string pw = store_.share_password.empty() ? "(appears when on)"
                     : share_show_pw_ || qs        ? store_.share_password
                                                   : std::string(11, '*');
    ui_.text(x + 18 + half, y + 40, pw, theme::text, qs ? 2.0f : 1.6f);
    if (!qs && ui_.button({x + 18 + half, y + 104, 80, 28}, share_show_pw_ ? "Hide" : "Show"))
        share_show_pw_ = !share_show_pw_;
    if (ui_.button({x + 18 + half + (qs ? 0 : 88), y + 104, 124, 28}, "New password")) {
        store_.share_password = random_share_password();
        store_.save();
        share_show_pw_ = true;
        if (sharing_) {  // restart so the new password takes effect
            stop_sharing();
            start_sharing();
        }
    }
    ui_.text(x + 18, y + 110, "Identity " + (share_identity_.empty() ? std::string("-") : share_identity_), theme::dim);
    y += card.h + 16;

    // How the helper connects.
    std::string lan = "-";
    if (!share_urls_.empty()) {
        lan = share_urls_[0].substr(7);
        if (!lan.empty() && lan.back() == '/') lan.pop_back();
        size_t colon = lan.rfind(':');
        if (colon != std::string::npos && lan.substr(colon + 1) == "5980") lan = lan.substr(0, colon);
    }
    ui_.text(x, y, "The helper can connect with the TetherDesk app, or in any web browser at", theme::dim);
    y += 22;
    ui_.text(x, y, "https://" + (share_relay_.empty() ? opts_.relay : share_relay_) + "/", theme::accent_hover, 1.1f);
    y += 28;
    ui_.text(x, y, "On the same network they can also use this computer's address: " + lan, theme::dim);
    y += 36;

    if (!qs) {
        // Options (take effect the next time sharing starts).
        bool vo = store_.share_view_only, demo = store_.share_demo;
        if (ui_.checkbox(x, y, "View only - others can watch but not control", vo) ||
            ui_.checkbox(x, y + 28, "Share a demo desktop instead of this screen (for testing)", demo)) {
            store_.share_view_only = vo;
            store_.share_demo = demo;
            store_.save();
            if (sharing_) {
                stop_sharing();
                start_sharing();
            }
        }
        y += 70;
    }

    ui_.text(x, y, "Activity", theme::dim);
    y += 24;
    if (share_activity_.empty()) ui_.text(x, y, sharing_ ? "Nobody has connected yet" : "-", theme::dim);
    for (auto &a : share_activity_) {
        ui_.text(x, y, ellipsize(ui_, a, cw), theme::text);
        y += 20;
    }
}

void App::draw_web_connect() {
    const float W = out_w_ / scale_, H = out_h_ / scale_;
    for (float x = 0; x < W; x += 48) ui_.fill({x, 0, 1, H}, {255, 255, 255, 6});
    for (float y = 0; y < H; y += 48) ui_.fill({0, y, W, 1}, {255, 255, 255, 6});

    const float cw = std::min(420.f, W - 32), ch = 400;
    const Rect card{(W - cw) / 2, std::max(16.f, (H - ch) / 2), cw, ch};
    ui_.fill({card.x, card.y + 6, card.w, card.h}, {0, 0, 0, 90}, 14);
    ui_.fill(card, theme::panel, 14);
    ui_.outline(card, theme::panel_border, 14);

    float y = card.y + 28;
    draw_logo(ui_, card.x + 28, y, 1);
    ui_.text(card.x + 76, y, "TetherDesk", theme::text, 1.6f);
    ui_.text(card.x + 76, y + 26, "Remote Desktop - web viewer", theme::dim);
    y += 76;

    const float fx = card.x + 28, fw = card.w - 56;
    const bool busy = phase_ != Phase::None || reconnect_at_;
    if (opts_.web_relay) {
        form_field({fx, y, fw, 36}, "Computer ID (shown on the computer you're helping)", f_host_);
    } else {
        form_field({fx, y, fw * 0.68f, 36}, "Computer ID or host", f_host_);
        form_field({fx + fw * 0.72f, y, fw * 0.28f, 36}, "Port", f_port_, false, true);
    }
    form_field({fx, y + 64, fw, 36}, "Your name", f_name_);
    form_field({fx, y + 128, fw, 36}, "Password", f_password_, true);
    y += 184;

    Rect btn{fx, y, fw, 40};
    bool go = false;
    if (busy) {
        uint32_t dots = (SDL_GetTicks() / 400) % 4;
        const char *what = phase_ == Phase::Auth ? "Authenticating" : "Connecting";
        if (ui_.button(btn, std::string(what) + std::string(dots, '.') + "   (Esc to cancel)") || cancel_)
            disconnect_user();
    } else {
        go = ui_.button(btn, "Connect", true) || submit_;
    }
    if (go) {
        SavedPc pc;
        pc.host = f_host_;
        pc.label = f_host_;
        pc.port = std::atoi(f_port_.c_str());
        pc.user_name = f_name_;
        pc.password = f_password_;
        target_ = pc;
        start_connect();
    }
    y += 52;
    if (!error_.empty()) ui_.text(fx, y, ellipsize(ui_, error_, fw), theme::bad);
    else ui_.text(fx, y, "Tab to move between fields, Enter to connect", theme::dim);
}

Rect App::dialog_frame(float w, float h, const std::string &title) {
    const float W = out_w_ / scale_, H = out_h_ / scale_;
    ui_.fill({0, 0, W, H}, {0, 0, 0, 150});
    w = std::min(w, W - 24);
    Rect d{(W - w) / 2, std::max(12.f, (H - h) / 2), w, h};
    ui_.fill({d.x, d.y + 6, d.w, d.h}, {0, 0, 0, 110}, 14);
    ui_.fill(d, theme::panel, 14);
    ui_.outline(d, theme::panel_border, 14);
    ui_.text(d.x + 24, d.y + 20, ellipsize(ui_, title, d.w - 48, 1.3f), theme::text, 1.3f);
    return {d.x + 24, d.y + 60, d.w - 48, d.h - 80};
}

void App::draw_dialog() {
    switch (dialog_) {
    case Dialog::EditPc: {
        Rect c = dialog_frame(460, 470, edit_is_new_ ? "Add PC" : "Edit PC");
        float y = c.y + 18;
        form_field({c.x, y, c.w, 36}, "Display name (optional)", edit_.label);
        y += 62;
        form_field({c.x, y, c.w * 0.7f, 36}, "Computer ID or address", edit_.host);
        form_field({c.x + c.w * 0.74f, y, c.w * 0.26f, 36}, "Port", edit_port_, false, true);
        edit_.port = std::atoi(edit_port_.c_str());
        y += 62;
        form_field({c.x, y, c.w, 36}, "Your name (shown to others on the host)", edit_.user_name);
        y += 62;
        form_field({c.x, y, c.w, 36}, "Password (optional)", edit_.password, true);
        y += 50;
        ui_.checkbox(c.x, y, "Remember password on this computer", edit_.remember);
        ui_.checkbox(c.x, y + 28, "Open sessions full screen", edit_.fullscreen);
        y += 66;
        if (!dialog_error_.empty()) ui_.text(c.x, y - 8, dialog_error_, theme::bad);
        bool save = ui_.button({c.x + c.w - 110, y + 12, 110, 36}, "Save", true) || submit_;
        if (ui_.button({c.x + c.w - 230, y + 12, 110, 36}, "Cancel") || cancel_) dialog_ = Dialog::None;
        else if (save) {
            if (edit_.host.empty()) dialog_error_ = "Enter the PC's address";
            else if (edit_.port <= 0 || edit_.port > 65535) dialog_error_ = "Port must be 1-65535";
            else {
                if (edit_.label.empty()) edit_.label = edit_.host;
                if (edit_.id.empty()) edit_.id = store_.new_id();
                SavedPc keep = edit_;
                if (!keep.remember) keep.password.clear();
                store_.upsert(keep);
                store_.save();
                dialog_ = Dialog::None;
            }
        }
        break;
    }
    case Dialog::Password: {
        Rect c = dialog_frame(440, 300, "Connect to " + (target_.label.empty() ? target_.host : target_.label));
        ui_.text(c.x, c.y - 8, display_address(target_.host, target_.port), theme::dim);
        float y = c.y + 38;
        form_field({c.x, y, c.w, 36}, "Password", pw_input_, true);
        y += 50;
        ui_.checkbox(c.x, y, "Remember password", pw_remember_);
        y += 34;
        if (!dialog_error_.empty()) ui_.text(c.x, y, dialog_error_, theme::bad);
        y += 28;
        bool go = ui_.button({c.x + c.w - 110, y, 110, 36}, "Connect", true) || submit_;
        if (ui_.button({c.x + c.w - 230, y, 110, 36}, "Cancel") || cancel_) {
            dialog_ = Dialog::None;
        } else if (go && !pw_input_.empty()) {
            target_.password = pw_input_;
            target_.remember = pw_remember_;
            if (SavedPc *pc = store_.find(target_.id)) {
                pc->remember = pw_remember_;
                pc->password = pw_remember_ ? pw_input_ : "";
                store_.save();
            }
            start_connect();
        }
        break;
    }
    case Dialog::Verify: {
        const bool changed = !verify_old_fp_.empty();
        Rect c = dialog_frame(520, changed ? 350 : 262, changed ? "Warning: this PC's identity changed" : "Verify this PC");
        float y = c.y;
        if (changed) {
            ui_.text(c.x, y, "The identity key of " + target_.host + " is different from last time.", theme::bad);
            y += 22;
            ui_.text(c.x, y, "This happens if TetherDesk was reinstalled there - or if someone", theme::text);
            y += 20;
            ui_.text(c.x, y, "is intercepting your connection. Only continue if you're sure.", theme::text);
            y += 30;
        } else {
            ui_.text(c.x, y, "First connection to " + ellipsize(ui_, target_.host, 260) + ".", theme::text);
            y += 22;
            ui_.text(c.x, y, "Check that this matches the \"Identity\" the host printed:", theme::dim);
            y += 32;
        }
        Rect fpbox{c.x, y, c.w, 44};
        ui_.fill(fpbox, theme::field, 8);
        ui_.text_centered(fpbox, host_fp_, changed ? theme::warn : theme::good, 1.15f);
        y += 56;
        if (changed) {
            ui_.text(c.x, y, "previously: " + verify_old_fp_, theme::dim);
            y += 24;
        }
        y += 16;
        bool trust = ui_.button({c.x + c.w - 170, y, 170, 36}, changed ? "Trust new identity" : "Trust and connect", !changed);
        if (!changed && submit_) trust = true;
        if (ui_.button({c.x + c.w - 290, y, 110, 36}, "Cancel", changed) || cancel_) {
            disconnect_user();
        } else if (trust) {
            store_.trust(target_.host, target_.port, host_fp_);
            store_.save();
            send_auth();
        }
        break;
    }
    case Dialog::Connecting: {
        Rect c = dialog_frame(480, 220, (dialog_error_.empty() ? "Connecting to " : "Couldn't connect to ") +
                                            (target_.label.empty() ? target_.host : target_.label));
        if (dialog_error_.empty()) {
            uint32_t dots = (SDL_GetTicks() / 400) % 4;
            const char *what = phase_ == Phase::Auth ? "Signing in" : phase_ == Phase::Verify ? "Verifying" : "Connecting";
            ui_.text(c.x, c.y + 4, std::string(what) + std::string(dots, '.'), theme::dim);
            ui_.text(c.x, c.y + 28, "Encrypted with X25519 + ChaCha20-Poly1305", theme::dim);
            if (ui_.button({c.x + c.w - 110, c.y + 72, 110, 36}, "Cancel") || cancel_) disconnect_user();
        } else {
            // Word-wrap the error over up to three lines.
            std::string rest = dialog_error_;
            float ly = c.y + 4;
            for (int line = 0; line < 3 && !rest.empty(); line++) {
                size_t cut = rest.size();
                while (cut > 0 && ui_.text_width(rest.substr(0, cut)) > c.w) {
                    size_t sp = rest.rfind(' ', cut - 1);
                    cut = (sp == std::string::npos || sp == 0) ? cut - 1 : sp;
                }
                ui_.text(c.x, ly, line == 2 ? ellipsize(ui_, rest, c.w) : rest.substr(0, cut), theme::bad);
                rest = rest.substr(std::min(rest.size(), cut + 1));
                ly += 20;
            }
            bool retry = ui_.button({c.x + c.w - 110, c.y + 92, 110, 36}, "Retry", true) || submit_;
            if (ui_.button({c.x + c.w - 230, c.y + 92, 110, 36}, "Close") || cancel_) dialog_ = Dialog::None;
            else if (retry) begin_connect(target_);
        }
        break;
    }
    case Dialog::None: break;
    }
}

void App::draw_session() {
    update_view();
    if (tex_) {
        SDL_FRect dst = {view_x_, view_y_, rw_ * view_s_, rh_ * view_s_};
        SDL_RenderCopyF(ren_, tex_, nullptr, &dst);
    }
    const float W = out_w_ / scale_, H = out_h_ / scale_;

    draw_connection_bar();
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

    if (phase_ != Phase::Live && dialog_ == Dialog::None) {
        ui_.fill({0, 0, W, H}, {0, 0, 0, 150});
        Rect box{W / 2 - 170, H / 2 - 40, 340, 80};
        ui_.fill(box, theme::panel, 12);
        ui_.text_centered({box.x, box.y + 12, box.w, 24}, "Reconnecting...", theme::text, 1.2f);
        ui_.text_centered({box.x, box.y + 44, box.w, 20},
                          "attempt " + std::to_string(reconnect_attempts_) + " of 10 - Esc to give up", theme::dim);
    }
}

// Remote Desktop-style connection bar: shown for a few seconds after
// connecting, whenever the mouse touches the top edge, or permanently when
// pinned.
void App::draw_connection_bar() {
    const float W = out_w_ / scale_;
    const uint32_t now = SDL_GetTicks();
    const int screens = displays_.size() > 1 ? int(std::min<size_t>(displays_.size(), 6)) : 0;
    const float screens_w = screens ? 64.f + screens * 32.f : 0.f;
    const float bw = std::min((is_web() ? 400.f : 560.f) + screens_w, W - 16);
    Rect bar{(W - bw) / 2, 0, bw, 38};
    const bool hot = mouse_y_ <= 4 || (bar_rect_.w > 0 && bar_rect_.contains(mouse_x_, mouse_y_)) || menu_open_;
    if (hot) bar_until_ = std::max(bar_until_, now + 1200);
    if (!bar_pinned_ && now >= bar_until_) {
        bar_rect_ = {0, 0, 0, 0};
        ui_.fill({W / 2 - 30, 0, 60, 3}, {98, 163, 255, 120}, 1);  // hint where the bar lives
        return;
    }
    bar_rect_ = bar;
    ui_.fill({bar.x, bar.y - 10, bar.w, bar.h + 10}, {22, 27, 36, 245}, 10);
    ui_.outline({bar.x, bar.y - 10, bar.w, bar.h + 10}, theme::panel_border, 10);
    float x = bar.x + 6;
    if (ui_.button({x, 5, 44, 28}, bar_pinned_ ? "Pin*" : "Pin", bar_pinned_)) bar_pinned_ = !bar_pinned_;
    x += 50;
    const float right_w = (is_web() ? 190 : 330) + screens_w;
    std::string title = target_.label.empty() ? host_name_ : target_.label;
    ui_.fill({x + 2, 16, 7, 7}, view_only_ ? theme::warn : theme::good, 3.5f);
    ui_.text(x + 14, 10, ellipsize(ui_, title, bar.x + bar.w - right_w - x - 20), theme::text);
    float bx = bar.x + bar.w - right_w;
    if (screens) {
        // Remote has several monitors: one button per screen.
        ui_.text(bx, 11, "Screen", theme::dim);
        bx += 56;
        for (int i = 0; i < screens; i++) {
            if (ui_.button({bx, 5, 28, 28}, std::to_string(i + 1), i == cur_display_) && i != cur_display_)
                switch_screen(i);
            bx += 32;
        }
        bx += 8;
    }
    if (ui_.button({bx, 5, 70, 28}, "Menu", menu_open_)) menu_open_ = !menu_open_;
    bx += 76;
    if (!is_web()) {
        if (ui_.button({bx, 5, 64, 28}, "Hide")) {
            release_input();
            SDL_MinimizeWindow(win_);
        }
        bx += 70;
        if (ui_.button({bx, 5, 78, 28}, fullscreen() ? "Window" : "Full")) set_fullscreen(!fullscreen());
        bx += 84;
    }
    if (ui_.button({bx, 5, 100, 28}, "Disconnect")) disconnect_user();
}

void App::draw_hud() {
    char l1[128], l2[128];
    std::snprintf(l1, sizeof l1, "%d fps   %.1f Mbit/s   %s", fps_shown_, mbps_,
                  rtt_ms_ >= 0 ? (std::to_string(int(rtt_ms_ + 0.5)) + " ms").c_str() : "- ms");
    std::snprintf(l2, sizeof l2, "%dx%d   %s   %d tiles   encrypted", rw_, rh_,
                  quality_ == 255 ? "auto quality" : kQualityNames[quality_], tiles_last_);
    const float w = std::max(ui_.text_width(l1), ui_.text_width(l2)) + 24;
    Rect box{12, 46, w, 54};
    ui_.fill(box, {0, 0, 0, 170}, 8);
    ui_.text(box.x + 12, box.y + 8, l1, theme::text);
    ui_.text(box.x + 12, box.y + 28, l2, theme::dim);
}

void App::draw_menu() {
    const float W = out_w_ / scale_, H = out_h_ / scale_;
    const float mw = std::min(kMenuW, W - 24);
    const float x = (W - mw) / 2, pad = 18, bx = x + 120, bw_avail = mw - 120 - pad;
    float y = 46;
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
    y += 20;
    ui_.text(x + pad, y, "Identity " + host_fp_, theme::dim);
    y += 30;

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
            if (i != cur_display_) switch_screen(i);
        });
    }
    if (hires_supported_) {
        row_label("Sharpness");
        buttons_row({"Sharp", "Fast"}, hires_on_ ? 0 : 1, [&](int i) {
            resolution_pref_ = i;
            send_settings();
        });
    }
    row_label("View");
    {
        const float bw = 96;
        if (ui_.button({bx, y, bw, 28}, "Fit", scale_mode_ == ScaleMode::Fit)) scale_mode_ = ScaleMode::Fit;
        if (ui_.button({bx + bw + 6, y, bw, 28}, "1:1 pixels", scale_mode_ == ScaleMode::Native))
            scale_mode_ = ScaleMode::Native;
        if (ui_.button({bx + 2 * (bw + 6), y, bw, 28}, "Stats", show_stats_)) show_stats_ = !show_stats_;
        y += 36;
    }
    row_label("Actions");
    {
        const float bw = (bw_avail - 12) / 3;
        if (ui_.button({bx, y, bw, 28}, "Ctrl+Alt+Del", false, !view_only_)) {
            for (uint16_t k : {RD_HID_LCTRL, RD_HID_LALT, RD_HID_DELETE}) send_key(k, true);
            for (uint16_t k : {RD_HID_DELETE, RD_HID_LALT, RD_HID_LCTRL}) send_key(k, false);
        }
        if (ui_.button({bx + bw + 6, y, bw, 28}, "Refresh")) send_simple(RD_C_REFRESH);
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
             is_web() ? "F7 next screen  -  F8 menu  -  F9 chat"
                      : "F7 next screen - F8 menu - F9 chat - F11 full screen - drop files to send",
             theme::dim);
    y += 30;
    last_h = std::min(y - 56, H - 60);
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
