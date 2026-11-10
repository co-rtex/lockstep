#pragma once

// ITCH 5.0 payload decoding and file framing.
//
// Two layers, kept separate on purpose:
//
// 1. Payload decoding (`decode`): one complete ITCH message (type byte first) → typed Message.
//    Pure function of the bytes; never reads outside the given span.
//
// 2. File framing (`StreamFramer`): splits a byte stream into messages. ITCH itself defines no
//    framing; it is carried by MoldUDP64 or SoupBinTCP in production. The supported *file*
//    convention is the one Nasdaq uses for its published historical sample files: every
//    message is preceded by a 2-byte big-endian length. That length prefix carries no sequence
//    number, so this layer cannot and does not detect gaps — ITCH timestamps are not sequence
//    numbers either. Transport-level gap detection (MoldUDP64 sequence numbers) is out of scope.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

#include "enginex/itch/messages.hpp"

namespace enginex::itch {

// Why a single payload could not be decoded. None of these break the framing, so a replay can
// skip the message and continue (counted), or stop in strict mode.
enum class DecodeError : std::uint8_t {
    Empty,           // zero-length payload
    UnknownType,     // type byte is not one of kKnownTypes
    LengthMismatch,  // known type, but payload length differs from the specified length
    InvalidField,    // enumerated field with a value outside the specification (side, printable)
};
[[nodiscard]] std::string_view to_string(DecodeError e) noexcept;

using DecodeResult = std::variant<Message, DecodeError>;

[[nodiscard]] DecodeResult decode(std::span<const std::uint8_t> payload) noexcept;

// Encodes a message into its exact wire bytes (type byte first, no length prefix). Used for
// synthetic fixtures and round-trip tests; the decoder never depends on it.
[[nodiscard]] std::vector<std::uint8_t> encode(const Message& m);
// Appends the 2-byte big-endian length prefix and the encoded message to `out`.
void append_framed(std::vector<std::uint8_t>& out, const Message& m);

// Errors that end a stream: after them the message boundaries are unknown.
enum class FrameError : std::uint8_t {
    None,
    ZeroLength,      // length prefix 0: not a valid message, boundaries lost
    TruncatedTail,   // stream ended inside a length prefix or a message
};
[[nodiscard]] std::string_view to_string(FrameError e) noexcept;

// Incremental framer for the 2-byte length-prefixed file convention.
//
// feed() accepts arbitrary chunk boundaries. Complete messages inside the chunk are handed to
// the callback as spans into the chunk itself (zero copy); only a message split across chunks
// is copied into a small carry buffer (≤ 65'536 bytes). Spans are valid only during the
// callback. After a FrameError the framer stays failed and ignores further input.
class StreamFramer {
public:
    // If `on_payload` throws, the framer must not be used again.
    template <typename OnPayload>
    FrameError feed(std::span<const std::uint8_t> chunk, OnPayload&& on_payload) {
        if (error_ != FrameError::None) {
            return error_;
        }
        const std::uint64_t base = fed_;  // stream offset of chunk[0]
        fed_ += chunk.size();
        std::size_t pos = 0;
        if (!carry_.empty()) {
            // Finish the length prefix, then the message, carried over from earlier chunks.
            while (carry_.size() < 2 && pos < chunk.size()) {
                carry_.push_back(chunk[pos++]);
            }
            if (carry_.size() < 2) {
                return FrameError::None;
            }
            const std::size_t len = (static_cast<std::size_t>(carry_[0]) << 8U) | carry_[1];
            if (len == 0) {
                return fail(FrameError::ZeroLength, base + pos - 2);
            }
            const std::size_t take = std::min(2 + len - carry_.size(), chunk.size() - pos);
            carry_.insert(carry_.end(), chunk.begin() + static_cast<std::ptrdiff_t>(pos),
                          chunk.begin() + static_cast<std::ptrdiff_t>(pos + take));
            pos += take;
            if (carry_.size() < 2 + len) {
                return FrameError::None;
            }
            ++frames_;
            payload_bytes_ += len;
            on_payload(std::span<const std::uint8_t>(carry_.data() + 2, len));
            carry_.clear();
        }
        while (chunk.size() - pos >= 2) {
            const std::size_t len = (static_cast<std::size_t>(chunk[pos]) << 8U) | chunk[pos + 1];
            if (len == 0) {
                return fail(FrameError::ZeroLength, base + pos);
            }
            if (chunk.size() - pos - 2 < len) {
                break;  // the message continues in the next chunk
            }
            ++frames_;
            payload_bytes_ += len;
            on_payload(chunk.subspan(pos + 2, len));
            pos += 2 + len;
        }
        carry_.assign(chunk.begin() + static_cast<std::ptrdiff_t>(pos), chunk.end());
        return FrameError::None;
    }

    // Call at end of input: reports a truncated final message, if any.
    FrameError finish() noexcept {
        if (error_ == FrameError::None && !carry_.empty()) {
            return fail(FrameError::TruncatedTail, fed_ - carry_.size());
        }
        return error_;
    }

    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] std::uint64_t payload_bytes() const noexcept { return payload_bytes_; }
    [[nodiscard]] std::uint64_t bytes_fed() const noexcept { return fed_; }
    // Stream offset of the length prefix at which the FrameError was detected.
    [[nodiscard]] std::uint64_t error_offset() const noexcept { return error_offset_; }
    [[nodiscard]] FrameError error() const noexcept { return error_; }

private:
    FrameError fail(FrameError e, std::uint64_t offset) noexcept {
        error_ = e;
        error_offset_ = offset;
        carry_.clear();
        return e;
    }

    std::vector<std::uint8_t> carry_;  // incomplete prefix + message from earlier chunks
    FrameError error_ = FrameError::None;
    std::uint64_t fed_ = 0;
    std::uint64_t error_offset_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t payload_bytes_ = 0;
};

}  // namespace enginex::itch
