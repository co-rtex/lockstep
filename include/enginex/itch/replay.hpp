#pragma once

// Replays a length-prefixed ITCH 5.0 file through the framer, decoder and book reconstructor.
//
// Error policy:
//   * framing errors (zero-length prefix, truncated final message) are fatal: message
//     boundaries after them are unknown;
//   * decode errors (unknown type, wrong length, invalid field) are counted and the frame is
//     skipped; book anomalies are counted (rejected messages leave the book unchanged;
//     SymbolMismatch/DirectoryConflict are applied and only counted);
//   * with `strict`, replay stops at the first decode error or anomaly of any kind.
// Replay stops at the *first* problem. The result then describes the stop point only: frames
// and bytes after it are neither processed nor counted, and later framing errors are not
// examined.

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
    std::uint64_t bytes_read = 0;       // input bytes read (may exceed bytes_processed)
    std::uint64_t bytes_processed = 0;  // through the end of the last processed frame
    std::uint64_t frames = 0;           // frames processed (decoded or counted as errors)
    std::uint64_t decoded = 0;
    std::array<std::uint64_t, 4> decode_errors{};  // indexed by DecodeError
    std::array<std::uint64_t, 256> unknown_types{};
    FrameError frame_error = FrameError::None;
    std::uint64_t frame_error_offset = 0;
    // Set when replay stopped early (framing error, strict mode, read error). Human-readable.
    std::optional<std::string> stopped;
    // 1-based number of the frame that caused the stop (for a framing error: the frame that
    // could not be read). 0 if not stopped, or stopped by a read error.
    std::uint64_t stopped_at_message = 0;

    [[nodiscard]] bool ok() const noexcept { return !stopped; }
    [[nodiscard]] std::uint64_t decode_error_count() const noexcept {
        return decode_errors[0] + decode_errors[1] + decode_errors[2] + decode_errors[3];
    }
};

ReplayResult replay(std::span<const std::uint8_t> file, BookBuilder& book,
                    const ReplayOptions& options = {});
ReplayResult replay(std::istream& in, BookBuilder& book, const ReplayOptions& options = {});

}  // namespace enginex::itch
