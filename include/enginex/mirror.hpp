#pragma once

// FeedMirror: a read-only replica of the synthetic exchange's aggregated books, built from the
// protocol-v1 market-data feed (subscribe snapshot + book_update events).
//
// It is a pure state machine over received messages (no I/O), so every failure path can be
// driven from tests. The rule it enforces: increments are applied only on top of a valid
// baseline and only when contiguous.
//
//   AwaitingBaseline ──subscribe snapshot──► Live ──gap / resync_required / disconnect──► Stale
//         ▲                                   │                                           │
//         └──────────── resubscribe (new connection) ◄───────────────────────────────────┘
//
//   * A book_update while not Live is rejected and never applied (counted).
//   * A book_update whose event_sequence is not last + 1 is a gap: nothing is applied and the
//     mirror goes Stale. The protocol promises no gaps, so a gap means loss or a bug; the only
//     recovery is a fresh snapshot (snapshot-based resynchronization, protocol.md §5).
//   * An error reply to `subscribe` (e.g. snapshot too large, fail-stop) leaves it not Live
//     (a Live mirror that receives one goes Stale).
//   * A book_update for a symbol absent from the baseline, or any malformed message, makes it
//     Stale; updates are parsed completely before any level changes (no partial application).
//   * Verification: a snapshot_response with "subscribed": false, received on the *same*
//     connection while Live, was serialized at event_watermark W after every event ≤ W had
//     been queued to this connection (one per-connection queue, sequencer order). So its W must
//     equal the mirror's last event and its levels must equal the mirror's top levels. Any
//     difference is a mismatch and makes the mirror Stale.
//
// This is the synthetic exchange's JSON feed. It has nothing to do with the ITCH path.

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "enginex/json_codec.hpp"

namespace enginex::net {

class FeedMirror {
public:
    enum class State : std::uint8_t { AwaitingBaseline, Live, Stale };
    enum class Verdict : std::uint8_t {
        Baseline,         // subscribe snapshot loaded
        Applied,          // contiguous book_update applied
        Rejected,         // book_update while not Live: ignored, not applied
        Gap,              // non-contiguous book_update: not applied, now Stale
        ResyncRequired,   // server ended the feed: now Stale
        SubscribeFailed,  // error reply to subscribe
        Verified,         // in-band snapshot equals the mirror
        Mismatch,         // in-band snapshot differs: now Stale
        Ignored,          // anything else (acks, unrelated errors, snapshots while not Live)
        Malformed,        // a server message with missing/mistyped fields: nothing applied, Stale
    };

    struct Stats {
        std::uint64_t baselines = 0;
        std::uint64_t applied = 0;
        std::uint64_t rejected = 0;
        std::uint64_t gaps = 0;
        std::uint64_t resyncs = 0;
        std::uint64_t subscribe_failures = 0;
        std::uint64_t verified = 0;
        std::uint64_t mismatches = 0;
        std::uint64_t disconnects = 0;
        std::uint64_t malformed = 0;
        std::uint64_t connect_failures = 0;
    };

    Verdict on_message(const codec::Json& m);
    // The connection ended (closed, reset, timed out). A Live mirror becomes Stale.
    void on_disconnect();
    // Start over on a new connection: the next subscribe snapshot is the only way back to Live.
    void reset_for_resubscribe();
    // A connection could not be made or broke with an error (counted only).
    void note_connection_failure();

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t last_event() const noexcept { return last_event_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    // Why the mirror last went Stale or failed verification (human-readable).
    [[nodiscard]] const std::string& last_problem() const noexcept { return problem_; }
    // Deterministic text form of all books (symbol, side, price, quantity, order count).
    [[nodiscard]] std::string canonical() const;

private:
    using Key = std::pair<char, std::int64_t>;  // ('B'|'S', price)
    using Level = std::pair<std::int64_t, std::int64_t>;  // quantity, order count
    using Book = std::map<Key, Level>;

    Verdict handle(const codec::Json& m);
    Verdict stale(Verdict v, std::string why);
    Verdict verify(const codec::Json& snapshot);
    static Book load_book(const codec::Json& b);

    State state_ = State::AwaitingBaseline;
    std::uint64_t last_event_ = 0;
    std::map<std::string, Book> books_;
    Stats stats_;
    std::string problem_;
};

// A mirror session over TCP: connect, subscribe, apply events, verify in band every
// `verify_every`, and on Stale (gap, resync_required, disconnect, mismatch) or a failed
// subscribe reconnect and resubscribe, up to `max_resubscribes` times. Ends after `duration`
// with one final in-band verification.
struct MirrorOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;
    std::chrono::milliseconds duration{5000};
    std::chrono::milliseconds verify_every{500};
    std::size_t max_resubscribes = 10;  // reconnect attempts after the first connection
    // Testing aid: stop reading for this long right after the first baseline (simulates a
    // stalled consumer so the server's slow-consumer path can be exercised).
    std::chrono::milliseconds stall_after_first_baseline{0};
};

struct MirrorReport {
    FeedMirror::Stats stats;
    std::uint64_t connections = 0;
    std::uint64_t last_event = 0;
    bool final_verified = false;  // the closing in-band verification succeeded
    std::string state;
    std::string last_problem;
    // No gap was ever accepted (by construction), no mismatch was seen, the session ended Live
    // and verified.
    [[nodiscard]] bool ok() const noexcept {
        return stats.mismatches == 0 && final_verified && state == "live";
    }
};

[[nodiscard]] MirrorReport run_mirror(const MirrorOptions& options, FeedMirror& mirror);

[[nodiscard]] std::string_view to_string(FeedMirror::State s) noexcept;
[[nodiscard]] std::string_view to_string(FeedMirror::Verdict v) noexcept;

}  // namespace enginex::net
