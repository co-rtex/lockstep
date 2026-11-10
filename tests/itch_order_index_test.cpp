// OrderIndex (open addressing, backward-shift deletion) checked against std::unordered_map.
#include <gtest/gtest.h>

#include <unordered_map>
#include <vector>

#include "enginex/itch/order_index.hpp"
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
    OrderIndex probe(16);
    std::vector<std::uint64_t> same_bucket;
    // Find keys colliding in a 16-slot table by brute force (bucket = top 4 bits of k*phi).
    for (std::uint64_t k = 1; same_bucket.size() < 6; ++k) {
        if (((k * 0x9E3779B97F4A7C15ULL) >> 60) == 15) {  // last bucket: chains wrap around
            same_bucket.push_back(k);
        }
    }
    for (std::size_t victim = 0; victim < same_bucket.size(); ++victim) {
        OrderIndex idx(16);
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
