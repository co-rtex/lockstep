#pragma once

// MoldUDP64 over real UDP sockets (IPv4): a sender that also answers re-requests, and a
// receiver that sequences packets, re-requests the ranges it is missing and rebuilds the book.
//
// Scope: unicast, one session, one sender process acting as both the feed and its re-request
// server (MoldUDP64 deployments usually separate the two and use multicast). Loss is real
// (deliberate drops at the sender, plus whatever the kernel drops when a socket buffer
// overflows). It has been exercised on loopback only: it is a lab transport, not a production
// feed handler (no multicast join, no kernel-bypass, no failover between feeds).
//
// Re-request protocol: the receiver sends a 20-byte request (session, first sequence, count)
// to the sender. The sender answers with the stored data packets that cover the range, sent
// to the request's source address. Answers are capped per request (no amplification beyond
// `max_request_messages`). Requests for another session or out of range are counted and ignored.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <netinet/in.h>

#include "enginex/itch/moldudp64.hpp"
#include "enginex/rng.hpp"
#include "enginex/socket.hpp"

namespace enginex::itch::mold::udp {

struct Endpoint {
    std::string host = "127.0.0.1";  // IPv4 literal
    std::uint16_t port = 0;          // 0 = ephemeral (bind only)
};

struct SenderOptions {
    Endpoint bind;                      // local socket (also where requests arrive)
    Endpoint dest;                      // receiver
    std::uint32_t drop_percent = 0;     // deliberately not sent on the live feed
    std::uint32_t retransmit_drop_percent = 0;  // deliberately not sent when answering
    std::uint64_t seed = 1;
    std::uint64_t packets_per_second = 100'000;  // live-feed pacing; 0 = unpaced
    std::chrono::milliseconds end_interval{20};  // end-of-session repeat while lingering
    // After the live feed, keep answering requests until none has arrived for this long.
    std::chrono::milliseconds linger{1000};
    std::uint32_t max_request_messages = 4096;   // cap per request
    // Anti-reflection: answer only requests from the receiver's IP address (`dest.host`), and
    // send at most this many answer packets per second overall (token bucket). A 20-byte
    // request can otherwise trigger ~95 answer packets to a spoofed source.
    bool answer_any_source = false;
    std::uint64_t max_answer_packets_per_second = 50'000;
};

struct SenderReport {
    std::uint64_t packets = 0;           // live-feed packets in the store
    std::uint64_t sent = 0;              // live-feed packets actually sent
    std::uint64_t dropped = 0;           // live-feed packets deliberately not sent
    std::uint64_t requests = 0;          // valid requests answered
    std::uint64_t bad_requests = 0;      // malformed, foreign session or out of range
    std::uint64_t foreign_requests = 0;  // from an address other than the receiver's
    std::uint64_t answers_rate_limited = 0;  // packets not sent: answer budget exhausted
    std::uint64_t retransmitted = 0;     // packets sent in answers
    std::uint64_t retransmit_dropped = 0;
    std::uint64_t end_packets = 0;
    std::uint64_t send_errors = 0;       // sendto failures (e.g. ENOBUFS), treated as loss
};

class Sender {
public:
    // Binds the request socket and connects the feed socket. Throws std::runtime_error.
    Sender(const PacketStore& store, SenderOptions options);
    [[nodiscard]] std::uint16_t port() const;
    // Sends the feed, then the end of session, answering requests throughout. Returns when
    // the linger time passes with no request, or when `stop` becomes true.
    SenderReport run(const std::atomic<bool>* stop = nullptr);

private:
    struct Peer {
        sockaddr_in addr;
        net::Socket sock;  // connected to `addr`, bound to the request port
    };
    static constexpr std::size_t kMaxPeers = 16;

    void serve_requests(int timeout_ms, SenderReport& r);
    void answer(std::span<const std::uint8_t> datagram, const sockaddr_in& from, int via_fd,
                SenderReport& r);
    Peer* peer_for(const sockaddr_in& from);

