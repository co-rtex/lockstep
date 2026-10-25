#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace enginex {

// All prices and quantities are fixed-width signed integers. Prices are US cents.
using PriceCents = std::int64_t;
using Quantity = std::int64_t;
using CommandSeq = std::uint64_t;
using ExecSeq = std::uint64_t;
using EventSeq = std::uint64_t;

// An order is identified by the command sequence that accepted it ("ord-<seq>").
using OrderId = std::uint64_t;
// Index into the configured symbol catalog.
using SymbolId = std::uint32_t;

// R1 domain limits (spec §6.1).
inline constexpr PriceCents kMinPrice = 1;
inline constexpr PriceCents kMaxPrice = 100'000'000;
inline constexpr Quantity kMinQuantity = 1;
inline constexpr Quantity kMaxQuantity = 1'000'000;
inline constexpr std::size_t kMaxRequestIdBytes = 64;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

enum class OrderStatus : std::uint8_t { Open = 0, PartiallyFilled = 1, Filled = 2, Cancelled = 3 };

enum class CommandType : std::uint8_t { NewLimit = 1, Cancel = 2 };

// Stable machine-readable error codes (spec §6.2). Never parse descriptions in clients.
enum class ErrorCode : std::uint8_t {
    None = 0,
    InvalidRequest,
    InvalidSymbol,
    InvalidSide,
    InvalidPrice,
    InvalidQuantity,
    DuplicateRequestIdConflict,
    UnknownOrder,
    OrderNotOpen,
    LimitExceeded,
    JournalIoError,
    JournalCorrupt,
    ServerOverloaded,
    InternalError,
};

[[nodiscard]] std::string_view to_string(Side side) noexcept;
[[nodiscard]] std::string_view to_string(OrderStatus status) noexcept;
[[nodiscard]] std::string_view to_string(CommandType type) noexcept;
[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

[[nodiscard]] std::optional<Side> parse_side(std::string_view text) noexcept;
[[nodiscard]] std::optional<ErrorCode> parse_error_code(std::string_view text) noexcept;

[[nodiscard]] constexpr Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

[[nodiscard]] constexpr bool is_valid_side(Side side) noexcept {
    return side == Side::Buy || side == Side::Sell;
}

// Opaque external identifiers. The core stores integers; these helpers are the only place
// where the textual form is defined.
[[nodiscard]] std::string format_order_id(OrderId id);
[[nodiscard]] std::string format_execution_id(ExecSeq seq);
// Parses "ord-<n>" with n in [1, 2^64-1], no leading zeros, no sign. Returns nullopt otherwise.
[[nodiscard]] std::optional<OrderId> parse_order_id(std::string_view text) noexcept;

// True when `text` is well-formed UTF-8 (no overlongs, surrogates or code points > U+10FFFF).
[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept;

}  // namespace enginex
