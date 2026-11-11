// OrderIndex (open addressing, backward-shift deletion) checked against std::unordered_map.
#include <gtest/gtest.h>

#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "enginex/itch/order_index.hpp"
#include "enginex/itch/paged_index.hpp"
#include "enginex/rng.hpp"

namespace enginex::itch {
namespace {

void check_same(const OrderIndex& idx, const std::unordered_map<std::uint64_t, std::uint32_t>& ref,
                const std::vector<std::uint64_t>& probe_keys) {
    ASSERT_EQ(idx.size(), ref.size());
    for (const std::uint64_t k : probe_keys) {
        const auto it = ref.find(k);
        ASSERT_EQ(idx.find(k), it == ref.end() ? OrderIndex::kEmpty : it->second) << k;
    }
    std::size_t visited = 0;
    idx.for_each([&](std::uint64_t k, std::uint32_t v) {
        ++visited;
        ASSERT_EQ(ref.at(k), v);
    });
    ASSERT_EQ(visited, ref.size());
}

TEST(OrderIndex, BasicOperations) {
    OrderIndex idx(16);
    EXPECT_EQ(idx.find(0), OrderIndex::kEmpty);
    EXPECT_TRUE(idx.insert(0, 7));  // key 0 is a valid key
    EXPECT_FALSE(idx.insert(0, 8));
    EXPECT_EQ(idx.find(0), 7U);
    EXPECT_TRUE(idx.erase(0));
    EXPECT_FALSE(idx.erase(0));
    EXPECT_EQ(idx.size(), 0U);
    EXPECT_THROW(idx.insert(1, OrderIndex::kEmpty), std::invalid_argument);
}

// Random interleavings with a small key space (many collisions and re-inserts) and a small
// initial capacity (growth, wraparound at the end of the table, long probe chains).
TEST(OrderIndex, MatchesUnorderedMapUnderRandomOperations) {
    for (const std::uint64_t space : {8ULL, 64ULL, 1000ULL, 1ULL << 40}) {
        Rng rng(space);
        OrderIndex idx(16);
        std::unordered_map<std::uint64_t, std::uint32_t> ref;
        std::vector<std::uint64_t> keys;
        for (int i = 0; i < 60'000; ++i) {
            const std::uint64_t k = rng.below(space) * (space > 1000 ? 0x10001ULL : 1);
            keys.push_back(k);
            const auto op = rng.below(10);
            if (op < 5) {
                const auto v = static_cast<std::uint32_t>(rng.below(1'000'000));
                ASSERT_EQ(idx.insert(k, v), ref.emplace(k, v).second);
            } else if (op < 9) {
                ASSERT_EQ(idx.erase(k), ref.erase(k) == 1);
            } else {
                const auto it = ref.find(k);
                ASSERT_EQ(idx.find(k), it == ref.end() ? OrderIndex::kEmpty : it->second);
            }
            if (i % 4999 == 0) {
                check_same(idx, ref, keys);
            }
        }
        check_same(idx, ref, keys);
    }
}

TEST(OrderIndex, ExtremeKeysAndMixedHomeChains) {
    OrderIndex idx(16);
    EXPECT_TRUE(idx.insert(UINT64_MAX, 1));
    EXPECT_TRUE(idx.insert(0, 2));
    EXPECT_EQ(idx.find(UINT64_MAX), 1U);
    // Keys whose home buckets are 14 and 15 interleave in one wrapped chain; deleting any of
    // them must not move an entry before its own home.
    const SipKey fixed{0x0123456789abcdefULL, 0xfedcba9876543210ULL};
    const OrderIndex probe(16, fixed);
    std::vector<std::uint64_t> keys;
    for (std::uint64_t k = 1; keys.size() < 6; ++k) {
        const auto home = probe.home_bucket(k);
        if (home == 14 || home == 15) keys.push_back(k);
    }
    for (std::size_t victim = 0; victim < keys.size(); ++victim) {
        OrderIndex t(16, fixed);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            ASSERT_TRUE(t.insert(keys[i], static_cast<std::uint32_t>(i)));
        }
        ASSERT_TRUE(t.erase(keys[victim]));
        for (std::size_t i = 0; i < keys.size(); ++i) {
            ASSERT_EQ(t.find(keys[i]), i == victim ? OrderIndex::kEmpty : i) << victim << " " << i;
        }
    }
}

TEST(OrderIndex, ReserveAndClear) {
    OrderIndex idx(16);
    idx.reserve(1000);
    const std::size_t cap = idx.capacity();
    EXPECT_GE(cap, 2000U);
    for (std::uint32_t i = 0; i < 1000; ++i) {
        ASSERT_TRUE(idx.insert(i * 7919ULL, i));
    }
    EXPECT_EQ(idx.capacity(), cap);  // no growth below the reserved count
    idx.reserve(10);                 // never shrinks
    EXPECT_EQ(idx.capacity(), cap);
    EXPECT_EQ(idx.find(999 * 7919ULL), 999U);
    idx.clear();
    EXPECT_EQ(idx.size(), 0U);
    EXPECT_EQ(idx.find(999 * 7919ULL), OrderIndex::kEmpty);
    EXPECT_TRUE(idx.insert(5, 5));
}

// Keys that hash to the same bucket chain, deleted in every order: backward shift must keep
// all remaining keys reachable.
TEST(OrderIndex, DeletionInsideCollisionChains) {
    std::vector<std::uint64_t> same_bucket;
    // Find keys colliding in a 16-slot table by brute force, under a fixed hash key.
    const SipKey fixed{1, 2};
    const OrderIndex probe(16, fixed);
    for (std::uint64_t k = 1; same_bucket.size() < 6; ++k) {
        if (probe.home_bucket(k) == 15) {  // last bucket: chains wrap around
            same_bucket.push_back(k);
        }
    }
    for (std::size_t victim = 0; victim < same_bucket.size(); ++victim) {
        OrderIndex idx(16, fixed);
        for (std::size_t i = 0; i < same_bucket.size(); ++i) {
            ASSERT_TRUE(idx.insert(same_bucket[i], static_cast<std::uint32_t>(i)));
        }
        ASSERT_EQ(idx.capacity(), 16U);  // 6 keys stay below the 1/2 load factor
        ASSERT_TRUE(idx.erase(same_bucket[victim]));
        for (std::size_t i = 0; i < same_bucket.size(); ++i) {
            EXPECT_EQ(idx.find(same_bucket[i]),
                      i == victim ? OrderIndex::kEmpty : static_cast<std::uint32_t>(i))
                << "victim " << victim << " key " << i;
        }
    }
}

}  // namespace
}  // namespace enginex::itch

