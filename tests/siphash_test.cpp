// SipHash-2-4 against the reference implementation's test vectors (key 00 01 .. 0f, message
// 00 01 .. (n-1) for n = 0..63; vectors.h of the SipHash reference code, 64-bit output).
#include <gtest/gtest.h>

#include <string>

#include "enginex/siphash.hpp"
#include "enginex/state.hpp"

namespace enginex {
namespace {

const SipKey kKey{0x0706050403020100ULL, 0x0F0E0D0C0B0A0908ULL};

std::string message(std::size_t n) {
    std::string m(n, '\0');
    for (std::size_t i = 0; i < n; ++i) m[i] = static_cast<char>(i);
    return m;
}

TEST(SipHash, ReferenceVectors) {
    EXPECT_EQ(siphash24(kKey, message(0)), 0x726fdb47dd0e0e31ULL);
    EXPECT_EQ(siphash24(kKey, message(1)), 0x74f839c593dc67fdULL);
    EXPECT_EQ(siphash24(kKey, message(7)), 0xab0200f58b01d137ULL);
    EXPECT_EQ(siphash24(kKey, message(8)), 0x93f5f5799a932462ULL);
    EXPECT_EQ(siphash24(kKey, message(15)), 0xa129ca6149be45e5ULL);
    EXPECT_EQ(siphash24(kKey, message(63)), 0x958a324ceb064572ULL);
}

TEST(SipHash, KeyChangesTheHash) {
    const SipKey other{kKey.k0 ^ 1, kKey.k1};
    EXPECT_NE(siphash24(kKey, "r-123"), siphash24(other, "r-123"));
}

// Two state machines get different random keys, yet identical inputs give identical
// results: the key only decides where an id sits in the index, never what is found.
TEST(SipHash, RandomIndexKeysDoNotChangeResults) {
    ExchangeStateMachine a(StateConfig{});
    ExchangeStateMachine b(StateConfig{});
    CommandResult ra;
    CommandResult rb;
    for (int i = 0; i < 2000; ++i) {
        const Command c = NewLimitOrder{"id-" + std::to_string(i % 1500), "AAPL",
                                        i % 2 == 0 ? Side::Buy : Side::Sell,
                                        10000 + (i * 37) % 200, 1 + i % 50};
        a.process(c, ra);
        b.process(c, rb);
        ASSERT_EQ(ra.accepted, rb.accepted);
        ASSERT_EQ(ra.duplicate, rb.duplicate);
        ASSERT_EQ(ra.command_sequence, rb.command_sequence);
    }
}

}  // namespace
}  // namespace enginex
