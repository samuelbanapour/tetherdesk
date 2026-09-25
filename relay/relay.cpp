// relay.cpp - TetherDesk internet relay.
//
// Lets a viewer reach a host with no port forwarding or VPN: both sides make
// *outbound* WebSocket connections here and the relay pairs them by the
// host's 9-digit ID. Sessions are end-to-end encrypted between viewer and
// host (see rd_secure.h), so the relay only ever sees ciphertext.
//
//   GET /host?id=ID&key=KEY          host control channel (WebSocket)
//        relay -> host text "open TICKET VIEWER_IP" for each new viewer
//   GET /data?id=ID&key=KEY&ticket=T host data channel for one viewer
//   GET /v/ID                         viewer (WebSocket); closes with code
//                                     4404 if that ID isn't online
//   GET /, /index.*                   the web viewer (C++ -> WebAssembly)
//   GET /get                          download page for the QuickSupport app
//   GET /download/FILE                QuickSupport downloads
//   GET /healthz                      health check
//
// A host owns its ID for as long as it's connected (it proves ownership with
// a random key it generated); if it reconnects with the same key it takes
// the ID back. Messages are forwarded whole, with back-pressure so a slow
// viewer can't make the relay buffer unbounded amounts of screen data.
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "rd_bytes.h"
#include "rd_crypto.h"
#include "rd_net.h"
#include "rd_ws.h"

