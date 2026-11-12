#include "enginex/types.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <system_error>
#include <utility>

namespace enginex {

namespace {

constexpr std::array<std::pair<ErrorCode, std::string_view>, 14> kErrorNames{{
    {ErrorCode::None, "NONE"},
    {ErrorCode::InvalidRequest, "INVALID_REQUEST"},
    {ErrorCode::InvalidSymbol, "INVALID_SYMBOL"},
    {ErrorCode::InvalidSide, "INVALID_SIDE"},
    {ErrorCode::InvalidPrice, "INVALID_PRICE"},
    {ErrorCode::InvalidQuantity, "INVALID_QUANTITY"},
    {ErrorCode::DuplicateRequestIdConflict, "DUPLICATE_REQUEST_ID_CONFLICT"},
    {ErrorCode::UnknownOrder, "UNKNOWN_ORDER"},
    {ErrorCode::OrderNotOpen, "ORDER_NOT_OPEN"},
    {ErrorCode::LimitExceeded, "LIMIT_EXCEEDED"},
    {ErrorCode::JournalIoError, "JOURNAL_IO_ERROR"},
    {ErrorCode::JournalCorrupt, "JOURNAL_CORRUPT"},
    {ErrorCode::ServerOverloaded, "SERVER_OVERLOADED"},
    {ErrorCode::InternalError, "INTERNAL_ERROR"},
}};

std::string format_prefixed(std::string_view prefix, std::uint64_t value) {
    std::array<char, 20> digits{};
    auto [end, ec] = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    (void)ec;  // 20 digits always suffice for uint64_t.
    std::string out;
    out.reserve(prefix.size() + static_cast<std::size_t>(end - digits.data()));
    out.append(prefix);
    out.append(digits.data(), end);
    return out;
}

}  // namespace

std::string_view to_string(Side side) noexcept {
    switch (side) {
        case Side::Buy:
            return "BUY";
        case Side::Sell:
            return "SELL";
    }
    return "INVALID";
}

std::string_view to_string(OrderStatus status) noexcept {
    switch (status) {
        case OrderStatus::Open:
            return "OPEN";
        case OrderStatus::PartiallyFilled:
            return "PARTIALLY_FILLED";
        case OrderStatus::Filled:
            return "FILLED";
        case OrderStatus::Cancelled:
            return "CANCELLED";
    }
    return "INVALID";
}

std::string_view to_string(CommandType type) noexcept {
    switch (type) {
        case CommandType::NewLimit:
            return "new_limit";
        case CommandType::Cancel:
            return "cancel";
    }
    return "invalid";
}

std::string_view to_string(ErrorCode code) noexcept {
    for (const auto& [value, name] : kErrorNames) {
        if (value == code) {
            return name;
        }
    }
    return "INTERNAL_ERROR";
}

std::optional<Side> parse_side(std::string_view text) noexcept {
    if (text == "BUY") {
        return Side::Buy;
    }
    if (text == "SELL") {
        return Side::Sell;
    }
    return std::nullopt;
}

std::optional<ErrorCode> parse_error_code(std::string_view text) noexcept {
    for (const auto& [value, name] : kErrorNames) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

std::string format_order_id(OrderId id) {
    return format_prefixed("ord-", id);
}

std::string format_execution_id(ExecSeq seq) {
    return format_prefixed("exe-", seq);
}

std::optional<OrderId> parse_order_id(std::string_view text) noexcept {
    constexpr std::string_view kPrefix = "ord-";
    if (text.size() <= kPrefix.size() || text.substr(0, kPrefix.size()) != kPrefix) {
        return std::nullopt;
    }
    const std::string_view digits = text.substr(kPrefix.size());
    if (digits.front() < '1' || digits.front() > '9') {
        return std::nullopt;  // rejects "0", leading zeros, signs and whitespace
    }
    OrderId value = 0;
    auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return value;
}

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if ((c & 0xE0U) == 0xC0U) {
            len = 2;
            cp = c & 0x1FU;
        } else if ((c & 0xF0U) == 0xE0U) {
            len = 3;
            cp = c & 0x0FU;
        } else if ((c & 0xF8U) == 0xF0U) {
            len = 4;
            cp = c & 0x07U;
        } else {
            return false;
        }
        if (i + len > n) {
            return false;
        }
        for (std::size_t k = 1; k < len; ++k) {
            const auto cc = static_cast<unsigned char>(text[i + k]);
            if ((cc & 0xC0U) != 0x80U) {
                return false;
            }
            cp = (cp << 6U) | (cc & 0x3FU);
        }
        const bool overlong =
            (len == 2 && cp < 0x80U) || (len == 3 && cp < 0x800U) || (len == 4 && cp < 0x10000U);
        if (overlong || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
            return false;
        }
        i += len;
    }
    return true;
}

}  // namespace enginex
