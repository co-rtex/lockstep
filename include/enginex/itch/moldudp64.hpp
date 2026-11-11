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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
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
                                                      std::uint64_t sequence, std::uint16_t count);
[[nodiscard]] Session make_session(std::string_view text);

// A request packet sent to a re-request server: exactly 20 bytes, count in 1..65534.
struct Request {
    Session session{};
    std::uint64_t sequence = 0;
    std::uint16_t count = 0;
};
[[nodiscard]] std::optional<Request> parse_request(std::span<const std::uint8_t> packet) noexcept;

// A message stream cut into MTU-bounded downstream packets, numbered from sequence 1: what a
// sender transmits and what a re-request server answers from. Packets are contiguous, so
// find() is a binary search. Messages are copied into the packets.
class PacketStore {
public:
    PacketStore(const Session& session, std::size_t mtu);

    // Adds the next message. A message that does not fit an empty packet within the MTU is
    // sent alone (the MTU is a target, not a hard limit); one over 65 535 bytes throws.
    void append(std::span<const std::uint8_t> message);
    // Closes the open packet. Call once after the last append().
    void finish();

    [[nodiscard]] std::size_t size() const noexcept { return packets_.size(); }
    [[nodiscard]] const std::vector<std::uint8_t>& packet(std::size_t i) const {
        return packets_[i];
    }
    [[nodiscard]] std::uint64_t first(std::size_t i) const { return first_[i]; }
    [[nodiscard]] std::uint16_t count(std::size_t i) const { return count_[i]; }
    // Sequence number after the last message (the end-of-session packet carries it).
    [[nodiscard]] std::uint64_t end_sequence() const noexcept { return next_; }
    [[nodiscard]] const Session& session() const noexcept { return session_; }
    // Index of the packet that holds message `sequence`, if any.
    [[nodiscard]] std::optional<std::size_t> find(std::uint64_t sequence) const noexcept;

private:
    void flush();

    Session session_;
    std::size_t mtu_;
    std::uint64_t next_ = 1;
    std::vector<std::vector<std::uint8_t>> packets_;
    std::vector<std::uint64_t> first_;
    std::vector<std::uint16_t> count_;
    std::vector<std::vector<std::uint8_t>> batch_;
    std::size_t batch_bytes_ = kHeaderSize;
};

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
// * Packets from a different session are rejected (counted); the session is the configured
//   one, or else is fixed by the first packet seen.
class Sequencer {
public:
    enum class State : std::uint8_t { Running, Gap, Ended, Failed };
    // Why the sequencer entered Failed: which bound a gap outlasted.
    enum class FailReason : std::uint8_t {
        None,
        TooFarAhead,       // a packet started more than max_ahead past the next expected sequence
        BufferedMessages,  // max_buffered_messages reached behind a hole
        BufferedBytes,     // the packet copies would exceed max_buffered_bytes
    };

    struct Options {
        std::uint64_t first_sequence = 1;           // sequence numbers start at 1
        std::size_t max_buffered_messages = 65536;  // reorder buffer bound (messages)
        // Reorder buffer bound in bytes: buffered messages are held in copies of their packets,
        // and those copies count (a copy can hold duplicates that came in the same packet).
        std::size_t max_buffered_bytes = 64U << 20U;
        // A packet whose messages would start more than this far beyond the next expected
        // sequence is treated as unrecoverable (a huge loss and a corrupt header look alike).
        std::uint64_t max_ahead = 1'000'000;
        // Expected session. Without it the first packet seen fixes the session, so a stray
        // packet from another session that arrives first would lock out the real feed.
        std::optional<Session> session;
    };