namespace {

constexpr uint64_t kSecond = 1000000;
constexpr size_t kMaxMessage = 64u << 20;
constexpr size_t kBackpressure = 8u << 20;
constexpr size_t kMaxConns = 4000;
constexpr int kMaxConnsPerIp = 60;

std::atomic<bool> g_stop{false};
std::string g_web_root = "public";

void log(const char *fmt, ...) {
    char ts[32];
    std::time_t now = std::time(nullptr);
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::gmtime(&now));
    std::fprintf(stdout, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

enum class Kind { Http, HostCtl, HostData, Viewer };

struct Conn {
    rd_socket sock = RD_INVALID_SOCKET;
    std::string ip;
    Kind kind = Kind::Http;
    rd_buf in{}, out{};
    size_t out_off = 0;
    rd_ws_reader ws{};
    uint64_t created_us = 0, last_rx_us = 0, last_ping_us = 0;
    bool close_after_flush = false, dead = false;
    std::string id, ticket;
    Conn *peer = nullptr;
    std::vector<std::vector<uint8_t>> pending;  // viewer messages before pairing
    size_t pending_bytes = 0;

    Conn() {
        rd_buf_init(&in);
        rd_buf_init(&out);
        rd_ws_reader_init(&ws, kMaxMessage, 1);
    }
    ~Conn() {
        rd_net_close(sock);
        rd_buf_free(&in);
        rd_buf_free(&out);
        rd_ws_reader_free(&ws);
    }
    size_t pending_out() const { return out.len - out_off; }
};

struct HostEntry {
    std::string key;
    Conn *ctl = nullptr;
};

std::vector<std::unique_ptr<Conn>> g_conns;
std::map<std::string, HostEntry> g_hosts;   // id -> owner
std::map<std::string, Conn *> g_tickets;    // ticket -> waiting viewer

std::string query_param(const std::string &target, const std::string &name) {
    size_t q = target.find('?');
    if (q == std::string::npos) return "";
    std::string qs = target.substr(q + 1);
    size_t pos = 0;
    while (pos <= qs.size()) {
        size_t amp = qs.find('&', pos);
        std::string kv = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == name) return kv.substr(eq + 1);
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

bool valid_id(const std::string &id) {
    return id.size() == 9 && std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool valid_hex(const std::string &s, size_t n) {
    return s.size() == n && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

std::string random_hex(size_t bytes) {
    std::vector<uint8_t> r(bytes);
    rd_random(r.data(), bytes);
    static const char hex[] = "0123456789abcdef";
    std::string s;
    for (uint8_t b : r) s += hex[b >> 4], s += hex[b & 15];
    return s;
}

void http_response(Conn &c, int code, const char *status, const char *type, const std::string &body,
                   const char *extra = "") {
    char hdr[768];
    int n = std::snprintf(hdr, sizeof hdr,
                          "HTTP/1.1 %d %s\r\nServer: TetherDesk-Relay\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                          "Cache-Control: no-cache\r\nX-Content-Type-Options: nosniff\r\n%sConnection: close\r\n\r\n",
                          code, status, type, body.size(), extra);
    rd_buf_put(&c.out, hdr, size_t(n));
    rd_buf_put(&c.out, body.data(), body.size());
    c.close_after_flush = true;
}

void ws_send(Conn &c, int opcode, const void *p, size_t n) {
    if (!c.dead) rd_ws_write_frame(&c.out, opcode, p, n, 0);
}

void ws_close(Conn &c, uint16_t code, const std::string &reason) {
    uint8_t buf[128];
    buf[0] = uint8_t(code >> 8);
    buf[1] = uint8_t(code);
    size_t n = std::min<size_t>(reason.size(), sizeof buf - 2);
    std::memcpy(buf + 2, reason.data(), n);
    ws_send(c, RD_WS_CLOSE, buf, n + 2);
    c.close_after_flush = true;
}

void kill_conn(Conn &c) {
    if (c.dead) return;
    c.dead = true;
    if (c.peer) {
        Conn &p = *c.peer;
        p.peer = nullptr;
        ws_close(p, 1001, "peer disconnected");
        c.peer = nullptr;
    }
    if (c.kind == Kind::Viewer && !c.ticket.empty()) g_tickets.erase(c.ticket);
    if (c.kind == Kind::HostCtl) {
        auto it = g_hosts.find(c.id);
        if (it != g_hosts.end() && it->second.ctl == &c) {
            it->second.ctl = nullptr;
            log("host %s offline", c.id.c_str());
        }
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
    if (ends(".png")) return "image/png";
    if (ends(".svg")) return "image/svg+xml";
    return "application/octet-stream";
}

void serve_static(Conn &c, std::string path) {
    path = path.substr(0, path.find_first_of("?#"));
    if (path == "/" || path.empty()) {  // open the web viewer in "connect by ID" mode
        http_response(c, 302, "Found", "text/plain", "", "Location: /app#relay\r\n");
        return;
    }
    if (path == "/app") path = "/index.html";
    if (path == "/get") path = "/get.html";
    if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos ||
        path.find('%') != std::string::npos) {
        http_response(c, 400, "Bad Request", "text/plain", "bad path\n");
        return;
    }
    std::ifstream f(g_web_root + path, std::ios::binary);
    if (!f) {
        http_response(c, 404, "Not Found", "text/plain", "not found\n");
        return;
    }
    std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string extra = "Strict-Transport-Security: max-age=31536000\r\n";
    if (path.rfind("/download/", 0) == 0)
        extra += "Content-Disposition: attachment; filename=\"" + path.substr(10) + "\"\r\n";
    http_response(c, 200, "OK", mime_type(path), body, extra.c_str());
}

void accept_upgrade(Conn &c, const char *key) {
    char accept[29];
    rd_ws_accept_key(key, accept);
    char resp[256];
    int n = std::snprintf(resp, sizeof resp,
                          "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n\r\n",
                          accept);
    rd_buf_put(&c.out, resp, size_t(n));
}

void handle_ws_message(Conn &c, int opcode, const uint8_t *p, size_t n);

void handle_http(Conn &c) {
    const char *base = reinterpret_cast<const char *>(c.in.data);
    size_t end = 0;
    for (size_t i = 0; i + 3 < c.in.len; i++)
        if (!std::memcmp(c.in.data + i, "\r\n\r\n", 4)) {
            end = i;
            break;
        }
    if (!end) {
        if (c.in.len > 16384) http_response(c, 431, "Request Header Fields Too Large", "text/plain", "too large\n");
        return;
    }
    std::string req(base, end + 2);
    rd_buf_consume(&c.in, end + 4);
    std::istringstream line(req.substr(0, req.find("\r\n")));
    std::string method, target;
    line >> method >> target;
    const std::string headers = req.substr(req.find("\r\n") + 2);

    char fly_ip[64];
    if (rd_http_header(headers.c_str(), "Fly-Client-IP", fly_ip, sizeof fly_ip)) c.ip = fly_ip;

    char upgrade[64] = "", key[128] = "";
    rd_http_header(headers.c_str(), "Upgrade", upgrade, sizeof upgrade);
    for (char *q = upgrade; *q; q++) *q = char(std::tolower(static_cast<unsigned char>(*q)));
    const bool is_ws = std::strstr(upgrade, "websocket") && rd_http_header(headers.c_str(), "Sec-WebSocket-Key", key, sizeof key);
    const std::string path = target.substr(0, target.find('?'));

    if (!is_ws) {
        if (method != "GET" && method != "HEAD") return http_response(c, 405, "Method Not Allowed", "text/plain", "no\n");
        if (path == "/healthz") return http_response(c, 200, "OK", "text/plain", "ok\n");
        return serve_static(c, target);
    }

    if (path == "/host") {
        std::string id = query_param(target, "id"), hkey = query_param(target, "key");
        if (!valid_id(id) || !valid_hex(hkey, 32)) return http_response(c, 400, "Bad Request", "text/plain", "bad id\n");
        HostEntry &h = g_hosts[id];
        if (h.ctl && h.key != hkey) return http_response(c, 409, "Conflict", "text/plain", "id in use\n");
        if (h.ctl) {  // same host reconnecting: retire the stale channel
            Conn *old = h.ctl;
            h.ctl = nullptr;
            ws_close(*old, 1001, "replaced");
            kill_conn(*old);
        }
        h.key = hkey;
        h.ctl = &c;
        c.kind = Kind::HostCtl;
        c.id = id;
        accept_upgrade(c, key);
        log("host %s online from %s", id.c_str(), c.ip.c_str());
    } else if (path == "/data") {
        std::string id = query_param(target, "id"), hkey = query_param(target, "key"), t = query_param(target, "ticket");
        auto h = g_hosts.find(id);
        auto tk = g_tickets.find(t);
        if (h == g_hosts.end() || h->second.key != hkey || tk == g_tickets.end() || tk->second->id != id)
            return http_response(c, 403, "Forbidden", "text/plain", "bad ticket\n");
        Conn &viewer = *tk->second;
        g_tickets.erase(tk);
        viewer.ticket.clear();
        c.kind = Kind::HostData;
        c.id = id;
        accept_upgrade(c, key);
        c.peer = &viewer;
        viewer.peer = &c;
        for (auto &m : viewer.pending) ws_send(c, RD_WS_BINARY, m.data(), m.size());
        viewer.pending.clear();
        viewer.pending_bytes = 0;
    } else if (path.rfind("/v/", 0) == 0) {
        std::string id = path.substr(3);
        c.kind = Kind::Viewer;
        c.id = id;
        accept_upgrade(c, key);
        auto h = g_hosts.find(id);
        if (!valid_id(id) || h == g_hosts.end() || !h->second.ctl) return ws_close(c, 4404, "offline");
        c.ticket = random_hex(16);
        g_tickets[c.ticket] = &c;
        std::string open = "open " + c.ticket + " " + c.ip;
        ws_send(*h->second.ctl, RD_WS_TEXT, open.data(), open.size());
    } else {
        return http_response(c, 404, "Not Found", "text/plain", "not found\n");
    }
    // A client may pipeline its first WebSocket frames behind the upgrade.
    rd_ws_msg m;
    int rc = 0;
    while (!c.dead && (rc = rd_ws_next(&c.ws, &c.in, &m)) == 1) handle_ws_message(c, m.opcode, m.data, m.len);
}

void handle_ws_message(Conn &c, int opcode, const uint8_t *p, size_t n) {
    switch (opcode) {
    case RD_WS_PING: ws_send(c, RD_WS_PONG, p, n); return;
    case RD_WS_PONG: return;
    case RD_WS_CLOSE:
        ws_send(c, RD_WS_CLOSE, p, std::min<size_t>(n, 2));
        c.close_after_flush = true;
        if (c.peer) {
            ws_close(*c.peer, 1000, "peer closed");
            c.peer->peer = nullptr;
            c.peer = nullptr;
        }
        return;
    default: break;
    }
    if (c.kind == Kind::HostCtl) return;  // keep-alive chatter
    if (c.peer) {
        ws_send(*c.peer, opcode, p, n);
    } else if (c.kind == Kind::Viewer && !c.ticket.empty()) {
        if (c.pending_bytes + n > (4u << 20)) return kill_conn(c);
        c.pending.emplace_back(p, p + n);
        c.pending_bytes += n;
    }
}

void read_conn(Conn &c) {
    uint8_t buf[65536];
    bool closed = false;
    for (int i = 0; i < 64; i++) {
        long n = rd_net_recv(c.sock, buf, sizeof buf);
        if (n < 0) {
            closed = true;  // forward what already arrived (e.g. an auth rejection) first
            break;
        }
        if (n == 0) break;
        rd_buf_put(&c.in, buf, size_t(n));
        c.last_rx_us = rd_now_us();
    }
    if (c.kind == Kind::Http) {
        if (closed) return kill_conn(c);
        handle_http(c);
        return;
    }
    rd_ws_msg m;
    int rc = 0;
    while (!c.dead && !c.close_after_flush && (rc = rd_ws_next(&c.ws, &c.in, &m)) == 1)
        handle_ws_message(c, m.opcode, m.data, m.len);
    if (rc < 0 || closed) kill_conn(c);
}

void flush(Conn &c) {
    while (c.pending_out()) {
        long n = rd_net_send(c.sock, c.out.data + c.out_off, c.pending_out());
        if (n < 0) return kill_conn(c);
        if (n == 0) break;
        c.out_off += size_t(n);
    }
    if (!c.pending_out()) {
        rd_buf_clear(&c.out);
        c.out_off = 0;
        if (c.close_after_flush) kill_conn(c);
    } else if (c.out_off > (1u << 20) && c.out_off > c.out.len / 2) {
        rd_buf_consume(&c.out, c.out_off);
        c.out_off = 0;
    }
}

void on_signal(int) { g_stop = true; }

}  // namespace

int main(int argc, char **argv) {
    int port = 8080;
    if (const char *p = std::getenv("PORT")) port = std::atoi(p);
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--web-root" && i + 1 < argc) g_web_root = argv[++i];
    }
    rd_net_init();
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    char err[256];
    rd_socket listener = rd_net_listen("::", port, err, sizeof err);
    if (listener == RD_INVALID_SOCKET) listener = rd_net_listen("0.0.0.0", port, err, sizeof err);
    if (listener == RD_INVALID_SOCKET) {
        std::fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    log("TetherDesk relay listening on port %d, web root %s", port, g_web_root.c_str());

    while (!g_stop) {
        std::vector<rd_pollfd> fds;
        fds.push_back({listener, POLLIN, 0});
        const size_t n = g_conns.size();
        for (auto &c : g_conns) {
            short ev = 0;
            // Back-pressure: stop reading from a side whose peer is backed up.
            if (!(c->peer && c->peer->pending_out() > kBackpressure)) ev |= POLLIN;
            if (c->pending_out()) ev |= POLLOUT;
            fds.push_back({c->sock, ev, 0});
        }
        rd_poll(fds.data(), fds.size(), 1000);

        for (size_t i = 0; i < n; i++) {
            Conn &c = *g_conns[i];
            short re = fds[i + 1].revents;
            if (!c.dead && (re & (POLLIN | POLLHUP | POLLERR))) read_conn(c);
            if (!c.dead && (re & POLLOUT)) flush(c);
        }

        if (fds[0].revents & POLLIN) {
            for (;;) {
                char addr[64];
                rd_socket s = rd_net_accept(listener, addr, sizeof addr);
                if (s == RD_INVALID_SOCKET) break;
                int same_ip = 0;
                for (auto &c : g_conns) same_ip += c->ip == addr;
                if (g_conns.size() >= kMaxConns || same_ip >= kMaxConnsPerIp) {
                    rd_net_close(s);
                    continue;
                }
                auto c = std::make_unique<Conn>();
                c->sock = s;
                c->ip = addr;
                c->created_us = c->last_rx_us = c->last_ping_us = rd_now_us();
                g_conns.push_back(std::move(c));
            }
        }

        const uint64_t now = rd_now_us();
        for (auto &cp : g_conns) {
            Conn &c = *cp;
            if (c.dead) continue;
            if (c.kind == Kind::Http && now - c.created_us > 15 * kSecond) kill_conn(c);
            else if (c.kind == Kind::Viewer && !c.peer && !c.ticket.empty() && now - c.created_us > 20 * kSecond)
                ws_close(c, 4408, "host did not answer");
            else if (c.kind != Kind::Http && now - c.last_rx_us > 90 * kSecond) kill_conn(c);
            else if (c.kind != Kind::Http && now - c.last_ping_us > 20 * kSecond) {
                c.last_ping_us = now;  // keep proxies from timing the connection out
                ws_send(c, RD_WS_PING, "td", 2);
            }
            if (!c.dead) flush(c);
        }
        g_conns.erase(std::remove_if(g_conns.begin(), g_conns.end(), [](auto &c) { return c->dead; }), g_conns.end());
    }
    log("relay shutting down");
    return 0;
}
