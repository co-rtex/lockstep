#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "enginex/state.hpp"

namespace enginex {

class Sha256;

// Minimal deterministic JSON writer for canonical bytes. Keys are emitted in call order; strings
// are escaped (", \, control characters); non-ASCII UTF-8 passes through unchanged.
class CanonicalWriter {
public:
    // With a sink, completed output is streamed into the hash in chunks instead of retained.
    explicit CanonicalWriter(Sha256* sink = nullptr) noexcept : sink_(sink) {}
    void begin_object();
    void end_object();
    void begin_array();
    void end_array();
    void key(std::string_view name);
    void value(std::string_view text);
    void value(const char* text) { value(std::string_view{text}); }
    void value(std::int64_t number);
    void value(std::uint64_t number);
    void value(std::uint32_t number) { value(static_cast<std::uint64_t>(number)); }
    void value(bool flag);

    [[nodiscard]] const std::string& str() const noexcept { return out_; }
    [[nodiscard]] std::string take() noexcept { return std::move(out_); }
    // Streams any buffered output into the sink (no-op without a sink).
    void flush();

private:
    void separator();

    Sha256* sink_ = nullptr;
    std::string out_;
    bool need_comma_ = false;
};

// Deterministic serialization of the full replay-relevant state (spec §8.2): sequence
// watermarks, per-symbol books (bids descending, asks ascending, FIFO within a level), every
// accepted command with its outcome (the idempotency map, in sequence order), and every
// execution in execution order. Contains no wall-clock times, addresses or hash-order output.
[[nodiscard]] std::string canonical_state(const ExchangeStateMachine& state);

// Hex SHA-256 of canonical_state(state). Used for replay/recovery identity checks only.
[[nodiscard]] std::string state_hash(const ExchangeStateMachine& state);

}  // namespace enginex