    struct Stats {
        std::uint64_t packets = 0;
        std::uint64_t heartbeats = 0;
        std::uint64_t delivered = 0;
        std::uint64_t duplicates = 0;  // messages dropped as already delivered/buffered
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
    [[nodiscard]] FailReason fail_reason() const noexcept { return fail_reason_; }
    [[nodiscard]] std::uint64_t next_expected() const noexcept { return next_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t buffered() const noexcept { return buffered_count_; }
    // Missing ranges [first, first + count) the consumer should re-request, in order.
    [[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>> missing() const;
    // Highest sequence seen so far + 1 (from data or control packets).
    [[nodiscard]] std::uint64_t horizon() const noexcept { return horizon_; }
    // The session fixed by the first packet (none before any packet).
    [[nodiscard]] const std::optional<Session>& session() const noexcept { return session_; }

private:
    // A buffered message: where its bytes are in a packet copy. kNoChunk = not present.
    static constexpr std::uint32_t kNoChunk = UINT32_MAX;
    struct Slot {
        std::uint32_t chunk = kNoChunk;
        std::uint32_t offset = 0;
        std::uint32_t length = 0;
    };
    // A copy of one packet's message blocks, shared by its buffered messages and recycled
    // (capacity kept) when the last of them is released.
    struct Chunk {
        std::vector<std::uint8_t> bytes;
        std::size_t charged = 0;  // capacity counted in chunk_bytes_ while live
        std::uint32_t live = 0;
    };

    template <typename Deliver>
    void on_parsed(const ParsedPacket& p, Deliver& deliver);
    void note_horizon(std::uint64_t end);
    void open_hole(std::uint64_t end);                // [horizon_, end) is missing; horizon_ = end
    [[nodiscard]] bool fill_hole(std::uint64_t seq);  // false if seq is not in a hole
    // Copies the packet's message blocks into a chunk, or returns kNoChunk if the capacity
    // it would hold exceeds the byte bound.
    std::uint32_t copy_packet(const ParsedPacket& p);
    void release_chunk(std::uint32_t id);

    Options o_;
    State state_ = State::Running;
    FailReason fail_reason_ = FailReason::None;
    std::optional<Session> session_;
    std::uint64_t next_;
    std::uint64_t horizon_;
    bool ended_ = false;
    std::uint64_t end_sequence_ = 0;
    // Messages ahead of next_: window_[k] is sequence next_ + k (holes are empty slots).
    // Buffering costs one packet copy per packet, not one allocation per message.
    std::deque<Slot> window_;
    std::vector<Chunk> chunks_;
    std::vector<std::uint32_t> free_chunks_;
    // Missing ranges [first, end) inside [next_, horizon_), disjoint and never adjacent. Only
    // touched when a hole opens or a retransmission fills one, so missing() is O(holes).
    std::map<std::uint64_t, std::uint64_t> holes_;
    std::size_t buffered_count_ = 0;
    std::size_t buffered_bytes_ = 0;  // payload bytes of buffered messages (statistics)
    std::size_t chunk_bytes_ = 0;     // capacity held by live packet copies (≤ max_buffered_bytes)
    std::size_t pooled_bytes_ = 0;    // capacity kept by free chunks (≤ max_buffered_bytes / 4)
    Stats stats_;
};

// "" for None, else too_far_ahead | buffered_messages_bound | buffered_bytes_bound.
[[nodiscard]] std::string_view to_string(Sequencer::FailReason r) noexcept;

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
        fail_reason_ = FailReason::TooFarAhead;
        return;
    }
    if (h.count == kHeartbeat || h.count == kEndOfSession) {
        // Control packets carry the next expected sequence number.
        if (h.count == kHeartbeat) {
            ++stats_.heartbeats;
        } else if (h.sequence >= horizon_) {
            // An end of session below messages already seen contradicts them (stale or bogus):
            // ignoring it keeps the sequencer from reporting Ended while the feed is live.
            ended_ = true;
            end_sequence_ = h.sequence;
        }
        note_horizon(h.sequence);
    } else {
        const std::uint64_t first = h.sequence;
        if (first > horizon_) {
            open_hole(first);  // messages [horizon_, first) exist but have not arrived
        }
        std::uint32_t chunk = kNoChunk;  // this packet's copy, made on first use
        for (std::size_t i = 0; i < p.messages.size(); ++i) {
            const std::uint64_t seq = first + i;
            if (seq < next_) {
                ++stats_.duplicates;
                continue;
            }
            if (seq < horizon_) {
                if (!fill_hole(seq)) {  // [next_, horizon_) = holes + buffered messages
                    ++stats_.duplicates;
                    continue;
                }
            } else {
                horizon_ = seq + 1;  // seq == horizon_: first <= horizon_ after open_hole()
            }
            if (seq == next_) {
                deliver(seq, p.messages[i]);
                ++stats_.delivered;
                ++next_;
                if (!window_.empty()) {
                    window_.pop_front();  // the slot of the message just delivered
                }
                continue;
            }
            if (buffered_count_ >= o_.max_buffered_messages ||
                p.messages[i].size() > o_.max_buffered_bytes - buffered_bytes_) {
                state_ = State::Failed;  // unrecoverable: never skip messages
                fail_reason_ = buffered_count_ >= o_.max_buffered_messages
                                   ? FailReason::BufferedMessages
                                   : FailReason::BufferedBytes;
                return;
            }
            if (chunk == kNoChunk) {
                chunk = copy_packet(p);
                if (chunk == kNoChunk) {
                    state_ = State::Failed;  // the copy would exceed the byte bound
                    fail_reason_ = FailReason::BufferedBytes;
                    return;
                }
            }
            const auto k = static_cast<std::size_t>(seq - next_);
            if (k >= window_.size()) {
                window_.resize(k + 1);
            }
            window_[k] = Slot{
                chunk, static_cast<std::uint32_t>(p.messages[i].data() - p.messages.front().data()),
                static_cast<std::uint32_t>(p.messages[i].size())};
            ++chunks_[chunk].live;
            ++buffered_count_;
            buffered_bytes_ += p.messages[i].size();
            stats_.buffered_peak = std::max<std::uint64_t>(stats_.buffered_peak, buffered_count_);
            stats_.buffered_bytes_peak =
                std::max<std::uint64_t>(stats_.buffered_bytes_peak, buffered_bytes_);
        }
    }
    // Release buffered messages that are now contiguous.
    while (!window_.empty() && window_.front().chunk != kNoChunk) {
        const Slot slot = window_.front();
        Chunk& c = chunks_[slot.chunk];
        deliver(next_, std::span<const std::uint8_t>(c.bytes.data() + slot.offset, slot.length));
        ++stats_.delivered;
        ++next_;
        window_.pop_front();
        buffered_bytes_ -= slot.length;
        --buffered_count_;
        if (--c.live == 0) {
            release_chunk(slot.chunk);
        }
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
