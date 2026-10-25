#pragma once

// Blocking EngineX protocol client used by tests, the `enginex client` CLI and the network
// benchmark. One instance per connection; not thread-safe.

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "enginex/json_codec.hpp"
#include "enginex/protocol.hpp"
#include "enginex/socket.hpp"

namespace enginex::net {

class Client {
public:
    // Throws std::runtime_error when the connection cannot be established.
    static Client connect(const std::string& host, std::uint16_t port,
                          std::chrono::milliseconds timeout = std::chrono::seconds(5));

    // Sends one JSON message as a frame; "protocol_version": 1 is added when missing.
    // Returns false if the connection failed.
    bool send(codec::Json message);
    // Sends raw bytes (tests: fragmentation, malformed frames).
    bool send_raw(std::string_view bytes);

    // Next message, waiting up to `timeout`. nullopt on timeout or once the server closed the
    // connection (check closed()). Throws std::runtime_error on an invalid server frame.
    std::optional<codec::Json> receive(std::chrono::milliseconds timeout);

    // Sends a request and returns the next message whose request_id matches, skipping and
    // returning nothing else (book_update events are dropped). nullopt on timeout/close.
    std::optional<codec::Json> request(codec::Json message, std::chrono::milliseconds timeout);

    void shutdown_write();
    void close();
    [[nodiscard]] bool closed() const noexcept { return closed_; }

private:
    explicit Client(Socket socket);

    Socket socket_;
    FrameDecoder decoder_{kMaxServerFrame};
    bool closed_ = false;
};

}  // namespace enginex::net
