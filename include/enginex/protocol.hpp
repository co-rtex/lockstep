#pragma once

// EngineX wire protocol v1 (spec §10, docs/protocol.md).
//
// Transport: TCP byte stream; every message is one frame
//   [u32 big-endian payload_length][payload_length bytes of UTF-8 JSON]
// Client→server frames are limited to 32 KiB; server→client frames to 16 MiB (a full-depth
// subscribe snapshot can be large; see DECISIONS.md D17). Every JSON message carries
// "protocol_version": 1.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "enginex/json_codec.hpp"
#include "enginex/state.hpp"

namespace enginex::net {

inline constexpr int kProtocolVersion = 1;
inline constexpr std::size_t kFrameHeaderBytes = 4;
inline constexpr std::size_t kMaxClientFrame = 32 * 1024;
inline constexpr std::size_t kMaxServerFrame = 16 * 1024 * 1024;

// ---- framing --------------------------------------------------------------------------------

// Prefixes `payload` with its big-endian length. Throws std::length_error if it exceeds `max`.
[[nodiscard]] std::string encode_frame(std::string_view payload, std::size_t max = kMaxServerFrame);

// Incremental decoder: tolerates arbitrary segmentation (one byte at a time, many frames per
// read). A declared length of 0 or above the limit is a fatal stream error, detected from the
// 4-byte header alone, before any payload allocation.
class FrameDecoder {
public:
    enum class Status : std::uint8_t { NeedMore, Frame, Error };

    explicit FrameDecoder(std::size_t max_frame) : max_frame_(max_frame) {}

    void feed(const char* data, std::size_t size);
    // Extracts the next complete frame into `payload` (Frame), or reports NeedMore / Error.
    Status next(std::string& payload);

    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    // True when bytes of an incomplete frame are buffered.
    [[nodiscard]] bool has_partial() const noexcept { return buffer_.size() > offset_; }
    [[nodiscard]] std::size_t max_frame() const noexcept { return max_frame_; }

private:
    std::size_t max_frame_;
    std::string buffer_;
    std::size_t offset_ = 0;
    bool failed_ = false;
    std::string error_;
};

// ---- client → server --------------------------------------------------------------------

struct SnapshotRequest {
    std::string request_id;                            // optional correlation id, echoed back
    std::optional<std::string> symbol;                 // nullopt = every symbol
    std::size_t depth = codec::kDefaultSnapshotDepth;  // 0 = full depth
};

struct SubscribeRequest {
    std::string request_id;  // optional correlation id, echoed in the snapshot
};

using ClientMessage = std::variant<Command, SnapshotRequest, SubscribeRequest>;

// Parses one client frame. Errors carry stable codes; a wrong or missing protocol_version is
// INVALID_REQUEST.
[[nodiscard]] codec::Parsed<ClientMessage> parse_client_message(std::string_view json);

// ---- server → client (JSON text, not yet framed) -------------------------------------------

// "ack" for accepted results (including duplicates), "error" for rejections.
[[nodiscard]] std::string result_message(const CommandResult& result,
                                         const ExchangeStateMachine& state);
[[nodiscard]] std::string error_message(ErrorCode code, std::string_view message,
                                        std::string_view request_id = {},
                                        std::string_view command_type = {});
[[nodiscard]] std::string error_message(const codec::ParseError& error);

// "snapshot_response": aggregated books (one symbol or all) at `event_watermark`.
[[nodiscard]] std::string snapshot_message(const ExchangeStateMachine& state,
                                           std::string_view request_id,
                                           std::optional<SymbolId> symbol, std::size_t depth,
                                           EventSeq event_watermark, bool subscribed);

// "book_update" for an accepted, non-duplicate command. Must be built immediately after the
// command was applied: it reads the post-command aggregates of every price level it touched.
[[nodiscard]] std::string book_update_message(const ExchangeStateMachine& state,
                                              const CommandResult& result, EventSeq event_sequence);

// "resync_required": the subscriber's feed ends here; reconnect and subscribe again.
[[nodiscard]] std::string resync_message(std::string_view reason, EventSeq last_event_queued);

}  // namespace enginex::net
