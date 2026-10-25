#include <gtest/gtest.h>

#include <limits>

#include "enginex/checked.hpp"
#include "enginex/types.hpp"

namespace enginex {
namespace {

TEST(Types, OrderIdRoundTrip) {
    EXPECT_EQ(format_order_id(1), "ord-1");
    EXPECT_EQ(format_order_id(18446744073709551615ULL), "ord-18446744073709551615");
    EXPECT_EQ(parse_order_id("ord-1"), 1U);
    EXPECT_EQ(parse_order_id("ord-42"), 42U);
    EXPECT_EQ(parse_order_id("ord-18446744073709551615"), 18446744073709551615ULL);
    EXPECT_EQ(format_execution_id(7), "exe-7");
}

TEST(Types, OrderIdRejectsMalformed) {
    for (const char* bad : {"", "ord-", "ord-0", "ord-01", "ord--1", "ord-+1", "ord-1x", "ORD-1",
                            "exe-1", "ord- 1", "ord-18446744073709551616", "1"}) {
        EXPECT_FALSE(parse_order_id(bad).has_value()) << bad;
    }
}

TEST(Types, SideParsing) {
    EXPECT_EQ(parse_side("BUY"), Side::Buy);
    EXPECT_EQ(parse_side("SELL"), Side::Sell);
    EXPECT_FALSE(parse_side("buy").has_value());
    EXPECT_FALSE(parse_side("").has_value());
    EXPECT_EQ(to_string(Side::Buy), "BUY");
    EXPECT_EQ(opposite(Side::Buy), Side::Sell);
}

TEST(Types, ErrorCodesRoundTrip) {
    for (int i = 1; i <= static_cast<int>(ErrorCode::InternalError); ++i) {
        const auto code = static_cast<ErrorCode>(i);
        EXPECT_EQ(parse_error_code(to_string(code)), code);
    }
    EXPECT_EQ(to_string(ErrorCode::DuplicateRequestIdConflict), "DUPLICATE_REQUEST_ID_CONFLICT");
    EXPECT_EQ(to_string(ErrorCode::JournalIoError), "JOURNAL_IO_ERROR");
}

TEST(Types, Utf8Validation) {
    EXPECT_TRUE(is_valid_utf8(""));
    EXPECT_TRUE(is_valid_utf8("req-001"));
    EXPECT_TRUE(is_valid_utf8("caf\xC3\xA9"));        // é
    EXPECT_TRUE(is_valid_utf8("\xE2\x82\xAC"));       // €
    EXPECT_TRUE(is_valid_utf8("\xF0\x9F\x98\x80"));   // 😀
    EXPECT_FALSE(is_valid_utf8("\xC3"));              // truncated
    EXPECT_FALSE(is_valid_utf8("\xC0\xAF"));          // overlong
    EXPECT_FALSE(is_valid_utf8("\xED\xA0\x80"));      // surrogate
    EXPECT_FALSE(is_valid_utf8("\xF4\x90\x80\x80"));  // > U+10FFFF
    EXPECT_FALSE(is_valid_utf8("\xFF"));
    EXPECT_FALSE(is_valid_utf8("a\x80"));
}

TEST(Checked, DetectsOverflow) {
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
    EXPECT_EQ(checked_add(1, 2), 3);
    EXPECT_FALSE(checked_add(kMax, 1).has_value());
    EXPECT_FALSE(checked_add(kMin, -1).has_value());
    EXPECT_EQ(checked_sub(5, 7), -2);
    EXPECT_FALSE(checked_sub(kMin, 1).has_value());
    EXPECT_EQ(checked_mul(kMaxPrice, kMaxQuantity), 100'000'000'000'000LL);
    EXPECT_FALSE(checked_mul(kMax / 2 + 1, 2).has_value());
    EXPECT_FALSE(checked_mul(kMin, -1).has_value());
}

}  // namespace
}  // namespace enginex