    const PacketStore& store_;
    SenderOptions o_;
    net::Socket sock_;  // bound: receives requests, sends answers and the end of session
    net::Socket feed_;  // connected to `dest`: the live feed (a connected send() is several
                        // times cheaper than sendto() here, see docs/itch.md)
    Rng rng_;
    std::chrono::steady_clock::time_point last_request_;
    std::vector<Peer> peers_;  // append-only: indices stay valid while one poll round runs
    in_addr receiver_ip_{};
    double answer_tokens_ = 0;
    std::chrono::steady_clock::time_point tokens_at_;
};

// Decides which missing ranges to request in one round. A range already requested less
// than `retry_after` ago is not asked again, even after part of its answer has arrived and
// the hole has shrunk or split (the answer for the rest may still be in flight). Pure: no
// sockets, so it is unit-tested directly.
class RequestPlanner {
public:
    using Clock = std::chrono::steady_clock;
    RequestPlanner(std::chrono::milliseconds retry_after, std::size_t max_ranges,
                   std::uint32_t max_messages)
        : retry_after_(retry_after), max_ranges_(max_ranges), max_messages_(max_messages) {}

    // Returns (first, count) requests for `missing` (ranges at or after `next_expected`).
    std::vector<std::pair<std::uint64_t, std::uint16_t>> plan(
        const std::vector<std::pair<std::uint64_t, std::uint64_t>>& missing,
        std::uint64_t next_expected, Clock::time_point now);

private:
    struct Asked {
        std::uint64_t end;
        Clock::time_point at;
    };
    std::chrono::milliseconds retry_after_;
    std::size_t max_ranges_;
    std::uint32_t max_messages_;
    std::map<std::uint64_t, Asked> asked_;  // requested [first, end), keyed by first
};

struct ReceiverOptions {
    Endpoint bind;
    Endpoint request_to;                          // the sender's address (set its port later)
    // Feed datagrams are accepted only from this IP (default: request_to.host). This is a
    // filter, not authentication: the transport has none (as MoldUDP64 itself has none).
    std::string feed_from;
    bool accept_any_source = false;
    bool recovery = true;                         // send re-requests for missing() ranges
    // At most one request round per interval, each for up to `max_ranges_per_round` holes.
    // Recovery keeps up only if ranges/interval exceeds the rate at which holes open (live
    // loss × packet rate): 512 per 5 ms covers ≈ 100 k holes/s.
    std::chrono::milliseconds request_interval{5};
    std::size_t max_ranges_per_round = 512;
    // A hole already requested is asked again only after this long (its answer may be in
    // flight). Without it every round re-requests every hole and the answers snowball.
    std::chrono::milliseconds retry_after{50};
    std::uint32_t max_request_messages = 4096;    // count per request
    // Stop after this long without a datagram, or, while a gap is open, without accepting a
    // new message (heartbeats and repeated end-of-session packets do not count).
    std::chrono::milliseconds idle_timeout{2000};
    int receive_buffer_bytes = 4 << 20;           // SO_RCVBUF request (the OS may cap it)
    Sequencer::Options sequencer;
};

struct ReceiverReport {
    std::uint64_t packets = 0;          // datagrams received
    std::uint64_t bytes = 0;
    std::uint64_t request_rounds = 0;
    std::uint64_t requests_sent = 0;
    std::uint64_t delivered = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t malformed = 0;
    std::uint64_t foreign_session = 0;
    std::uint64_t foreign_source = 0;   // datagrams from an address other than the feed's
    std::uint64_t gaps_opened = 0;
    std::uint64_t gaps_closed = 0;
    std::uint64_t buffered_peak = 0;
    bool in_order = true;               // every delivery was the next sequence, exactly once
    std::string final_state;            // running | gap | ended | failed
    std::string failure;                // when failed: which bound (Sequencer::FailReason)
    std::uint64_t next_expected = 0;
    bool timed_out = false;
    // Gave up on an open gap: no new message was accepted for `idle_timeout` (timed_out too).
    bool gap_stalled = false;
    double seconds = 0;
};

class Receiver {
public:
    // Binds the socket. Throws std::runtime_error.
    explicit Receiver(ReceiverOptions options);
    [[nodiscard]] std::uint16_t port() const;
    void set_request_port(std::uint16_t port) { o_.request_to.port = port; }
    // Receives until the session ends, the sequencer fails, the idle timeout expires or
    // `stop` becomes true. `deliver(sequence, message)` sees every message once, in order.
    ReceiverReport run(const std::function<void(std::uint64_t, std::span<const std::uint8_t>)>& deliver,
                       const std::atomic<bool>* stop = nullptr);

private:
    ReceiverOptions o_;
    net::Socket sock_;
};

// True for 127.0.0.0/8 literals (the CLI refuses other addresses without --allow-remote).
[[nodiscard]] bool is_loopback(const std::string& host);

}  // namespace enginex::itch::mold::udp
