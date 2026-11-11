#pragma once

// SipHash-2-4 (Aumasson & Bernstein, 2012): a keyed 64-bit hash for tables indexed by
// attacker-controlled strings. Client-chosen request ids index the idempotency table; with an
// unkeyed hash a client could craft ids that land in one probe cluster and turn lookups into
// linear scans. With a secret random key, collisions cannot be precomputed.
// Verified against the reference test vectors (tests/siphash_test.cpp).

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace enginex {

struct SipKey {
    std::uint64_t k0 = 0;
    std::uint64_t k1 = 0;
};

[[nodiscard]] inline std::uint64_t siphash24(const SipKey& key, std::string_view data) noexcept {
    std::uint64_t v0 = 0x736f6d6570736575ULL ^ key.k0;
    std::uint64_t v1 = 0x646f72616e646f6dULL ^ key.k1;
    std::uint64_t v2 = 0x6c7967656e657261ULL ^ key.k0;
    std::uint64_t v3 = 0x7465646279746573ULL ^ key.k1;
    auto round = [&] {
        v0 += v1;
        v1 = std::rotl(v1, 13);
        v1 ^= v0;
        v0 = std::rotl(v0, 32);
        v2 += v3;
        v3 = std::rotl(v3, 16);
        v3 ^= v2;
        v0 += v3;
        v3 = std::rotl(v3, 21);
        v3 ^= v0;
        v2 += v1;
        v1 = std::rotl(v1, 17);
        v1 ^= v2;
        v2 = std::rotl(v2, 32);
    };
    auto byte = [&](std::size_t i) { return static_cast<std::uint64_t>(static_cast<unsigned char>(data[i])); };
    const std::size_t n = data.size();
    const std::size_t whole = n - n % 8;
    for (std::size_t i = 0; i < whole; i += 8) {
        std::uint64_t m = 0;
        for (std::size_t b = 0; b < 8; ++b) m |= byte(i + b) << (8 * b);  // little-endian
        v3 ^= m;
        round();
        round();
        v0 ^= m;
    }
    std::uint64_t last = static_cast<std::uint64_t>(n & 0xFFU) << 56U;
    for (std::size_t b = 0; b < n % 8; ++b) last |= byte(whole + b) << (8 * b);
    v3 ^= last;
    round();
    round();
    v0 ^= last;
    v2 ^= 0xFFU;
    round();
    round();
    round();
    round();
    return v0 ^ v1 ^ v2 ^ v3;
}

}  // namespace enginex
