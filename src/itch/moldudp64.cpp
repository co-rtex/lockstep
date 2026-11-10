#include "enginex/itch/moldudp64.hpp"

#include <algorithm>
#include <stdexcept>

#include "enginex/itch/byte_reader.hpp"

namespace enginex::itch::mold {

std::string_view to_string(PacketError e) noexcept {
    switch (e) {
        case PacketError::TooShort: return "too_short";
        case PacketError::BlockOverrun: return "block_overrun";
        case PacketError::TrailingBytes: return "trailing_bytes";
        case PacketError::ControlWithPayload: return "control_with_payload";
        case PacketError::SequenceOverflow: return "sequence_overflow";
    }
    return "unknown";
}

std::optional<PacketError> parse(std::span<const std::uint8_t> packet, ParsedPacket& out) {
    out.messages.clear();
    if (packet.size() < kHeaderSize) {
        return PacketError::TooShort;
    }
    const ByteReader r(packet);
    out.header.session = r.alpha<10>(0);
    out.header.sequence = r.u64(10);
    out.header.count = r.u16(18);
    if (out.header.count == kHeartbeat || out.header.count == kEndOfSession) {
        if (packet.size() != kHeaderSize) {
            return PacketError::ControlWithPayload;
        }
        return std::nullopt;
    }
    if (out.header.sequence > UINT64_MAX - out.header.count) {
        return PacketError::SequenceOverflow;
    }
    std::size_t pos = kHeaderSize;
    out.messages.reserve(out.header.count);
    for (std::uint16_t i = 0; i < out.header.count; ++i) {
        if (packet.size() - pos < 2) {
            out.messages.clear();
            return PacketError::BlockOverrun;
        }
        const std::size_t len = r.u16(pos);
        pos += 2;
        if (packet.size() - pos < len) {
            out.messages.clear();
            return PacketError::BlockOverrun;
        }
        out.messages.push_back(packet.subspan(pos, len));
        pos += len;
    }
    if (pos != packet.size()) {
        out.messages.clear();
        return PacketError::TrailingBytes;
    }
    return std::nullopt;
}

namespace {

std::vector<std::uint8_t> header_bytes(const Session& s, std::uint64_t seq, std::uint16_t count) {
    std::vector<std::uint8_t> out(kHeaderSize);
    std::copy(s.begin(), s.end(), out.begin());
    put_be(out.data() + 10, seq, 8);
    put_be(out.data() + 18, count, 2);
    return out;
}

}  // namespace

std::vector<std::uint8_t> build_packet(const Session& session, std::uint64_t sequence,
                                       const std::vector<std::vector<std::uint8_t>>& messages) {
    if (messages.empty() || messages.size() >= kEndOfSession) {
        throw std::invalid_argument("MoldUDP64 data packet needs 1..65534 messages");
    }
    std::vector<std::uint8_t> out =
        header_bytes(session, sequence, static_cast<std::uint16_t>(messages.size()));
    for (const auto& m : messages) {
        if (m.size() > 0xFFFF) {
            throw std::invalid_argument("MoldUDP64 message longer than 65535 bytes");
        }
        out.push_back(static_cast<std::uint8_t>(m.size() >> 8U));
        out.push_back(static_cast<std::uint8_t>(m.size() & 0xFFU));
        out.insert(out.end(), m.begin(), m.end());
    }
    return out;
}

std::vector<std::uint8_t> build_control(const Session& session, std::uint64_t next_sequence,
                                        std::uint16_t count) {
    if (count != kHeartbeat && count != kEndOfSession) {
        throw std::invalid_argument("control packet count must be 0 or 0xFFFF");
    }
    return header_bytes(session, next_sequence, count);
}

std::vector<std::uint8_t> build_request(const Session& session, std::uint64_t sequence,
                                        std::uint16_t count) {
    return header_bytes(session, sequence, count);
}

Session make_session(std::string_view text) {
    Session s;
    s.fill(' ');
    std::copy_n(text.begin(), std::min(text.size(), s.size()), s.begin());
    return s;
}

Sequencer::Sequencer(Options options)
    : o_(options), next_(options.first_sequence), horizon_(options.first_sequence) {}

void Sequencer::note_horizon(std::uint64_t end) { horizon_ = std::max(horizon_, end); }

std::vector<std::pair<std::uint64_t, std::uint64_t>> Sequencer::missing() const {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    std::uint64_t cursor = next_;
    for (const auto& [seq, bytes] : pending_) {
        if (seq > cursor) {
            out.emplace_back(cursor, seq - cursor);
        }
        cursor = seq + 1;
    }
    if (cursor < horizon_) {
        out.emplace_back(cursor, horizon_ - cursor);
    }
    return out;
}

}  // namespace enginex::itch::mold
