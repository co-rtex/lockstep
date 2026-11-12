#pragma once

// Open-addressing hash map from ITCH order reference → 32-bit pool slot.
//
// Replaces std::unordered_map in the book's hot path (measured in docs/itch.md): no allocation
// per insert/erase, one contiguous slot array, linear probing over a keyed hash (below), and
// backward-shift deletion so there
// are no tombstones and probe sequences never degrade. Load factor ≤ 1/2; growth doubles the
// table and reinserts (amortized O(1); the one-off pause is visible in tail latency, and
// `reserve()` avoids it when the order count is known).
//
// Keys are arbitrary 64-bit values (0 included). Values must be < kEmpty.
//
// The bucket is SipHash-2-4 of the key under a random per-instance key (D34). Order references
// come from the input file, and the earlier unkeyed multiplicative hash could be inverted:
// references (C + i) * phi^-1 all share one home bucket at every table size, which made
// inserts and misses linear scans (measured: 40 000 such adds cost 43 µs each, quadratic in
// total). Placement never affects results; nothing iterates the table for output.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "enginex/siphash.hpp"

namespace enginex::itch {

class OrderIndex {
public:
    static constexpr std::uint32_t kEmpty = UINT32_MAX;

    explicit OrderIndex(std::size_t capacity = 1024) : OrderIndex(capacity, random_key()) {}
    // A fixed hash key: tests use it to build collision chains deterministically.
    OrderIndex(std::size_t capacity, SipKey key) : key_(key) { rebuild(round_up(capacity)); }

    // A secret key from the OS entropy source. Fails closed: std::random_device throws if it
    // is unavailable, rather than the index running with a guessable key.
    [[nodiscard]] static SipKey random_key() {
        std::random_device rd;
        auto word = [&rd] { return (static_cast<std::uint64_t>(rd()) << 32U) | rd(); };
        return SipKey{word(), word()};
    }
    // Home bucket of `key` at the current capacity (diagnostics and tests).
    [[nodiscard]] std::size_t home_bucket(std::uint64_t key) const noexcept { return bucket(key); }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t allocated_bytes() const noexcept;  // the slot array

    // Value for `key`, or kEmpty.
    [[nodiscard]] std::uint32_t find(std::uint64_t key) const noexcept {
        for (std::size_t i = bucket(key);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.value == kEmpty) {
                return kEmpty;
            }
            if (s.key == key) {
                return s.value;
            }
        }
    }
    [[nodiscard]] bool contains(std::uint64_t key) const noexcept { return find(key) != kEmpty; }

    // Inserts key → value; returns false (and changes nothing) if the key is present.
    bool insert(std::uint64_t key, std::uint32_t value) {
        if (value == kEmpty) {
            throw std::invalid_argument("OrderIndex: value out of range");
        }
        if ((size_ + 1) * 2 > slots_.size()) {
            rebuild(slots_.size() * 2);
        }
        for (std::size_t i = bucket(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.value == kEmpty) {
                s.key = key;
                s.value = value;
                ++size_;
                return true;
            }
            if (s.key == key) {
                return false;
            }
        }
    }

    // Removes `key`; returns false if absent. Backward-shift deletion: entries after the hole
    // whose home bucket is not in (hole, entry] move back, keeping every probe chain intact.
    bool erase(std::uint64_t key) noexcept {
        std::size_t i = bucket(key);
        while (true) {
            const Slot& s = slots_[i];
            if (s.value == kEmpty) {
                return false;
            }
            if (s.key == key) {
                break;
            }
            i = (i + 1) & mask_;
        }
        std::size_t hole = i;
        for (std::size_t j = (hole + 1) & mask_; slots_[j].value != kEmpty; j = (j + 1) & mask_) {
            const std::size_t home = bucket(slots_[j].key);
            // Distance from home to j vs from home to hole (cyclic): move if the hole lies on
            // the entry's probe path.
            if (((j - home) & mask_) >= ((j - hole) & mask_)) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole].value = kEmpty;
        --size_;
        return true;
    }

    // Grows so that `n` keys fit without another rebuild.
    void reserve(std::size_t n) {
        if (n * 2 > slots_.size()) {
            rebuild(round_up(n * 2));
        }
    }

    void clear() noexcept {
        for (Slot& s : slots_) {
            s.value = kEmpty;
        }
        size_ = 0;
    }

    // Visits every (key, value); order depends on hashing — never use it for output order.
    template <typename F>
    void for_each(F&& f) const {
        for (const Slot& s : slots_) {
            if (s.value != kEmpty) {
                f(s.key, s.value);
            }
        }
    }

private:
    struct Slot {
        std::uint64_t key = 0;
        std::uint32_t value = kEmpty;
    };

    static std::size_t round_up(std::size_t n) {
        return std::bit_ceil(n < 16 ? std::size_t{16} : n);
    }
    [[nodiscard]] std::size_t bucket(std::uint64_t key) const noexcept {
        // The key's bytes in little-endian order, so placement does not depend on the host.
        char bytes[8];
        for (unsigned i = 0; i < 8; ++i) {
            bytes[i] = static_cast<char>(static_cast<unsigned char>(key >> (8U * i)));
        }
        return static_cast<std::size_t>(siphash24(key_, std::string_view(bytes, 8)) >> shift_);
    }

    void rebuild(std::size_t capacity) {
        std::vector<Slot> old(capacity);
        old.swap(slots_);
        mask_ = capacity - 1;
        shift_ = 64 - static_cast<unsigned>(std::countr_zero(capacity));
        size_ = 0;
        for (const Slot& s : old) {
            if (s.value != kEmpty) {
                (void)insert(s.key, s.value);
            }
        }
    }

    SipKey key_;  // declared first: rebuild() in the constructor hashes with it
    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 0;
    std::size_t size_ = 0;
};

inline std::size_t OrderIndex::allocated_bytes() const noexcept {
    return slots_.capacity() * sizeof(Slot);
}

}  // namespace enginex::itch
