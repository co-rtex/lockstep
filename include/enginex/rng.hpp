#pragma once

#include <cstdint>

namespace enginex {

// Portable deterministic PRNG (SplitMix64 seeding + xoshiro256**). Unlike
// std::uniform_int_distribution, results are identical on every standard library, so seeded
// workloads and property tests are reproducible across macOS/Linux and libc++/libstdc++.
class Rng {
public:
    explicit Rng(std::uint64_t seed) noexcept {
        std::uint64_t x = seed;
        for (auto& word : s_) {
            word = splitmix(x);
        }
    }

    std::uint64_t next() noexcept {
        const std::uint64_t result = rotl(s_[1] * 5U, 7) * 9U;
        const std::uint64_t t = s_[1] << 17U;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    // Uniform integer in [0, bound) without modulo bias (bound > 0).
    std::uint64_t below(std::uint64_t bound) noexcept {
        const std::uint64_t limit = UINT64_MAX - UINT64_MAX % bound;
        std::uint64_t v = next();
        while (v >= limit) {
            v = next();
        }
        return v % bound;
    }

    // Uniform integer in [lo, hi] (lo <= hi).
    std::int64_t between(std::int64_t lo, std::int64_t hi) noexcept {
        const auto span = static_cast<std::uint64_t>(hi - lo) + 1U;
        return lo + static_cast<std::int64_t>(below(span));
    }

    // True with probability percent / 100.
    bool chance(std::uint32_t percent) noexcept { return below(100) < percent; }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) noexcept {
        return (x << k) | (x >> (64 - k));
    }
    static std::uint64_t splitmix(std::uint64_t& x) noexcept {
        std::uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31U);
    }

    std::uint64_t s_[4]{};
};

}  // namespace enginex
