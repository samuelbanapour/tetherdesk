// transport.h - message transport used by the viewer.
//
// Both implementations deliver whole TetherDesk messages (one per WebSocket
// binary message):
//   transport_native.cpp  non-blocking TCP + rd_ws client framing
//   transport_web.cpp     the browser's WebSocket via Emscripten
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace td {

class Transport {
public:
    enum class State { Idle, Connecting, Open, Closed };

    virtual ~Transport() = default;
    virtual void connect(const std::string &host, int port) = 0;
    virtual void send(const uint8_t *data, size_t len) = 0;
    // Moves any received messages into `out` (appending). Never blocks.
    virtual void poll(std::vector<std::vector<uint8_t>> &out) = 0;
    virtual State state() const = 0;
    virtual std::string error() const = 0;
    // Bytes queued but not yet handed to the network (upload pacing).
    virtual size_t buffered() const = 0;
    virtual void close() = 0;

    static std::unique_ptr<Transport> create();
};

}  // namespace td
