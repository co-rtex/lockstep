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

std::optional<Request> parse_request(std::span<const std::uint8_t> packet) noexcept {
    if (packet.size() != kHeaderSize) {
        return std::nullopt;
    }
    const ByteReader r(packet);
    Request q;
    q.session = r.alpha<10>(0);
    q.sequence = r.u64(10);
    q.count = r.u16(18);
    if (q.count == 0 || q.count == kEndOfSession || q.sequence == 0 ||
        q.sequence > UINT64_MAX - q.count) {
        return std::nullopt;
    }
    return q;
}

PacketStore::PacketStore(const Session& session, std::size_t mtu) : session_(session), mtu_(mtu) {}

void PacketStore::append(std::span<const std::uint8_t> message) {
    if (message.size() > 0xFFFF) {
        throw std::invalid_argument("MoldUDP64 message longer than 65535 bytes");
    }
    if (!batch_.empty() &&
        (batch_bytes_ + 2 + message.size() > mtu_ || batch_.size() + 1 >= kEndOfSession)) {
        flush();
    }
    batch_bytes_ += 2 + message.size();
    batch_.emplace_back(message.begin(), message.end());
}

void PacketStore::finish() { flush(); }

void PacketStore::flush() {
    if (batch_.empty()) {
        return;
    }
    packets_.push_back(build_packet(session_, next_, batch_));
    first_.push_back(next_);
    count_.push_back(static_cast<std::uint16_t>(batch_.size()));
    next_ += batch_.size();
    batch_.clear();
    batch_bytes_ = kHeaderSize;
}

std::optional<std::size_t> PacketStore::find(std::uint64_t sequence) const noexcept {
    if (first_.empty() || sequence < first_.front() || sequence >= next_) {
        return std::nullopt;
    }
    // The last packet whose first sequence is <= `sequence`.
    const auto it = std::upper_bound(first_.begin(), first_.end(), sequence);
    return static_cast<std::size_t>(std::distance(first_.begin(), it) - 1);
}

Sequencer::Sequencer(Options options)
    : o_(options),
      session_(options.session),
      next_(options.first_sequence),
      horizon_(options.first_sequence) {}

void Sequencer::note_horizon(std::uint64_t end) {
    if (end > horizon_) {
        open_hole(end);
    }
}

void Sequencer::open_hole(std::uint64_t end) {
    // The new hole starts at horizon_; only the last hole can end there (merge, so holes
    // stay maximal and missing() reports one range per hole).
    if (!holes_.empty() && std::prev(holes_.end())->second == horizon_) {
        std::prev(holes_.end())->second = end;
    } else {
        holes_.emplace(horizon_, end);
    }
    horizon_ = end;
}

bool Sequencer::fill_hole(std::uint64_t seq) {
    auto it = holes_.upper_bound(seq);
    if (it == holes_.begin()) {
        return false;
    }
    --it;
    const auto [start, end] = *it;
    if (seq >= end) {
        return false;
    }
    holes_.erase(it);
    if (start < seq) {
        holes_.emplace(start, seq);
    }
    if (seq + 1 < end) {
        holes_.emplace(seq + 1, end);
    }
    return true;
}

std::uint32_t Sequencer::copy_packet(const ParsedPacket& p) {
    // The copy spans all of the packet's blocks (duplicates included): one new message must
    // not pin a large packet for free, so the capacity actually held counts against the bound.
    const std::uint8_t* base = p.messages.front().data();
    const std::uint8_t* last = p.messages.back().data() + p.messages.back().size();
    const auto span = static_cast<std::size_t>(last - base);
    const std::size_t room = o_.max_buffered_bytes - chunk_bytes_;
    std::uint32_t id = kNoChunk;
    if (!free_chunks_.empty()) {
        id = free_chunks_.back();
        std::vector<std::uint8_t>& old = chunks_[id].bytes;
        const std::size_t cap = old.capacity();
        // Reuse the old allocation only if it is not much larger than needed.
        if (cap < span || cap > 2 * span + 4096 || cap > room) {
            pooled_bytes_ -= cap;
            std::vector<std::uint8_t>().swap(old);
        }
    }
    const std::size_t held =
        id != kNoChunk ? std::max(span, chunks_[id].bytes.capacity()) : span;
    if (held > room) {
        return kNoChunk;
    }
    if (id != kNoChunk) {
        free_chunks_.pop_back();
        pooled_bytes_ -= chunks_[id].bytes.capacity();
    } else {
        id = static_cast<std::uint32_t>(chunks_.size());
        chunks_.emplace_back();
    }
    Chunk& c = chunks_[id];
    c.bytes.assign(base, last);
    c.live = 0;
    c.charged = c.bytes.capacity();
    chunk_bytes_ += c.charged;
    return id;
}

void Sequencer::release_chunk(std::uint32_t id) {
    Chunk& c = chunks_[id];
    chunk_bytes_ -= c.charged;
    c.charged = 0;
    if (pooled_bytes_ + c.bytes.capacity() <= o_.max_buffered_bytes / 4) {
        pooled_bytes_ += c.bytes.capacity();  // keep the capacity for the next packet copy
        c.bytes.clear();
    } else {
        std::vector<std::uint8_t>().swap(c.bytes);  // keep the pool itself bounded
    }
    free_chunks_.push_back(id);
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> Sequencer::missing() const {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    out.reserve(holes_.size());
    for (const auto& [start, end] : holes_) {
        out.emplace_back(start, end - start);
    }
    return out;
}

}  // namespace enginex::itch::mold
