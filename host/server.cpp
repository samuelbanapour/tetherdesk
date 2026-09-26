#include "server.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "rd_crypto.h"
#include "rd_proto.h"

namespace td {

namespace {

std::atomic<bool> g_stop{false};

constexpr uint64_t kSecond = 1000000;
constexpr size_t kMaxPendingOut = 8u << 20;  // skip frames beyond this backlog
constexpr int kMaxFramesInFlight = 2;

void log(const char *fmt, ...) {
    char ts[16];
    std::time_t now = std::time(nullptr);
    std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&now));
    std::fprintf(stdout, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// Keep names/chat printable and bounded; the viewer UI and the console both
// display them.
std::string clean_text(const std::string &s, size_t max_len) {
    std::string out;
    for (unsigned char ch : s) {
        if (out.size() >= max_len) break;
        if (ch >= 32 && ch != 127) out += char(ch);
    }
    return out;
}

bool file_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

void make_dirs(const std::string &path) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/' || path[i] == '\\') {
            if (!cur.empty() && cur != "~") {
#ifdef _WIN32
                _mkdir(cur.c_str());
#else
                mkdir(cur.c_str(), 0755);
#endif
            }
        }
        if (i < path.size()) cur += path[i];
    }
}

const char *mime_type(const std::string &path) {
    auto ends = [&](const char *ext) {
        size_t n = std::strlen(ext);
        return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js")) return "text/javascript";
    if (ends(".wasm")) return "application/wasm";
    if (ends(".css")) return "text/css";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".png")) return "image/png";
    if (ends(".ico")) return "image/x-icon";
    if (ends(".json")) return "application/json";
    return "application/octet-stream";
}

const char kFallbackPage[] =
    "<!doctype html><meta charset=utf-8><title>TetherDesk</title>"
    "<body style=\"font:16px system-ui;max-width:640px;margin:60px auto;padding:0 16px\">"
    "<h1>TetherDesk host is running</h1>"
    "<p>The web viewer has not been built, so there is nothing to serve here yet.</p>"
    "<p>Build it with <code>./build.sh web</code> (needs Emscripten), or connect with the native "
    "<code>tetherdesk</code> viewer.</p></body>";

}  // namespace

void Server::stop() { g_stop = true; }

Server::Server(ServerConfig cfg, Platform &platform)
    : cfg_(std::move(cfg)),
      plat_(platform),
      pool_(cfg_.encoder_threads > 0
                ? unsigned(cfg_.encoder_threads - 1)
                : std::min(7u, std::max(1u, std::thread::hardware_concurrency()) - 1)),
      encoder_(pool_) {
    rd_buf_init(&scratch_);
}

Server::~Server() {
    for (auto &c : conns_) rd_net_close(c->sock);
    rd_net_close(listener_);
    rd_buf_free(&scratch_);
}

// ------------------------------------------------------------ main loop

