// transport_web.cpp - the browser's native WebSocket, via Emscripten's
// <emscripten/websocket.h> C API. Callbacks fire on the main thread between
// frames, so messages are simply queued for poll().
#include <emscripten/websocket.h>

#include "transport.h"

namespace td {
namespace {

class WebTransport : public Transport {
public:
    ~WebTransport() override { close(); }

    void connect(const std::string &host, int port, const std::string &path, bool secure) override {
        close();
        queue_.clear();
        error_.clear();
        generic_error_ = false;
        if (!emscripten_websocket_is_supported()) {
            state_ = State::Closed;
            error_ = "this browser has no WebSocket support";
            return;
        }
        std::string h = host.find(':') != std::string::npos ? "[" + host + "]" : host;
        url_ = std::string(secure ? "wss://" : "ws://") + h + ":" + std::to_string(port) + path;
        EmscriptenWebSocketCreateAttributes attr;
        emscripten_websocket_init_create_attributes(&attr);
        attr.url = url_.c_str();
        attr.createOnMainThread = EM_TRUE;
        ws_ = emscripten_websocket_new(&attr);
        if (ws_ <= 0) {
            state_ = State::Closed;
            error_ = "could not create WebSocket";
            return;
        }
        state_ = State::Connecting;
        emscripten_websocket_set_onopen_callback(ws_, this, on_open);
        emscripten_websocket_set_onmessage_callback(ws_, this, on_message);
        emscripten_websocket_set_onerror_callback(ws_, this, on_error);
        emscripten_websocket_set_onclose_callback(ws_, this, on_close);
    }

    void send(const uint8_t *data, size_t len) override {
        if (state_ == State::Open) emscripten_websocket_send_binary(ws_, const_cast<uint8_t *>(data), uint32_t(len));
    }

    void poll(std::vector<std::vector<uint8_t>> &out) override {
        for (auto &m : queue_) out.push_back(std::move(m));
        queue_.clear();
    }

    State state() const override { return state_; }
    std::string error() const override { return error_; }

    size_t buffered() const override {
        size_t n = 0;
        if (ws_ > 0) emscripten_websocket_get_buffered_amount(ws_, &n);
        return n;
    }

    void close() override {
        if (ws_ > 0) {
            emscripten_websocket_set_onclose_callback(ws_, nullptr, nullptr);
            emscripten_websocket_set_onmessage_callback(ws_, nullptr, nullptr);
            emscripten_websocket_set_onerror_callback(ws_, nullptr, nullptr);
            emscripten_websocket_close(ws_, 1000, "bye");
            emscripten_websocket_delete(ws_);
        }
        ws_ = 0;
        if (state_ != State::Idle) state_ = State::Closed;
    }

private:
    static EM_BOOL on_open(int, const EmscriptenWebSocketOpenEvent *, void *user) {
        static_cast<WebTransport *>(user)->state_ = State::Open;
        return EM_TRUE;
    }
    static EM_BOOL on_message(int, const EmscriptenWebSocketMessageEvent *e, void *user) {
        if (!e->isText) static_cast<WebTransport *>(user)->queue_.emplace_back(e->data, e->data + e->numBytes);
        return EM_TRUE;
    }
    static EM_BOOL on_error(int, const EmscriptenWebSocketErrorEvent *, void *user) {
        auto *t = static_cast<WebTransport *>(user);
        if (t->error_.empty())
            t->error_ = t->state_ == State::Open ? "connection lost" : "could not connect - is the host running?";
        t->generic_error_ = true;
        return EM_TRUE;
    }
    static EM_BOOL on_close(int, const EmscriptenWebSocketCloseEvent *e, void *user) {
        auto *t = static_cast<WebTransport *>(user);
        if (t->generic_error_) t->error_.clear();
        if (e->code == 4404) t->error_ = "That computer isn't online right now. Ask them to open TetherDesk and turn on sharing.";
        else if (e->code == 4408) t->error_ = "That computer didn't answer - try again in a moment.";
        if (t->error_.empty()) t->error_ = "the host closed the connection";
        t->state_ = State::Closed;
        return EM_TRUE;
    }

    EMSCRIPTEN_WEBSOCKET_T ws_ = 0;
    State state_ = State::Idle;
    std::string url_, error_;
    bool generic_error_ = false;
    std::vector<std::vector<uint8_t>> queue_;
};

}  // namespace

std::unique_ptr<Transport> Transport::create() { return std::make_unique<WebTransport>(); }

}  // namespace td
