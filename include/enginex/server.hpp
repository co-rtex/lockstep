#pragma once

// EngineX TCP server (R3): order entry plus a sequenced market-data feed around one sequencer.
//
//   acceptor thread ── accepts up to max_clients; extra connections get SERVER_OVERLOADED
//   per connection:  reader thread  → frames → JSON → bounded ingress queue (or OVERLOADED)
//                    writer thread  ← bounded outbound queue (acks, snapshots, events)
//   sequencer thread: the only code touching exchange state. Drains ingress in batches:
//                    stage commands (validate+apply+journal encode) → one write + one sync
//                    (group commit) → release replies and book_update events in order.
//
// Ordering: replies to one connection are delivered in request order (except immediate
// SERVER_OVERLOADED rejections, which never entered the sequencer). Events reach every
// subscriber in event_sequence order with no gaps; a subscriber that cannot keep up is
// disconnected after a final resync_required frame (DECISIONS.md D13-D17).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "enginex/durable.hpp"
#include "enginex/state.hpp"

namespace enginex::net {

struct ServerOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;  // 0 = ephemeral (see Server::port())
    // Journal settings; nullopt runs purely in memory (no durability; acks after apply).
    std::optional<DurableOptions> journal;
    StateConfig state;  // used when running in memory (journal options carry their own)
    std::size_t max_clients = 64;
    std::size_t ingress_capacity = 4096;
    std::size_t outbound_capacity = 1024;  // frames per connection
    // Bytes queued per connection (each subscriber counts a shared event frame once). A
    // connection exceeding either limit is treated as a slow consumer.
    std::size_t outbound_byte_limit = 64 * 1024 * 1024;
    std::size_t max_batch = 512;                             // commands per group commit
    std::chrono::milliseconds send_timeout{2000};            // slow-reader disconnect
    std::chrono::milliseconds partial_frame_timeout{10000};  // stalled half-frame disconnect
    // SO_SNDBUF per connection (0 = OS default). Smaller values bound the bytes buffered in the
    // kernel for a slow reader, so backpressure reaches the outbound queue sooner.
    int socket_send_buffer = 0;
    // The protocol has no authentication (spec §3.2 non-goal). Binding to anything other than a
    // loopback address is refused unless explicitly allowed.
    bool allow_remote = false;
    // Largest server→client frame (≤ protocol limit of 16 MiB). An ack or event that would be
    // larger is sent in a bounded form with "executions_truncated": true (see protocol.md).
    std::size_t max_frame_bytes = 16 * 1024 * 1024;
};

struct ServerStats {
    std::uint64_t connections_accepted = 0;
    std::uint64_t connections_rejected = 0;       // over max_clients
    std::uint64_t requests = 0;                   // frames parsed into requests
    std::uint64_t protocol_errors = 0;            // framing errors (connection closed)
    std::uint64_t overloaded = 0;                 // SERVER_OVERLOADED due to a full ingress queue
    std::uint64_t slow_consumer_disconnects = 0;  // outbound queue overflow (frames or bytes)
    std::uint64_t send_timeout_disconnects = 0;   // socket stayed unwritable past send_timeout
    std::uint64_t batches = 0;
    std::uint64_t commands_staged = 0;
    std::uint64_t events_published = 0;
    std::uint64_t max_batch_seen = 0;
    std::uint64_t subscribers = 0;       // currently registered subscribers
    std::uint64_t truncated_frames = 0;  // acks/events sent without their execution list
    std::uint64_t internal_errors = 0;   // unexpected exceptions caught in the sequencer
};

class Server {
public:
    // Recovers the journal (if configured) and binds the listener. Throws on failure.
    explicit Server(ServerOptions options);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void start();
    // Graceful shutdown: stop accepting, stop reading, process every request already queued,
    // make the final flush/sync, deliver replies (bounded by the send timeout), close all
    // connections, join every thread. Idempotent.
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] const RecoveryReport& recovery() const noexcept;
    [[nodiscard]] ServerStats stats() const;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool journal_failed() const;
    // Only valid while the server is not running (before start() or after stop()).
    [[nodiscard]] const ExchangeStateMachine& state() const;

    class Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace enginex::net
