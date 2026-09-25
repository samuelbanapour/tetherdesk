// server.h - the TetherDesk host server.
//
// A single-threaded, poll()-driven event loop that:
//   * serves the WebAssembly web viewer over plain HTTP,
//   * upgrades /ws requests to WebSocket sessions,
//   * authenticates viewers with HMAC-SHA256 challenge/response and locks out
//     addresses that keep guessing,
//   * streams dirty screen tiles to each viewer with per-viewer flow control
//     and adaptive quality,
//   * relays input, clipboard, chat and file uploads,
//   * offers an operator console on stdin (/list, /kick, /viewonly, chat).
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "encoder.h"
#include "platform.h"
#include "rd_bytes.h"
#include "rd_net.h"
#include "rd_secure.h"
#include "rd_ws.h"

namespace td {

struct ServerConfig {
    std::string bind = "0.0.0.0";
    int port = 5980;
    std::string password;
    bool view_only = false;       // every viewer is view-only
    bool allow_files = true;
    bool console = true;          // read operator commands from stdin
    int max_viewers = 8;
    int max_fps = 30;
    int display = 0;
    std::string web_root;         // directory with the built web viewer
    std::string downloads_dir;    // where uploaded files land
    std::string host_name;
    int encoder_threads = 0;      // 0 = auto
    uint8_t static_priv[32] = {}; // host identity key (X25519)
    uint8_t static_pub[32] = {};
};

class Server {
public:
    Server(ServerConfig cfg, Platform &platform);
    ~Server();
    // Runs until stop() is called (e.g. from a signal handler). Returns exit code.
    int run();
    static void stop();

private:
    struct Upload {
        FILE *file = nullptr;
        uint64_t size = 0, received = 0;
        std::string path;
    };

    enum class State { Http, Hello, Auth, Active, Closing };

    struct Conn {
        rd_socket sock = RD_INVALID_SOCKET;
        std::string addr;
        State state = State::Http;
        rd_buf in{}, out{};
        size_t out_off = 0;  // bytes of `out` already written to the socket
        rd_ws_reader ws{};
        uint64_t created_us = 0, last_rx_us = 0;
        bool close_after_flush = false;
        bool dead = false;

        // Session state (valid once authenticated).
        uint32_t id = 0;
        std::string name;
        uint8_t nonce[16] = {};
        uint8_t transcript[32] = {};
        rd_channel ch{};
        std::vector<uint8_t> plain;  // decrypt buffer
        bool view_only = false;
        bool sent_input = false;
        int quality_setting = 255;  // 0..3 fixed, 255 = automatic
        int quality = 1;            // effective level in use
        int fps = 30;
        uint32_t frame_id = 0, acked = 0;
        uint64_t last_seq = 0;
        bool need_full = true;
        uint64_t last_frame_us = 0;
        std::vector<uint8_t> shadow;
        int shadow_w = 0, shadow_h = 0;
        // Adaptive quality bookkeeping.
        int stalls = 0, sends = 0, clean_windows = 0;
        uint64_t adapt_window_us = 0, last_refine_us = 0;
        // Stats.
        uint64_t bytes_sent = 0;
        std::map<uint32_t, Upload> uploads;

        Conn() {
            rd_buf_init(&in);
            rd_buf_init(&out);
            rd_ws_reader_init(&ws, 0, 1);
        }
        ~Conn() {
            rd_buf_free(&in);
            rd_buf_free(&out);
            rd_ws_reader_free(&ws);
        }
        Conn(const Conn &) = delete;
        Conn &operator=(const Conn &) = delete;
        size_t pending() const { return out.len - out_off; }
    };

    void accept_new();
    void read_conn(Conn &c);
    void handle_http(Conn &c);
    void serve_file(Conn &c, std::string path);
    void http_response(Conn &c, int code, const char *status, const char *type, const std::string &body);
    void handle_ws_messages(Conn &c);
    void handle_message(Conn &c, const uint8_t *p, size_t n);
    void handle_session_message(Conn &c, uint8_t type, rd_reader &r);
    void on_authenticated(Conn &c);
    void close_conn(Conn &c, const char *reason);
    void flush(Conn &c);

    void send_msg(Conn &c, const rd_buf &msg);
    void broadcast(const rd_buf &msg, const Conn *except = nullptr, bool control_only = false);
    void send_display_info(Conn &c);
    void broadcast_viewers();
    void notice(const std::string &text, const Conn *except = nullptr);

    void pump_frames();
    void maybe_send_frame(Conn &c, const FramePtr &f, uint64_t now);
    void adapt_quality(Conn &c, uint64_t now);
    void poll_clipboard();
    void switch_display(int index);
    void handle_console_line(const std::string &line);
    Conn *find_viewer(const std::string &name);

    void begin_upload(Conn &c, uint32_t id, uint64_t size, const std::string &name);
    void finish_upload(Conn &c, uint32_t id, bool ok, const std::string &msg);

    bool locked_out(const std::string &addr, int &seconds_left);
    void record_auth_failure(const std::string &addr);

    ServerConfig cfg_;
    Platform &plat_;
    rd_socket listener_ = RD_INVALID_SOCKET;
    std::vector<std::unique_ptr<Conn>> conns_;
    WorkerPool pool_;
    FrameEncoder encoder_;
    uint32_t next_id_ = 1;
    uint64_t clip_count_ = 0;
    std::string last_clip_;
    std::vector<DisplayInfo> displays_;
    uint64_t next_clip_poll_us_ = 0;
    std::string console_buf_;
    struct Failures {
        int count = 0;
        uint64_t until_us = 0;
    };
    std::map<std::string, Failures> failures_;
    std::vector<uint64_t> recent_failures_;  // all addresses, for a global rate limit
    rd_buf scratch_{};
};

}  // namespace td