// ---- PagedIndex ------------------------------------------------------------------------------

namespace enginex::itch {
namespace {

// Keys mix dense recent references, scattered old ones, and huge (≥ 2^32) values; a tiny page
// budget forces the hash fallback while pages are created and freed.
TEST(PagedIndex, MatchesUnorderedMapIncludingFallbackAndPageFreeing) {
    for (const std::size_t budget : {std::size_t{2}, std::size_t{64}, std::size_t{16384}}) {
        Rng rng(budget);
        PagedIndex idx(budget);
        std::unordered_map<std::uint64_t, std::uint32_t> ref;
        std::vector<std::uint64_t> keys;
        std::uint64_t next = 1;
        for (int i = 0; i < 200'000; ++i) {
            std::uint64_t k = 0;
            switch (rng.below(10)) {
                case 0: k = rng.next(); break;                          // anywhere, often huge
                case 1: k = rng.below(1 << 20); break;                  // old, scattered
                default: k = next + rng.below(64); next += rng.below(3);  // dense, increasing
            }
            if (!keys.empty() && rng.chance(40)) k = keys[rng.below(keys.size())];
            keys.push_back(k);
            if (rng.chance(55)) {
                const auto v = static_cast<std::uint32_t>(rng.below(1'000'000));
                ASSERT_EQ(idx.insert(k, v), ref.emplace(k, v).second) << k;
            } else {
                ASSERT_EQ(idx.erase(k), ref.erase(k) == 1) << k;
            }
            ASSERT_EQ(idx.size(), ref.size());
            ASSERT_LE(idx.pages(), budget);
            if (i % 9973 == 0) {
                for (const std::uint64_t q : keys) {
                    const auto it = ref.find(q);
                    ASSERT_EQ(idx.find(q), it == ref.end() ? PagedIndex::kEmpty : it->second) << q;
                }
            }
        }
        if (budget == 2) {
            EXPECT_GT(idx.fallback_size(), 0U);  // the budget really forced the fallback
        }
    }
}

TEST(PagedIndex, BoundaryKeysAndDirectoryGrowth) {
    PagedIndex idx;
    const std::uint64_t last_direct = PagedIndex::kDirectLimit - 1;
    EXPECT_TRUE(idx.insert(last_direct, 1));  // highest page: directory grows from empty
    EXPECT_TRUE(idx.insert(PagedIndex::kDirectLimit, 2));         // first fallback key
    EXPECT_TRUE(idx.insert(PagedIndex::kDirectLimit + 4095, 3));  // fallback too
    EXPECT_EQ(idx.pages(), 1U);
    EXPECT_EQ(idx.fallback_size(), 2U);
    EXPECT_EQ(idx.find(last_direct), 1U);
    EXPECT_EQ(idx.find(PagedIndex::kDirectLimit), 2U);
    EXPECT_EQ(idx.find(PagedIndex::kDirectLimit + 4095), 3U);
    EXPECT_EQ(idx.find(last_direct - 1), PagedIndex::kEmpty);
    EXPECT_TRUE(idx.erase(last_direct));
    EXPECT_EQ(idx.pages(), 0U);
    EXPECT_EQ(idx.size(), 2U);
}

TEST(PagedIndex, KeyInFallbackStaysFindableWhenItsPageAppearsAndGoes) {
    PagedIndex idx(1);                 // one page allowed
    EXPECT_TRUE(idx.insert(10, 1));    // page 0 created
    EXPECT_TRUE(idx.insert(5000, 2));  // page 1 over budget -> fallback
    EXPECT_EQ(idx.fallback_size(), 1U);
    EXPECT_TRUE(idx.erase(10));        // page 0 freed (kept as spare)
    EXPECT_TRUE(idx.insert(5001, 3));  // page 1 now created, from the spare
    EXPECT_EQ(idx.pages(), 1U);
    EXPECT_EQ(idx.find(5000), 2U);     // still found in the fallback behind an existing page
    EXPECT_EQ(idx.find(5001), 3U);
    EXPECT_FALSE(idx.insert(5000, 9)); // no duplicate across page and fallback
    EXPECT_TRUE(idx.erase(5000));
    EXPECT_EQ(idx.find(5000), PagedIndex::kEmpty);
    EXPECT_EQ(idx.fallback_size(), 0U);
    EXPECT_TRUE(idx.erase(5001));
    EXPECT_EQ(idx.size(), 0U);
}

TEST(PagedIndex, PagesAreFreedWhenEmptyAndKeyZeroWorks) {
    PagedIndex idx;
    EXPECT_TRUE(idx.insert(0, 1));
    EXPECT_TRUE(idx.insert(5000, 2));  // second page
    EXPECT_EQ(idx.pages(), 2U);
    EXPECT_TRUE(idx.erase(5000));
    EXPECT_EQ(idx.pages(), 1U);
    EXPECT_EQ(idx.find(0), 1U);
    EXPECT_TRUE(idx.insert(UINT64_MAX, 3));  // beyond the direct range
    EXPECT_EQ(idx.fallback_size(), 1U);
    EXPECT_EQ(idx.find(UINT64_MAX), 3U);
    EXPECT_FALSE(idx.insert(0, 9));
    EXPECT_TRUE(idx.erase(0));
    EXPECT_EQ(idx.pages(), 0U);
    EXPECT_THROW(idx.insert(1, PagedIndex::kEmpty), std::invalid_argument);
}

// References crafted against the old unkeyed multiplicative hash, (C + i) * phi^-1 mod 2^64:
// every one of them had the same home bucket at every table size, so 40 000 such adds cost
// 43 µs each (D34). Under the keyed hash they spread like any other keys.
// The key, not just the reference, decides placement, and each default index draws its own
// key. Otherwise an attacker who learns one placement (or the code) can predict them all.
// The collision tests above use one fixed key for probe and index alike, so they would still
// pass if the key were ignored or constant.
TEST(OrderIndex, TheKeyDecidesPlacementAndEachIndexDrawsItsOwn) {
    auto homes = [](const OrderIndex& idx) {
        std::vector<std::size_t> h;
        for (std::uint64_t k = 1; k <= 64; ++k) {
            h.push_back(idx.home_bucket(k));
        }
        return h;
    };
    const OrderIndex a(1024, SipKey{1, 2});
    EXPECT_EQ(homes(a), homes(OrderIndex(1024, SipKey{1, 2})));    // deterministic per key
    EXPECT_NE(homes(a), homes(OrderIndex(1024, SipKey{1, 3})));    // a different key moves them
    EXPECT_NE(homes(OrderIndex{}), homes(OrderIndex{}));          // 1024^-64 chance to be equal
    EXPECT_NE(OrderIndex::random_key().k0, OrderIndex::random_key().k0);  // 2^-64 chance
}

TEST(OrderIndex, CraftedReferencesDoNotShareAHomeBucket) {
    constexpr std::uint64_t kPhi = 0x9E3779B97F4A7C15ULL;
    std::uint64_t inverse = kPhi;  // Newton iteration for phi^-1 mod 2^64 (phi is odd)
    for (int i = 0; i < 6; ++i) {
        inverse *= 2 - kPhi * inverse;
    }
    ASSERT_EQ(kPhi * inverse, 1U);
    constexpr std::uint64_t kCount = 20'000;
    OrderIndex idx;
    std::vector<std::uint64_t> keys;
    for (std::uint64_t i = 0; i < kCount; ++i) {
        keys.push_back(((std::uint64_t{1} << 40U) + i) * inverse);
        // The attack premise: all share the top bits the old hash used.
        ASSERT_EQ((keys.back() * kPhi) >> 40U, 1U);
        ASSERT_TRUE(idx.insert(keys.back(), static_cast<std::uint32_t>(i)));
    }
    std::unordered_map<std::size_t, int> homes;
    for (const std::uint64_t k : keys) {
        ++homes[idx.home_bucket(k)];
    }
    // 20 000 keys over 65 536 buckets: about 17 000 distinct homes expected, never one.
    EXPECT_GT(homes.size(), kCount / 2);
    for (std::uint64_t i = 0; i < kCount; ++i) {
        ASSERT_EQ(idx.find(keys[i]), i);
    }
}

}  // namespace
}  // namespace enginex::itch
