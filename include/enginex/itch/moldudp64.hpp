#pragma once

// MoldUDP64 transport: packet parsing and message sequencing with gap detection.
//
// Layout (all integers big-endian). Sources: the MoldUDP64 protocol specification as published by
// Nasdaq (moldudp64.pdf), cross-checked against the FINRA and Japannext copies of the same
// specification and the Wireshark dissector (docs/itch.md § MoldUDP64):
//
//   downstream packet   offset  size
//     session              0     10   alpha, identifies the session
//     sequence number     10      8   sequence number of the first message in the packet
//     message count       18      2   0 = heartbeat, 0xFFFF = end of session
//     message blocks      20      …   repeated `count` times: u16 length + `length` bytes
//   request packet (to a re-request server): session(10), sequence(8), requested count(2)
//
// Messages are numbered implicitly: the i-th block of a packet with sequence S is message S+i.
// Heartbeats and end-of-session packets carry the next expected sequence number, so they can
// reveal a gap even when no data packet follows.
//
// This is where gap detection belongs: ITCH payloads have no sequence numbers of their own.

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace enginex::itch::mold {

inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::uint16_t kHeartbeat = 0;
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;
using Session = std::array<char, 10>;

struct PacketHeader {
    Session session{};
    std::uint64_t sequence = 0;
    std::uint16_t count = 0;
};

enum class PacketError : std::uint8_t {
    TooShort,            // fewer than 20 bytes
    BlockOverrun,        // a block length runs past the end of the packet
    TrailingBytes,       // bytes left after `count` blocks
    ControlWithPayload,  // heartbeat / end-of-session packet that carries bytes
    SequenceOverflow,    // sequence + count would wrap the 64-bit space
};
[[nodiscard]] std::string_view to_string(PacketError e) noexcept;

struct ParsedPacket {
    PacketHeader header;
    std::vector<std::span<const std::uint8_t>> messages;  // views into the packet bytes
};

// Validates the whole packet before returning any message: a malformed packet yields an error
// and no messages (never a partial delivery).
[[nodiscard]] std::optional<PacketError> parse(std::span<const std::uint8_t> packet,
                                               ParsedPacket& out);

// Builds a downstream packet (tests, simulators). Throws if the blocks do not fit 65'535.
[[nodiscard]] std::vector<std::uint8_t> build_packet(
    const Session& session, std::uint64_t sequence,
    const std::vector<std::vector<std::uint8_t>>& messages);
[[nodiscard]] std::vector<std::uint8_t> build_control(const Session& session,
                                                      std::uint64_t next_sequence,
                                                      std::uint16_t count);
[[nodiscard]] std::vector<std::uint8_t> build_request(const Session& session,
                                                      std::uint64_t sequence,
                                                      std::uint16_t count);
[[nodiscard]] Session make_session(std::string_view text);

// Turns packets (possibly duplicated, reordered or lost) into the exact message sequence.
//
// * Messages are released strictly in sequence order, each exactly once.
// * Duplicates and already-delivered parts of overlapping packets are dropped (counted).
// * A packet ahead of the next expected sequence is a gap. Its messages are held in a bounded
//   reorder buffer; missing ranges are reported for re-request. When the hole is filled
//   (reordered packet or retransmission), buffered messages are released in order.
// * If a buffer limit (messages or bytes) would be exceeded, or a packet starts more than
//   `max_ahead` beyond the next expected sequence, the gap is declared unrecoverable: the sequencer
//   stops releasing anything (Failed) instead of skipping messages. Consumers that maintain
//   state (an order book) must then rebuild from a snapshot or the start of the session.
// * Packets from a different session are rejected (counted); a session is fixed by the first
//   packet seen.
class Sequencer {
public:
    enum class State : std::uint8_t { Running, Gap, Ended, Failed };

    struct Options {
        std::uint64_t first_sequence = 1;               // sequence numbers start at 1
        std::size_t max_buffered_messages = 65536;      // reorder buffer bound (messages)
        std::size_t max_buffered_bytes = 64U << 20U;    // reorder buffer bound (payload bytes)
        // A packet whose messages would start more than this far beyond the next expected
        // sequence is treated as unrecoverable (a huge loss and a corrupt header look alike).
        std::uint64_t max_ahead = 1'000'000;
    };

    struct Stats {
        std::uint64_t packets = 0;
        std::uint64_t heartbeats = 0;
        std::uint64_t delivered = 0;
        std::uint64_t duplicates = 0;          // messages dropped as already delivered/buffered
        std::uint64_t gaps_opened = 0;
        std::uint64_t gaps_closed = 0;
        std::uint64_t buffered_peak = 0;
        std::uint64_t malformed_packets = 0;
        std::uint64_t foreign_session_packets = 0;
        std::uint64_t buffered_bytes_peak = 0;
    };

    explicit Sequencer(Options options);
    Sequencer() : Sequencer(Options{}) {}

    // Feeds one UDP payload. `deliver(sequence, message)` is called for every message that
    // becomes deliverable, in order. Message spans are valid only during the call.
    template <typename Deliver>
    void on_packet(std::span<const std::uint8_t> packet, Deliver&& deliver) {
        ParsedPacket p;
        if (parse(packet, p)) {
            ++stats_.malformed_packets;
            return;
        }
        on_parsed(p, deliver);
    }

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t next_expected() const noexcept { return next_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t buffered() const noexcept { return buffered_count_; }
    // Missing ranges [first, first + count) the consumer should re-request, in order.
    [[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>> missing() const;
    // Highest sequence seen so far + 1 (from data or control packets).
    [[nodiscard]] std::uint64_t horizon() const noexcept { return horizon_; }

private:
    template <typename Deliver>
    void on_parsed(const ParsedPacket& p, Deliver& deliver);
    void note_horizon(std::uint64_t end);

    Options o_;
    State state_ = State::Running;
    std::optional<Session> session_;
    std::uint64_t next_;
    std::uint64_t horizon_;
    bool ended_ = false;
    std::uint64_t end_sequence_ = 0;
    // Messages ahead of next_, keyed by sequence (copies; the packet buffer is transient).
    std::map<std::uint64_t, std::vector<std::uint8_t>> pending_;
    std::size_t buffered_count_ = 0;
    std::size_t buffered_bytes_ = 0;
    Stats stats_;
};

template <typename Deliver>
void Sequencer::on_parsed(const ParsedPacket& p, Deliver& deliver) {
    if (state_ == State::Failed) {
        return;
    }
    if (!session_) {
        session_ = p.header.session;
    } else if (*session_ != p.header.session) {
        ++stats_.foreign_session_packets;
        return;
    }
    ++stats_.packets;
    const PacketHeader& h = p.header;
    if (h.sequence > next_ && h.sequence - next_ > o_.max_ahead) {
        state_ = State::Failed;  // too far ahead to ever be filled by buffering
        return;
    }
    if (h.count == kHeartbeat || h.count == kEndOfSession) {
        // Control packets carry the next expected sequence number.
        if (h.count == kHeartbeat) {
            ++stats_.heartbeats;
        } else {
            ended_ = true;
            end_sequence_ = h.sequence;
        }
        note_horizon(h.sequence);
    } else {
        const std::uint64_t first = h.sequence;
        note_horizon(first + p.messages.size());
        for (std::size_t i = 0; i < p.messages.size(); ++i) {
            const std::uint64_t seq = first + i;
            if (seq < next_ || pending_.contains(seq)) {
                ++stats_.duplicates;
                continue;
            }
            if (seq == next_) {
                deliver(seq, p.messages[i]);
                ++stats_.delivered;
                ++next_;
                continue;
            }
            if (buffered_count_ >= o_.max_buffered_messages ||
                p.messages[i].size() > o_.max_buffered_bytes - buffered_bytes_) {
                state_ = State::Failed;  // unrecoverable: never skip messages
                return;
            }
            pending_.emplace(seq, std::vector<std::uint8_t>(p.messages[i].begin(),
                                                            p.messages[i].end()));
            ++buffered_count_;
            buffered_bytes_ += p.messages[i].size();
            stats_.buffered_peak = std::max<std::uint64_t>(stats_.buffered_peak, buffered_count_);
            stats_.buffered_bytes_peak =
                std::max<std::uint64_t>(stats_.buffered_bytes_peak, buffered_bytes_);
        }
    }
    // Release buffered messages that are now contiguous.
    for (auto it = pending_.begin(); it != pending_.end() && it->first == next_;) {
        deliver(it->first, std::span<const std::uint8_t>(it->second));
        ++stats_.delivered;
        ++next_;
        buffered_bytes_ -= it->second.size();
        it = pending_.erase(it);
        --buffered_count_;
    }
    const bool was_gap = state_ == State::Gap;
    if (next_ < horizon_) {
        if (!was_gap) {
            ++stats_.gaps_opened;
        }
        state_ = State::Gap;
    } else {
        if (was_gap) {
            ++stats_.gaps_closed;
        }
        state_ = ended_ && next_ >= end_sequence_ ? State::Ended : State::Running;
    }
}

}  // namespace enginex::itch::mold
