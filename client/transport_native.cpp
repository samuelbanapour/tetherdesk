// transport_native.cpp - WebSocket client over a non-blocking TCP socket.
#include <cstring>

#include "rd_bytes.h"
#include "rd_crypto.h"
#include "rd_net.h"
#include "rd_proto.h"
#include "rd_ws.h"
#include "transport.h"

namespace td {
namespace {

class NativeTransport : public Transport {
public:
    NativeTransport() {
        rd_net_init();
        rd_buf_init(&in_);
        rd_buf_init(&out_);
        rd_ws_reader_init(&ws_, RD_MAX_SERVER_MSG, 0);
    }
    ~NativeTransport() override {
        close();
        rd_buf_free(&in_);
        rd_buf_free(&out_);
        rd_ws_reader_free(&ws_);
    }

    void connect(const std::string &host, int port) override {
        close();
        rd_buf_clear(&in_);
        rd_buf_clear(&out_);
        rd_ws_reader_free(&ws_);
        rd_ws_reader_init(&ws_, RD_MAX_SERVER_MSG, 0);
        error_.clear();
        host_ = host;
        port_ = port;
        attempt_ = 0;
        if (!open_socket()) return;
        state_ = State::Connecting;
        started_us_ = rd_now_us();

        uint8_t raw[16];
        rd_random(raw, sizeof raw);
        char key[32];
        rd_base64(raw, sizeof raw, key);
        rd_ws_accept_key(key, expected_accept_);
        std::string h = host.find(':') != std::string::npos ? "[" + host + "]" : host;
        std::string req = "GET /ws HTTP/1.1\r\nHost: " + h + ":" + std::to_string(port) +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
                          "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: TetherDesk\r\n\r\n";
        rd_buf_put(&out_, req.data(), req.size());
    }

    void send(const uint8_t *data, size_t len) override {
        if (state_ != State::Open) return;
        rd_ws_write_frame(&out_, RD_WS_BINARY, data, len, 1);
        flush();
    }

    void poll(std::vector<std::vector<uint8_t>> &out) override {
        if (state_ != State::Connecting && state_ != State::Open) return;
        rd_pollfd p = {sock_, short(POLLIN | (out_.len ? POLLOUT : 0)), 0};
        rd_poll(&p, 1, 0);
        if (!tcp_up_) {
            if (p.revents & (POLLOUT | POLLERR | POLLHUP)) {
                int e = rd_net_connect_result(sock_);
                if (e != 0) {
                    // Try the next address (e.g. IPv4 after IPv6 for "localhost").
                    rd_net_close(sock_);
                    sock_ = RD_INVALID_SOCKET;
                    attempt_++;
                    if (!open_socket(true))
                        return fail("could not connect (error " + std::to_string(e) + ") - is the host running?");
                    return;
                }
                tcp_up_ = true;
            } else {
                if (rd_now_us() - started_us_ > 10000000) fail("connection timed out");
                return;
            }
        }
        flush();
        uint8_t buf[65536];
        bool peer_closed = false;
        for (int i = 0; i < 256; i++) {  // cap per poll so the UI stays responsive
            long n = rd_net_recv(sock_, buf, sizeof buf);
            if (n < 0) {
                peer_closed = true;  // still deliver what already arrived (e.g. an auth rejection)
                break;
            }
            if (n == 0) break;
            rd_buf_put(&in_, buf, size_t(n));
        }
        if (state_ == State::Connecting && !finish_handshake()) {
            if (peer_closed) fail("connection refused");
            return;
        }
        rd_ws_msg m;
        int rc;
        while ((rc = rd_ws_next(&ws_, &in_, &m)) == 1) {
            if (m.opcode == RD_WS_BINARY) out.emplace_back(m.data, m.data + m.len);
            else if (m.opcode == RD_WS_PING) rd_ws_write_frame(&out_, RD_WS_PONG, m.data, m.len, 1);
            else if (m.opcode == RD_WS_CLOSE) return fail("the host closed the connection");
        }
        if (rc < 0) return fail("protocol error from host");
        if (peer_closed) return fail("the host closed the connection");
        flush();
    }

    State state() const override { return state_; }
    std::string error() const override { return error_; }
    size_t buffered() const override { return out_.len; }

    void close() override {
        if (sock_ != RD_INVALID_SOCKET) {
            if (state_ == State::Open) {
                rd_ws_write_frame(&out_, RD_WS_CLOSE, "\x03\xe8", 2, 1);  // 1000 normal closure
                flush();
            }
            rd_net_close(sock_);
        }
        sock_ = RD_INVALID_SOCKET;
        if (state_ != State::Idle) state_ = State::Closed;
    }

private:
    // Opens a socket to the current address attempt, skipping addresses that
    // fail immediately. `quiet` keeps the original error if all fail.
    bool open_socket(bool quiet = false) {
        char err[256] = "";
        for (; attempt_ < 8; attempt_++) {
            sock_ = rd_net_connect_start(host_.c_str(), port_, attempt_, err, sizeof err);
            if (sock_ != RD_INVALID_SOCKET) {
                tcp_up_ = false;
                return true;
            }
            if (std::strncmp(err, "no more", 7) == 0 || std::strncmp(err, "cannot resolve", 14) == 0) break;
        }
        if (!quiet) fail(err);
        return false;
    }

    bool finish_handshake() {
        const char *end = nullptr;
        for (size_t i = 0; i + 3 < in_.len; i++)
            if (!std::memcmp(in_.data + i, "\r\n\r\n", 4)) {
                end = reinterpret_cast<const char *>(in_.data) + i;
                break;
            }
        if (!end) {
            if (in_.len > 16384) fail("bad handshake response");
            return false;
        }
        std::string resp(reinterpret_cast<const char *>(in_.data), size_t(end - reinterpret_cast<const char *>(in_.data)) + 2);
        rd_buf_consume(&in_, resp.size() + 2);
        char accept[64] = "";
        if (resp.compare(0, 12, "HTTP/1.1 101") != 0 ||
            !rd_http_header(resp.substr(resp.find("\r\n") + 2).c_str(), "Sec-WebSocket-Accept", accept, sizeof accept) ||
            std::strcmp(accept, expected_accept_) != 0) {
            fail("not a TetherDesk host (WebSocket upgrade refused)");
            return false;
        }
        state_ = State::Open;
        return true;
    }

    void flush() {
        size_t off = 0;
        while (off < out_.len) {
            long n = rd_net_send(sock_, out_.data + off, out_.len - off);
            if (n < 0) {
                rd_buf_clear(&out_);
                fail("connection lost");
                return;
            }
            if (n == 0) break;
            off += size_t(n);
        }
        rd_buf_consume(&out_, off);
    }

    void fail(const std::string &why) {
        if (error_.empty()) error_ = why;
        if (sock_ != RD_INVALID_SOCKET) rd_net_close(sock_);
        sock_ = RD_INVALID_SOCKET;
        state_ = State::Closed;
    }

    rd_socket sock_ = RD_INVALID_SOCKET;
    std::string host_;
    int port_ = 0, attempt_ = 0;
    State state_ = State::Idle;
    bool tcp_up_ = false;
    uint64_t started_us_ = 0;
    rd_buf in_, out_;
    rd_ws_reader ws_;
    char expected_accept_[29] = "";
    std::string error_;
};

}  // namespace

std::unique_ptr<Transport> Transport::create() { return std::make_unique<NativeTransport>(); }

}  // namespace td