int Server::run() {
    char err[256];
    // Local-network listener. If the port is taken (e.g. TetherDesk is already
    // sharing, or QuickSupport runs alongside the app), try the next few; if
    // all are busy, carry on through the internet relay alone.
    for (int p = cfg_.port; p < cfg_.port + 10 && listener_ == RD_INVALID_SOCKET; p++) {
        listener_ = rd_net_listen(cfg_.bind.c_str(), p, err, sizeof err);
        if (listener_ == RD_INVALID_SOCKET && cfg_.bind == "::")  // no IPv6 on this machine
            listener_ = rd_net_listen("0.0.0.0", p, err, sizeof err);
        if (listener_ != RD_INVALID_SOCKET && p != cfg_.port)
            log("port %d is busy - local network connections use port %d", cfg_.port, p);
    }
    if (listener_ == RD_INVALID_SOCKET) {
        if (cfg_.relay_host.empty()) {
            std::fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        log("no local port available - reachable through the relay only");
    }
    std::string cerr;
    if (!plat_.capturer->start(cfg_.display, cfg_.max_fps, cerr)) {
        std::fprintf(stderr, "error: %s\n", cerr.c_str());
        return 1;
    }
    displays_ = plat_.capturer->displays();
    clip_count_ = plat_.clipboard->change_count();
    plat_.clipboard->get_text(last_clip_);

    bool console = cfg_.console;
#ifdef _WIN32
    console = false;  // stdin can't be poll()ed on Windows
#endif

    while (!g_stop) {
        std::vector<rd_pollfd> fds;
        fds.push_back({listener_, POLLIN, 0});
        const size_t first_conn = fds.size() + (console ? 1 : 0);
#ifndef _WIN32
        if (console) fds.push_back({0, POLLIN, 0});
#endif
        bool any_active = false;
        const size_t n_conns = conns_.size();
        for (auto &c : conns_) {
            short ev = POLLIN;
            if (c->pending() || c->tcp_pending || c->tls_handshaking) ev |= POLLOUT;
            fds.push_back({c->sock, ev, 0});
            any_active |= c->state == State::Active;
        }
        rd_poll(fds.data(), fds.size(), any_active ? 4 : 250);

#ifndef _WIN32
        if (console && (fds[1].revents & (POLLIN | POLLHUP))) {
            char buf[1024];
            ssize_t n = ::read(0, buf, sizeof buf);
            if (n <= 0) {
                console = false;  // stdin closed (e.g. running as a service)
            } else {
                console_buf_.append(buf, size_t(n));
                size_t nl;
                while ((nl = console_buf_.find('\n')) != std::string::npos) {
                    std::string line = console_buf_.substr(0, nl);
                    console_buf_.erase(0, nl + 1);
                    handle_console_line(line);
                }
            }
        }
#endif
        for (size_t i = 0; i < n_conns; i++) {
            Conn &c = *conns_[i];
            short re = fds[first_conn + i].revents;
            if (c.tcp_pending) {
                if (re & (POLLOUT | POLLERR | POLLHUP)) {
                    int e = rd_net_connect_result(c.sock);
                    if (e != 0) {
                        close_conn(c, "relay unreachable");
                        continue;
                    }
                    c.tcp_pending = false;
                    if (cfg_.relay_port == 443) {
                        c.tls = rd_tls_new(c.sock, cfg_.relay_host.c_str());
                        if (!c.tls) {
                            close_conn(c, "TLS setup failed");
                            continue;
                        }
                        c.tls_handshaking = true;
                    }
                    flush(c);
                }
                if (c.tcp_pending) continue;
            }
            if (c.tls_handshaking) {
                int h = rd_tls_handshake(c.tls);
                if (h < 0) {
                    if (c.relay_ctl) log("relay: %s", rd_tls_error(c.tls));
                    close_conn(c, "TLS handshake failed");
                    continue;
                }
                if (h > 0) continue;
                c.tls_handshaking = false;
                flush(c);
            }
            if (re & (POLLIN | POLLHUP | POLLERR)) read_conn(c);
            if ((re & POLLOUT) && !c.dead) flush(c);
        }
        if (listener_ != RD_INVALID_SOCKET && (fds[0].revents & POLLIN)) accept_new();

        const uint64_t now = rd_now_us();
        for (auto &c : conns_) {
            if (c->dead) continue;
            if (c->state == State::RelayCtl) {
                if (now - c->last_rx_us > 70 * kSecond) close_conn(*c, "relay went quiet");
                else if (now - c->last_ping_us > 25 * kSecond) {
                    c->last_ping_us = now;
                    ws_frame(*c, RD_WS_PING, "td", 2);
                }
                continue;
            }
            bool authed = c->state == State::Active;
            if (!authed && now - c->created_us > 120 * kSecond) close_conn(*c, "handshake timeout");
            else if (authed && now - c->last_rx_us > 45 * kSecond) close_conn(*c, "timed out");
        }

        pump_frames();
        poll_clipboard();
        for (auto &c : conns_)
            if (!c->dead) flush(*c);

        // Reap closed connections.
        std::vector<std::string> left;
        for (auto &c : conns_) {
            if (!c->dead) continue;
            for (auto &u : c->uploads) {
                if (u.second.file) std::fclose(u.second.file);
                std::remove(u.second.path.c_str());
            }
            if (c->sent_input) plat_.input->release_all();
            if (c.get() == relay_ctl_) {
                relay_ctl_ = nullptr;
                if (relay_online_) log("relay connection lost - reconnecting");
                relay_online_ = false;
                relay_next_us_ = rd_now_us() + relay_backoff_us_;
                relay_backoff_us_ = std::min<uint64_t>(relay_backoff_us_ * 2, 30 * kSecond);
            }
            if (c->state == State::Active || c->state == State::Closing) {
                if (!c->name.empty()) left.push_back(c->name);
            }
            rd_net_close(c->sock);
        }
        conns_.erase(std::remove_if(conns_.begin(), conns_.end(), [](auto &c) { return c->dead; }), conns_.end());
        if (!left.empty()) {
            broadcast_viewers();
            for (auto &n : left) notice(n + " disconnected");
        }
        maintain_relay();
    }

    log("shutting down");
    plat_.capturer->stop();
    return 0;
}

void Server::accept_new() {
    for (;;) {
        char addr[64];
        rd_socket s = rd_net_accept(listener_, addr, sizeof addr);
        if (s == RD_INVALID_SOCKET) return;
        if (conns_.size() >= size_t(cfg_.max_viewers) * 2 + 16) {  // hard cap on sockets
            rd_net_close(s);
            continue;
        }
        auto c = std::make_unique<Conn>();
        c->sock = s;
        c->addr = addr;
        c->created_us = c->last_rx_us = rd_now_us();
        conns_.push_back(std::move(c));
    }
}

// ------------------------------------------------------------ relay client

void Server::maintain_relay() {
    if (cfg_.relay_host.empty() || relay_ctl_ || rd_now_us() < relay_next_us_) return;
    open_relay_conn(true, "", "");
}

void Server::open_relay_conn(bool control, const std::string &ticket, const std::string &viewer_ip) {
    char err[256];
    rd_socket s = rd_net_connect_start(cfg_.relay_host.c_str(), cfg_.relay_port, 0, err, sizeof err);
    if (s == RD_INVALID_SOCKET) {
        if (control) {
            relay_next_us_ = rd_now_us() + relay_backoff_us_;
            relay_backoff_us_ = std::min<uint64_t>(relay_backoff_us_ * 2, 30 * kSecond);
        }
        return;
    }
    auto c = std::make_unique<Conn>();
    c->sock = s;
    c->addr = control ? "relay" : (viewer_ip.empty() ? "relay" : viewer_ip) + " (via relay)";
    c->created_us = c->last_rx_us = c->last_ping_us = rd_now_us();
    c->client_ws = true;
    c->relay_ctl = control;
    c->tcp_pending = true;
    c->state = State::RelayUpgrade;
    c->ws.expect_masked = 0;
    c->ws.max_message = RD_MAX_CLIENT_MSG;

    uint8_t raw[16];
    rd_random(raw, sizeof raw);
    char key[32];
    rd_base64(raw, sizeof raw, key);
    rd_ws_accept_key(key, c->ws_accept);
    std::string path = control ? "/host?id=" + cfg_.relay_id + "&key=" + cfg_.relay_key
                               : "/data?id=" + cfg_.relay_id + "&key=" + cfg_.relay_key + "&ticket=" + ticket;
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + cfg_.relay_host +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                      "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: TetherDesk-Host\r\n\r\n";
    rd_buf_put(&c->out, req.data(), req.size());
    if (control) relay_ctl_ = c.get();
    conns_.push_back(std::move(c));
}

void Server::finish_relay_upgrade(Conn &c) {
    const char *base = reinterpret_cast<const char *>(c.in.data);
    size_t end = 0;
    for (size_t i = 0; i + 3 < c.in.len; i++)
        if (!std::memcmp(c.in.data + i, "\r\n\r\n", 4)) {
            end = i;
            break;
        }
    if (!end) {
        if (c.in.len > 16384) close_conn(c, "bad relay response");
        return;
    }
    std::string resp(base, end + 2);
    rd_buf_consume(&c.in, end + 4);
    char accept[64] = "";
    if (resp.compare(0, 12, "HTTP/1.1 101") != 0 ||
        !rd_http_header(resp.substr(resp.find("\r\n") + 2).c_str(), "Sec-WebSocket-Accept", accept, sizeof accept) ||
        std::strcmp(accept, c.ws_accept) != 0) {
        if (c.relay_ctl && resp.compare(9, 3, "409") == 0)
            log("relay: ID %s is in use by another computer", cfg_.relay_id.c_str());
        close_conn(c, "relay refused");
        return;
    }
    if (c.relay_ctl) {
        c.state = State::RelayCtl;
        relay_online_ = true;
        relay_backoff_us_ = 2 * kSecond;
        log("relay: online as %s (%s) - connect from anywhere with this ID", cfg_.relay_id.c_str(),
            cfg_.relay_host.c_str());
    } else {
        c.state = State::Hello;
    }
    if (c.in.len) handle_ws_messages(c);
}

void Server::ws_frame(Conn &c, int opcode, const void *p, size_t n) {
    if (!c.dead) rd_ws_write_frame(&c.out, opcode, p, n, c.client_ws ? 1 : 0);
}

void Server::read_conn(Conn &c) {
    uint8_t buf[65536];
    size_t total = 0;
    while (total < (4u << 20)) {
        long n = c.tls ? rd_tls_recv(c.tls, buf, sizeof buf) : rd_net_recv(c.sock, buf, sizeof buf);
        if (n < 0) {
            close_conn(c, "connection closed");
            return;
        }
        if (n == 0) break;
        rd_buf_put(&c.in, buf, size_t(n));
        total += size_t(n);
    }
    if (!total) return;
    c.last_rx_us = rd_now_us();
    if (c.state == State::Http) handle_http(c);
    else if (c.state == State::RelayUpgrade) finish_relay_upgrade(c);
    else handle_ws_messages(c);
}

void Server::flush(Conn &c) {
    if (c.tcp_pending || c.tls_handshaking) return;
    while (c.pending()) {
        long n = c.tls ? rd_tls_send(c.tls, c.out.data + c.out_off, c.pending())
                       : rd_net_send(c.sock, c.out.data + c.out_off, c.pending());
        if (n < 0) {
            close_conn(c, "send failed");
            return;
        }
        if (n == 0) break;
        c.out_off += size_t(n);
        c.bytes_sent += uint64_t(n);
    }
    if (!c.pending()) {
        rd_buf_clear(&c.out);
        c.out_off = 0;
        if (c.close_after_flush) c.dead = true;
    } else if (c.out_off > (1u << 20) && c.out_off > c.out.len / 2) {
        rd_buf_consume(&c.out, c.out_off);
        c.out_off = 0;
    }
}

void Server::close_conn(Conn &c, const char *reason) {
    if (c.dead) return;
    if (c.state == State::Active) log("%s (%s): %s", c.name.c_str(), c.addr.c_str(), reason);
    c.dead = true;
}

// ------------------------------------------------------------ HTTP

void Server::http_response(Conn &c, int code, const char *status, const char *type, const std::string &body) {
    char hdr[512];
    int n = std::snprintf(hdr, sizeof hdr,
                          "HTTP/1.1 %d %s\r\nServer: TetherDesk\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                          "Cache-Control: no-cache\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
                          code, status, type, body.size());
    rd_buf_put(&c.out, hdr, size_t(n));
    rd_buf_put(&c.out, body.data(), body.size());
    c.close_after_flush = true;
}

void Server::handle_http(Conn &c) {
    const char *end = nullptr;
    for (size_t i = 0; i + 3 < c.in.len; i++)
        if (!std::memcmp(c.in.data + i, "\r\n\r\n", 4)) {
            end = reinterpret_cast<const char *>(c.in.data) + i;
            break;
        }
    if (!end) {
        if (c.in.len > 16384) http_response(c, 431, "Request Header Fields Too Large", "text/plain", "too large\n");
        return;
    }
    std::string req(reinterpret_cast<const char *>(c.in.data), size_t(end - reinterpret_cast<const char *>(c.in.data)) + 2);
    rd_buf_consume(&c.in, req.size() + 2);

    std::istringstream line(req.substr(0, req.find("\r\n")));
    std::string method, target;
    line >> method >> target;
    const std::string headers = req.substr(req.find("\r\n") + 2);

    char upgrade[64] = "", key[128] = "", version[16] = "";
    rd_http_header(headers.c_str(), "Upgrade", upgrade, sizeof upgrade);
    for (char *p = upgrade; *p; p++) *p = char(std::tolower(static_cast<unsigned char>(*p)));

    if (std::strstr(upgrade, "websocket")) {
        if (method != "GET" || !rd_http_header(headers.c_str(), "Sec-WebSocket-Key", key, sizeof key) ||
            !rd_http_header(headers.c_str(), "Sec-WebSocket-Version", version, sizeof version) ||
            std::strcmp(version, "13") != 0) {
            http_response(c, 400, "Bad Request", "text/plain", "bad websocket handshake\n");
            return;
        }
        char accept[29];
        rd_ws_accept_key(key, accept);
        char resp[256];
        int n = std::snprintf(resp, sizeof resp,
                              "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Accept: %s\r\n\r\n",
                              accept);
        rd_buf_put(&c.out, resp, size_t(n));
        c.state = State::Hello;
        c.ws.max_message = RD_MAX_CLIENT_MSG;
        if (c.in.len) handle_ws_messages(c);
        return;
    }
    if (method != "GET" && method != "HEAD") {
        http_response(c, 405, "Method Not Allowed", "text/plain", "method not allowed\n");
        return;
    }
    serve_file(c, target);
    if (method == "HEAD") {  // drop the body we just queued, keep headers
        const char *hdr_end = std::strstr(reinterpret_cast<const char *>(c.out.data), "\r\n\r\n");
        if (hdr_end) c.out.len = size_t(hdr_end - reinterpret_cast<const char *>(c.out.data)) + 4;
    }
}

void Server::serve_file(Conn &c, std::string path) {
    path = path.substr(0, path.find_first_of("?#"));
    if (path.empty() || path == "/") path = "/index.html";
    if (path[0] != '/' || path.find("..") != std::string::npos || path.find('\\') != std::string::npos ||
        path.find('%') != std::string::npos) {
        http_response(c, 400, "Bad Request", "text/plain", "bad path\n");
        return;
    }
    if (!cfg_.web_root.empty()) {
        std::ifstream f(cfg_.web_root + path, std::ios::binary);
        if (f) {
            std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            http_response(c, 200, "OK", mime_type(path), body);
            return;
        }
    }
    if (path == "/index.html") http_response(c, 200, "OK", "text/html; charset=utf-8", kFallbackPage);
    else http_response(c, 404, "Not Found", "text/plain", "not found\n");
}

// ------------------------------------------------------------ WebSocket

void Server::send_msg(Conn &c, const rd_buf &msg) {
    if (c.dead) return;
    if (!c.ch.active) {
        ws_frame(c, RD_WS_BINARY, msg.data, msg.len);
        return;
    }
    if (c.client_ws) {  // relay link: frames must be masked, so seal separately
        rd_buf sealed;
        rd_buf_init(&sealed);
        rd_channel_seal(&c.ch, msg.data, msg.len, &sealed);
        ws_frame(c, RD_WS_BINARY, sealed.data, sealed.len);
        rd_buf_free(&sealed);
        return;
    }
    // Seal straight into the send queue behind the WebSocket header.
    rd_ws_write_header(&c.out, RD_WS_BINARY, msg.len + RD_TAG_LEN);
    rd_channel_seal(&c.ch, msg.data, msg.len, &c.out);
}

void Server::broadcast(const rd_buf &msg, const Conn *except, bool control_only) {
    for (auto &c : conns_)
        if (c.get() != except && c->state == State::Active && !c->dead && !(control_only && c->view_only))
            send_msg(*c, msg);
}

void Server::handle_ws_messages(Conn &c) {
    rd_ws_msg m;
    int rc;
    while (!c.dead && c.state != State::Closing && (rc = rd_ws_next(&c.ws, &c.in, &m)) == 1) {
        switch (m.opcode) {
        case RD_WS_BINARY: handle_message(c, m.data, m.len); break;
        case RD_WS_PING: ws_frame(c, RD_WS_PONG, m.data, m.len); break;
        case RD_WS_CLOSE:
            ws_frame(c, RD_WS_CLOSE, m.data, std::min<size_t>(m.len, 2));
            if (c.state == State::Active) log("%s left", c.name.c_str());
            c.state = State::Closing;
            c.close_after_flush = true;
            return;
        case RD_WS_TEXT:
            // Relay control: "open TICKET VIEWER_IP" = a viewer wants in.
            if (c.state == State::RelayCtl && m.len > 5 && m.len < 256 && !std::memcmp(m.data, "open ", 5)) {
                std::string cmd(reinterpret_cast<const char *>(m.data) + 5, m.len - 5);
                size_t sp = cmd.find(' ');
                std::string ticket = cmd.substr(0, sp), ip = sp == std::string::npos ? "" : cmd.substr(sp + 1);
                int relayed = 0;
                for (auto &o : conns_) relayed += o->client_ws && !o->relay_ctl && !o->dead;
                if (relayed < cfg_.max_viewers * 2) open_relay_conn(false, ticket, ip);
            }
            break;
        default: break;  // pong: ignored
        }
    }
    if (rc < 0) close_conn(c, "websocket protocol error");
}

void Server::handle_message(Conn &c, const uint8_t *p, size_t n) {
    if (c.ch.active) {
        c.plain.resize(n);
        long pl = n >= RD_TAG_LEN ? rd_channel_open(&c.ch, p, n, c.plain.data()) : -1;
        if (pl < 0) return close_conn(c, "decryption failed (tampered or corrupted data)");
        p = c.plain.data();
        n = size_t(pl);
    }
    rd_reader r;
    rd_reader_init(&r, p, n);
    const uint8_t type = rd_get_u8(&r);
    rd_buf &msg = scratch_;
    rd_buf_clear(&msg);

    if (c.state == State::Hello) {
        if (type != RD_C_HELLO) return close_conn(c, "expected hello");
        uint16_t version = rd_get_u16(&r);
        char name[256];
        rd_get_str(&r, name, sizeof name);
        const uint8_t *ce = rd_get_bytes(&r, 32);
        c.name = clean_text(name, 32);
        if (c.name.empty()) c.name = "Viewer";
        if (version != RD_PROTO_VERSION || !ce) {
            rd_buf_put_u8(&msg, RD_S_AUTH_RESULT);
            rd_buf_put_u8(&msg, 0);
            rd_buf_put_u8(&msg, 0);
            rd_buf_put_str(&msg, "Incompatible viewer version - please update TetherDesk");
            send_msg(c, msg);
            c.close_after_flush = true;
            return;
        }
        // Key agreement: ephemeral-ephemeral + ephemeral(viewer)-static(host).
        uint8_t se_priv[32], se[32], dh1[32], dh2[32];
        rd_random(c.nonce, sizeof c.nonce);
        if (rd_x25519_keypair(se_priv, se) != 0 || rd_x25519(dh1, se_priv, ce) != 0 ||
            rd_x25519(dh2, cfg_.static_priv, ce) != 0)
            return close_conn(c, "invalid key exchange");
        rd_hs_transcript(c.transcript, ce, cfg_.static_pub, se, c.nonce);
        rd_buf_put_u8(&msg, RD_S_HELLO);
        rd_buf_put_u16(&msg, RD_PROTO_VERSION);
        rd_buf_put_u8(&msg, RD_AUTH_X25519_CHACHA);
        rd_buf_put(&msg, c.nonce, sizeof c.nonce);
        rd_buf_put_str(&msg, cfg_.host_name.c_str());
        rd_buf_put_str(&msg, plat_.os_name.c_str());
        rd_buf_put(&msg, cfg_.static_pub, 32);
        rd_buf_put(&msg, se, 32);
        send_msg(c, msg);  // last plaintext message
        rd_hs_keys(&c.ch, dh1, dh2, c.transcript, 1);
        rd_wipe(se_priv, sizeof se_priv);
        rd_wipe(dh1, sizeof dh1);
        rd_wipe(dh2, sizeof dh2);
        c.state = State::Auth;
        return;
    }

    if (c.state == State::Auth) {
        if (type != RD_C_AUTH) return close_conn(c, "expected auth");
        const uint8_t *mac = rd_get_bytes(&r, RD_MAC_LEN);
        auto fail = [&](const std::string &why) {
            rd_buf_put_u8(&msg, RD_S_AUTH_RESULT);
            rd_buf_put_u8(&msg, 0);
            rd_buf_put_u8(&msg, 0);
            rd_buf_put_str(&msg, why.c_str());
            send_msg(c, msg);
            c.state = State::Closing;
            c.close_after_flush = true;
        };
        int wait_s = 0;
        if (locked_out(c.addr, wait_s)) {
            log("rejected %s: locked out for %ds", c.addr.c_str(), wait_s);
            return fail("Too many failed attempts - try again in " + std::to_string(wait_s) + " s");
        }
        uint8_t expected[32];
        rd_hs_auth_mac(expected, cfg_.password.data(), cfg_.password.size(), c.transcript);
        if (!mac || !rd_ct_equal(mac, expected, sizeof expected)) {
            record_auth_failure(c.addr);
            log("wrong password from %s (\"%s\")", c.addr.c_str(), c.name.c_str());
            return fail("Wrong password");
        }
        int active = 0;
        for (auto &o : conns_) active += o->state == State::Active && !o->dead;
        if (active >= cfg_.max_viewers) return fail("Host is full (" + std::to_string(cfg_.max_viewers) + " viewers)");
        failures_.erase(c.addr);
        on_authenticated(c);
        return;
    }

    if (c.state == State::Active) handle_session_message(c, type, r);
}

void Server::on_authenticated(Conn &c) {
    c.state = State::Active;
    c.id = next_id_++;
    c.view_only = cfg_.view_only;
    c.fps = cfg_.max_fps;
    c.eff_fps = c.fps;
    c.need_full = true;
    c.adapt_window_us = rd_now_us();

    rd_buf &msg = scratch_;
    rd_buf_clear(&msg);
    rd_buf_put_u8(&msg, RD_S_AUTH_RESULT);
    rd_buf_put_u8(&msg, 1);
    rd_buf_put_u8(&msg, c.view_only ? 1 : 0);
    rd_buf_put_str(&msg, c.view_only ? "Connected (view only)" : "Connected");
    send_msg(c, msg);
    send_display_info(c);
    log("%s connected from %s%s", c.name.c_str(), c.addr.c_str(), c.view_only ? " (view only)" : "");
    broadcast_viewers();
    notice(c.name + " connected", &c);
}

void Server::handle_session_message(Conn &c, uint8_t type, rd_reader &r) {
    rd_buf &msg = scratch_;
    rd_buf_clear(&msg);
    switch (type) {
    case RD_C_FRAME_ACK: {
        uint32_t id = rd_get_u32(&r);
        if (int32_t(id - c.acked) > 0 && int32_t(id - c.frame_id) <= 0) c.acked = id;
        break;
    }
    case RD_C_REFRESH: c.need_full = true; break;
    case RD_C_SETTINGS: {
        int q = rd_get_u8(&r), fps = rd_get_u8(&r), disp = rd_get_u8(&r);
        if (r.err) break;
        int res = rd_remaining(&r) ? rd_get_u8(&r) : 255;  // newer viewers only
        int old_q = c.quality;
        c.quality_setting = (q <= 3 || q == 255) ? q : 255;
        if (c.quality_setting != 255) c.quality = c.quality_setting;
        (void)old_q;  // sharper settings take effect through per-tile refinement
        c.fps = std::max(1, std::min(cfg_.max_fps, fps));
        c.eff_fps = c.fps;
        if (res != 255 && !c.view_only && plat_.capturer->supports_high_resolution() &&
            (res == 0) != plat_.capturer->high_resolution()) {
            plat_.capturer->set_high_resolution(res == 0);
            switch_display(plat_.capturer->current_display());  // restart capture at the new size
            notice(res == 0 ? "Sharp (full resolution) screen" : "Fast (half resolution) screen");
        }
        if (disp != plat_.capturer->current_display()) {
            if (!c.view_only) {
                switch_display(disp);
            } else {
                rd_buf_clear(&msg);
                rd_buf_put_u8(&msg, RD_S_NOTICE);
                rd_buf_put_str(&msg, "View-only viewers can't switch displays");
                send_msg(c, msg);
            }
        }
        break;
    }
    case RD_C_POINTER: {
        int x = rd_get_u16(&r), y = rd_get_u16(&r), buttons = rd_get_u8(&r);
        int wx = rd_get_i16(&r), wy = rd_get_i16(&r);
        if (r.err || c.view_only || !c.shadow_w) break;
        plat_.input->pointer(std::min(x, c.shadow_w - 1), std::min(y, c.shadow_h - 1), uint8_t(buttons), wx, wy);
        c.sent_input = true;
        break;
    }
    case RD_C_KEY: {
        uint16_t hid = rd_get_u16(&r);
        bool down = rd_get_u8(&r) != 0;
        if (r.err || c.view_only || hid >= RD_HID_MAX) break;
        plat_.input->key(hid, down);
        c.sent_input = true;
        break;
    }
    case RD_C_CLIPBOARD: {
        uint32_t len;
        const uint8_t *p = rd_get_blob(&r, &len);
        if (r.err || c.view_only || len > RD_MAX_CLIPBOARD) break;
        std::string text(reinterpret_cast<const char *>(p), len);
        if (text == last_clip_) break;
        plat_.clipboard->set_text(text);
        last_clip_ = text;
        clip_count_ = plat_.clipboard->change_count();
        rd_buf_put_u8(&msg, RD_S_CLIPBOARD);
        rd_buf_put_blob(&msg, text.data(), uint32_t(text.size()));
        broadcast(msg, &c, true);
        break;
    }
    case RD_C_CHAT: {
        char text[4096];
        rd_get_str(&r, text, sizeof text);
        std::string t = clean_text(text, 1000);
        if (r.err || t.empty()) break;
        std::printf("  [chat] %s: %s\n", c.name.c_str(), t.c_str());
        std::fflush(stdout);
        rd_buf_put_u8(&msg, RD_S_CHAT);
        rd_buf_put_str(&msg, c.name.c_str());
        rd_buf_put_str(&msg, t.c_str());
        broadcast(msg, &c);
        break;
    }
    case RD_C_PING: {
        uint64_t t = rd_get_u64(&r);
        rd_buf_put_u8(&msg, RD_S_PONG);
        rd_buf_put_u64(&msg, t);
        send_msg(c, msg);
        break;
    }
    case RD_C_FILE_BEGIN: {
        uint32_t id = rd_get_u32(&r);
        uint64_t size = rd_get_u64(&r);
        char name[1024];
        rd_get_str(&r, name, sizeof name);
        if (!r.err) begin_upload(c, id, size, name);
        break;
    }
    case RD_C_FILE_CHUNK: {
        uint32_t id = rd_get_u32(&r);
        auto it = c.uploads.find(id);
        if (r.err || it == c.uploads.end()) break;
        Upload &u = it->second;
        size_t n = rd_remaining(&r);
        const uint8_t *data = rd_get_bytes(&r, n);
        if (u.received + n > u.size) return finish_upload(c, id, false, "File is larger than announced");
        if (n && std::fwrite(data, 1, n, u.file) != n) return finish_upload(c, id, false, "Host disk write failed");
        u.received += n;
        break;
    }
    case RD_C_FILE_END: {
        uint32_t id = rd_get_u32(&r);
        auto it = c.uploads.find(id);
        if (r.err || it == c.uploads.end()) break;
        if (it->second.received != it->second.size) finish_upload(c, id, false, "Transfer incomplete");
        else finish_upload(c, id, true, "Saved to " + it->second.path);
        break;
    }
    default: break;  // unknown messages are ignored for forward compatibility
    }
}

// ------------------------------------------------------------ screen

void Server::send_display_info(Conn &c) {
    rd_buf msg;
    rd_buf_init(&msg);
    FramePtr f = plat_.capturer->latest();
    int cur = plat_.capturer->current_display();
    int w = f ? f->width : 0, h = f ? f->height : 0;
    if (!f && cur >= 0 && cur < int(displays_.size())) w = displays_[size_t(cur)].width, h = displays_[size_t(cur)].height;
    rd_buf_put_u8(&msg, RD_S_DISPLAY_INFO);
    rd_buf_put_u16(&msg, uint16_t(w));
    rd_buf_put_u16(&msg, uint16_t(h));
    rd_buf_put_u8(&msg, uint8_t(cur));
    rd_buf_put_u8(&msg, uint8_t(std::min<size_t>(displays_.size(), 255)));
    for (size_t i = 0; i < displays_.size() && i < 255; i++) {
        rd_buf_put_str(&msg, displays_[i].name.c_str());
        rd_buf_put_u16(&msg, uint16_t(displays_[i].width));
        rd_buf_put_u16(&msg, uint16_t(displays_[i].height));
    }
    uint8_t flags = 0;
    if (plat_.capturer->supports_high_resolution()) flags |= 1;
    if (plat_.capturer->high_resolution()) flags |= 2;
    rd_buf_put_u8(&msg, flags);
    send_msg(c, msg);
    rd_buf_free(&msg);
}

void Server::pump_frames() {
    FramePtr f = plat_.capturer->latest();
    const uint64_t now = rd_now_us();
    for (auto &c : conns_) {
        if (c->dead || c->state != State::Active) continue;
        maybe_send_frame(*c, f, now);
        adapt_quality(*c, now);
    }
}

void Server::maybe_send_frame(Conn &c, const FramePtr &f, uint64_t now) {
    if (!f) return;
    if (f->width != c.shadow_w || f->height != c.shadow_h) {
        c.shadow.assign(size_t(f->width) * f->height * 4, 0);
        c.shadow_w = f->width;
        c.shadow_h = f->height;
        c.tile_q.clear();
        c.lossy_tiles = 0;
        c.need_full = true;
        send_display_info(c);
    }
    const bool changed = f->seq != c.last_seq;
    if (changed) c.last_change_us = now;
    // Lossy tiles get refined to lossless once the screen settles briefly
    // (or a few at a time alongside ongoing changes) - only in auto mode.
    const bool refine = c.quality_setting == 255 && c.lossy_tiles > 0;
    const bool settled = now - c.last_change_us > 250000;
    if (!changed && !c.need_full && !(refine && settled)) return;
    const int fps = std::max(1, std::min(c.fps, c.eff_fps));
    if (now - c.last_frame_us < kSecond / uint64_t(fps)) return;
    if (c.frame_id - c.acked >= uint32_t(kMaxFramesInFlight) || c.pending() > kMaxPendingOut) {
        // Only a backlog in our own send queue means the *network* is the
        // bottleneck; a viewer that is merely slow to decode isn't helped by
        // sending less, so that doesn't count towards congestion.
        if (c.pending() > 0 && changed) c.stalls++;
        return;
    }

    rd_buf &msg = scratch_;
    rd_buf_clear(&msg);
    rd_buf_put_u8(&msg, RD_S_FRAME);
    rd_buf_put_u32(&msg, c.frame_id + 1);
    rd_buf_put_u8(&msg, c.need_full ? RD_FRAME_FULL : 0);
    const size_t count_at = msg.len;
    rd_buf_put_u16(&msg, 0);
    const int budget = !refine ? 0 : settled ? 400 : 12;
    EncodeStats st = encoder_.encode(*f, c.shadow, c.need_full, c.quality, msg, &c.tile_q, budget);
    c.last_seq = f->seq;
    c.need_full = false;
    c.lossy_tiles = int(std::count_if(c.tile_q.begin(), c.tile_q.end(), [](uint8_t q) { return q != 0; }));
    if (st.tiles == 0) return;
    rd_buf_patch_u16(&msg, count_at, uint16_t(st.tiles));
    c.frame_id++;
    c.last_frame_us = now;
    if (changed) c.sends++;
    send_msg(c, msg);
}

// Automatic quality. When the network can't keep up ("stalls": a new frame
// was ready but our send queue was still backed up), first lower the frame
// rate, and only reduce colour depth if that isn't enough. When things are
// smooth again, restore colour first, then frame rate. Anything sent with
// reduced colour is refined back to lossless once it stops changing.
void Server::adapt_quality(Conn &c, uint64_t now) {
    if (now - c.adapt_window_us < kSecond) return;
    const bool congested = c.stalls > 2 && c.stalls * 2 > c.sends;
    if (congested) {
        c.clean_windows = 0;
        if (c.eff_fps > 10) c.eff_fps = std::max(10, c.eff_fps / 2);
        else if (c.quality_setting == 255 && c.quality < 2) c.quality++;
    } else if (c.stalls == 0) {
        if (++c.clean_windows >= 2) {
            c.clean_windows = 0;
            if (c.quality_setting == 255 && c.quality > 0) c.quality--;
            else if (c.eff_fps < c.fps) c.eff_fps = std::min(c.fps, c.eff_fps * 2);
        }
    } else {
        c.clean_windows = 0;
    }
    c.stalls = c.sends = 0;
    c.adapt_window_us = now;
}

void Server::switch_display(int index) {
    if (index < 0 || index >= int(displays_.size())) return;
    int old = plat_.capturer->current_display();
    std::string err;
    if (!plat_.capturer->start(index, cfg_.max_fps, err)) {
        notice("Could not switch display: " + err);
        plat_.capturer->start(old, cfg_.max_fps, err);
        return;
    }
    displays_ = plat_.capturer->displays();
    log("switched to display %d (%s)", index, displays_[size_t(index)].name.c_str());
    for (auto &c : conns_)
        if (c->state == State::Active && !c->dead) {
            c->need_full = true;
            c->shadow_w = c->shadow_h = 0;  // forces DISPLAY_INFO with the new size
        }
    notice("Now showing " + displays_[size_t(index)].name);
}

// ------------------------------------------------------------ collaboration

void Server::poll_clipboard() {
    uint64_t now = rd_now_us();
    if (now < next_clip_poll_us_) return;
    next_clip_poll_us_ = now + 400000;
    uint64_t cc = plat_.clipboard->change_count();
    if (cc == clip_count_) return;
    clip_count_ = cc;
    std::string text;
    if (!plat_.clipboard->get_text(text) || text == last_clip_ || text.size() > RD_MAX_CLIPBOARD) return;
    last_clip_ = text;
    rd_buf &msg = scratch_;
    rd_buf_clear(&msg);
    rd_buf_put_u8(&msg, RD_S_CLIPBOARD);
    rd_buf_put_blob(&msg, text.data(), uint32_t(text.size()));
    broadcast(msg, nullptr, true);
}

void Server::broadcast_viewers() {
    rd_buf msg;
    rd_buf_init(&msg);
    std::vector<Conn *> active;
    for (auto &c : conns_)
        if (c->state == State::Active && !c->dead) active.push_back(c.get());
    rd_buf_put_u8(&msg, RD_S_VIEWERS);
    rd_buf_put_u8(&msg, uint8_t(std::min<size_t>(active.size(), 255)));
    for (Conn *c : active) {
        rd_buf_put_str(&msg, c->name.c_str());
        rd_buf_put_str(&msg, c->addr.c_str());
        rd_buf_put_u8(&msg, c->view_only ? 1 : 0);
    }
    broadcast(msg);
    rd_buf_free(&msg);
}

void Server::notice(const std::string &text, const Conn *except) {
    rd_buf msg;
    rd_buf_init(&msg);
    rd_buf_put_u8(&msg, RD_S_NOTICE);
    rd_buf_put_str(&msg, text.c_str());
    broadcast(msg, except);
    rd_buf_free(&msg);
}

Server::Conn *Server::find_viewer(const std::string &name) {
    for (auto &c : conns_)
        if (c->state == State::Active && !c->dead && (c->name == name || std::to_string(c->id) == name)) return c.get();
    return nullptr;
}

void Server::handle_console_line(const std::string &raw) {
    std::string line = clean_text(raw, 1000);
    if (line.empty()) return;
    std::string cmd = line, arg;
    if (size_t sp = line.find(' '); sp != std::string::npos) cmd = line.substr(0, sp), arg = line.substr(sp + 1);

    if (cmd == "/help") {
        std::printf("  /list               show connected viewers\n"
                    "  /kick <name|id>     disconnect a viewer\n"
                    "  /viewonly <name|id> toggle a viewer's control rights\n"
                    "  /password <new>     change the password for new connections\n"
                    "  /quit               stop the host\n"
                    "  anything else       sends a chat message to all viewers\n");
    } else if (cmd == "/list") {
        int n = 0;
        for (auto &c : conns_)
            if (c->state == State::Active && !c->dead) {
                std::printf("  #%u %-20s %-16s q%d %s%dfps  sent %.1f MB\n", c->id, c->name.c_str(), c->addr.c_str(),
                            c->quality, c->view_only ? "view-only " : "", c->fps, double(c->bytes_sent) / 1e6);
                n++;
            }
        if (!n) std::printf("  no viewers connected\n");
    } else if (cmd == "/kick" || cmd == "/viewonly") {
        Conn *c = find_viewer(arg);
        if (!c) {
            std::printf("  no viewer named '%s' (see /list)\n", arg.c_str());
        } else if (cmd == "/kick") {
            rd_buf msg;
            rd_buf_init(&msg);
            rd_buf_put_u8(&msg, RD_S_NOTICE);
            rd_buf_put_str(&msg, "The host disconnected you");
            send_msg(*c, msg);
            rd_buf_free(&msg);
            c->close_after_flush = true;
            c->state = State::Closing;
            log("kicked %s", c->name.c_str());
        } else {
            c->view_only = !c->view_only;
            if (c->view_only && c->sent_input) plat_.input->release_all();
            rd_buf msg;
            rd_buf_init(&msg);
            rd_buf_put_u8(&msg, RD_S_AUTH_RESULT);
            rd_buf_put_u8(&msg, 1);
            rd_buf_put_u8(&msg, c->view_only ? 1 : 0);
            rd_buf_put_str(&msg, c->view_only ? "The host made you view-only" : "The host gave you control");
            send_msg(*c, msg);
            rd_buf_free(&msg);
            broadcast_viewers();
            log("%s is now %s", c->name.c_str(), c->view_only ? "view-only" : "in control");
        }
    } else if (cmd == "/password") {
        if (arg.size() < 4) std::printf("  password must be at least 4 characters\n");
        else {
            cfg_.password = arg;
            std::printf("  password changed (existing viewers stay connected)\n");
        }
    } else if (cmd == "/quit") {
        stop();
    } else if (cmd[0] == '/') {
        std::printf("  unknown command - try /help\n");
    } else {
        rd_buf msg;
        rd_buf_init(&msg);
        rd_buf_put_u8(&msg, RD_S_CHAT);
        rd_buf_put_str(&msg, "Host");
        rd_buf_put_str(&msg, line.c_str());
        broadcast(msg);
        rd_buf_free(&msg);
    }
    std::fflush(stdout);
}

// ------------------------------------------------------------ file upload

void Server::begin_upload(Conn &c, uint32_t id, uint64_t size, const std::string &raw_name) {
    auto reject = [&](const std::string &why) {
        rd_buf msg;
        rd_buf_init(&msg);
        rd_buf_put_u8(&msg, RD_S_FILE_RESULT);
        rd_buf_put_u32(&msg, id);
        rd_buf_put_u8(&msg, 0);
        rd_buf_put_str(&msg, why.c_str());
        send_msg(c, msg);
        rd_buf_free(&msg);
    };
    if (!cfg_.allow_files) return reject("File transfer is disabled on this host");
    if (c.view_only) return reject("View-only viewers can't send files");
    if (size > RD_MAX_FILE_SIZE) return reject("File too large");
    if (c.uploads.size() >= 4 || c.uploads.count(id)) return reject("Too many transfers at once");

    // Strip any directory components and characters that are unsafe on
    // common file systems; the viewer never chooses where the file goes.
    std::string name = raw_name;
    if (size_t slash = name.find_last_of("/\\"); slash != std::string::npos) name = name.substr(slash + 1);
    std::string safe;
    for (unsigned char ch : name)
        if (ch >= 32 && !std::strchr("<>:\"|?*", ch)) safe += char(ch);
    while (!safe.empty() && (safe[0] == '.' || safe[0] == ' ')) safe.erase(0, 1);
    if (safe.empty()) safe = "upload.bin";
    if (safe.size() > 120) safe = safe.substr(safe.size() - 120);

    make_dirs(cfg_.downloads_dir);
    std::string base = safe, ext;
    if (size_t dot = safe.find_last_of('.'); dot != std::string::npos && dot > 0) base = safe.substr(0, dot), ext = safe.substr(dot);
    std::string path = cfg_.downloads_dir + "/" + safe;
    for (int i = 1; file_exists(path) && i < 1000; i++) path = cfg_.downloads_dir + "/" + base + " (" + std::to_string(i) + ")" + ext;

    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return reject("Host could not create the file");
    Upload &u = c.uploads[id];
    u.file = f;
    u.size = size;
    u.path = path;
    log("%s is sending \"%s\" (%.1f MB)", c.name.c_str(), safe.c_str(), double(size) / 1e6);
}

void Server::finish_upload(Conn &c, uint32_t id, bool ok, const std::string &text) {
    auto it = c.uploads.find(id);
    if (it == c.uploads.end()) return;
    std::fclose(it->second.file);
    if (!ok) std::remove(it->second.path.c_str());
    log("upload from %s %s: %s", c.name.c_str(), ok ? "finished" : "failed", text.c_str());
    c.uploads.erase(it);
    rd_buf msg;
    rd_buf_init(&msg);
    rd_buf_put_u8(&msg, RD_S_FILE_RESULT);
    rd_buf_put_u32(&msg, id);
    rd_buf_put_u8(&msg, ok ? 1 : 0);
    rd_buf_put_str(&msg, text.c_str());
    send_msg(c, msg);
    rd_buf_free(&msg);
}

// ------------------------------------------------------------ brute-force protection

bool Server::locked_out(const std::string &addr, int &seconds_left) {
    // Global limit: many failures from many addresses (a distributed guess
    // attack when exposed to the internet) pauses all logins briefly.
    uint64_t now_us = rd_now_us();
    recent_failures_.erase(std::remove_if(recent_failures_.begin(), recent_failures_.end(),
                                          [&](uint64_t t) { return now_us - t > 60 * kSecond; }),
                           recent_failures_.end());
    if (recent_failures_.size() >= 30) {
        seconds_left = int((recent_failures_.front() + 60 * kSecond - now_us) / kSecond) + 1;
        return true;
    }
    auto it = failures_.find(addr);
    if (it == failures_.end()) return false;
    uint64_t now = rd_now_us();
    if (it->second.until_us <= now) return false;
    seconds_left = int((it->second.until_us - now) / kSecond) + 1;
    return true;
}

void Server::record_auth_failure(const std::string &addr) {
    recent_failures_.push_back(rd_now_us());
    Failures &f = failures_[addr];
    f.count++;
    if (f.count >= 5) {
        // 30 s, 60 s, 120 s ... capped at 15 minutes.
        uint64_t secs = std::min<uint64_t>(900, 30ull << std::min(f.count - 5, 5));
        f.until_us = rd_now_us() + secs * kSecond;
    }
}

}  // namespace td
