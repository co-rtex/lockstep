#pragma once

// Replays a length-prefixed ITCH 5.0 file through the framer, decoder and book reconstructor.
//
// Error policy:
//   * framing errors (zero-length prefix, truncated final message) are fatal: message
//     boundaries after them are unknown;
//   * decode errors (unknown type, wrong length, invalid field) and book anomalies are counted
//     and the message is skipped, unless `strict`, in which case replay stops at the first one.
// The result reports which of these ended the replay, with the 1-based message number.

#include <cstdint>
#include <functional>
#include <istream>
#include <optional>
#include <span>
#include <string>

#include "enginex/itch/book.hpp"
#include "enginex/itch/decoder.hpp"

namespace enginex::itch {

struct ReplayOptions {
    bool strict = false;
    std::size_t chunk_bytes = 1U << 20U;  // read size for streams
    // Optional observer called after each decoded message is applied.
    std::function<void(const Message&, const ApplyOutcome&)> on_message;
};

struct ReplayResult {
    std::uint64_t bytes = 0;
    std::uint64_t frames = 0;
    std::uint64_t decoded = 0;
    std::array<std::uint64_t, 4> decode_errors{};  // indexed by DecodeError
    std::array<std::uint64_t, 256> unknown_types{};
    FrameError frame_error = FrameError::None;
    std::uint64_t frame_error_offset = 0;
    // Set when replay stopped early (framing error, or strict mode). Human-readable.
    std::optional<std::string> stopped;
    std::uint64_t stopped_at_message = 0;  // 1-based frame number, 0 if not stopped

    [[nodiscard]] bool ok() const noexcept { return !stopped; }
    [[nodiscard]] std::uint64_t decode_error_count() const noexcept {
        return decode_errors[0] + decode_errors[1] + decode_errors[2] + decode_errors[3];
    }
};

ReplayResult replay(std::span<const std::uint8_t> file, BookBuilder& book,
                    const ReplayOptions& options = {});
ReplayResult replay(std::istream& in, BookBuilder& book, const ReplayOptions& options = {});

}  // namespace enginex::itch
