#pragma once

// Order reference → pool slot, exploiting how Nasdaq assigns references: day-unique, dense
// and mostly increasing (measured on a real sample: 73 % of adds carry a larger reference
// than the previous add; ~20 M distinct references in 21 M messages). See D27.
//
// References below kDirectLimit index a two-level table: a directory of pages, each page a
// flat array of 4096 slots (16 KiB). Lookup is two dependent loads and no hashing or probing.
// Recent orders, the busy ones, share a few hot pages. Pages are allocated on first use and
// freed when their last live order is removed.
//
// Memory is bounded: at most `max_pages` pages exist (default 16384 = 256 MiB). A reference
// whose page cannot be created (budget spent) or that is ≥ kDirectLimit goes to an
// open-addressing hash table instead, so adversarial or unusual reference patterns degrade to
// the hash table's behavior, never to unbounded memory.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "enginex/itch/order_index.hpp"

namespace enginex::itch {

class PagedIndex {
public:
    static constexpr std::uint32_t kEmpty = OrderIndex::kEmpty;
    static constexpr unsigned kPageBits = 12;
    static constexpr std::size_t kPageSlots = std::size_t{1} << kPageBits;
    static constexpr std::uint64_t kDirectLimit = std::uint64_t{1} << 32;

    explicit PagedIndex(std::size_t max_pages = 16384) : max_pages_(max_pages) {}

    [[nodiscard]] std::uint32_t find(std::uint64_t key) const noexcept {
        if (key < kDirectLimit) {
            const std::size_t p = page_of(key);
            if (p < dir_.size() && dir_[p]) {
                const std::uint32_t v = dir_[p]->slots[offset_of(key)];
                if (v != kEmpty || far_.size() == 0) {
                    return v;
                }
            } else if (far_.size() == 0) {
                return kEmpty;
            }
        }
        return far_.find(key);
    }
    [[nodiscard]] bool contains(std::uint64_t key) const noexcept { return find(key) != kEmpty; }

    bool insert(std::uint64_t key, std::uint32_t value) {
        if (value == kEmpty) {
            throw std::invalid_argument("PagedIndex: value out of range");
        }
        if (contains(key)) {
            return false;
        }
        if (Page* page = key < kDirectLimit ? page_for_insert(key) : nullptr) {
            page->slots[offset_of(key)] = value;
            ++page->live;
        } else {
            (void)far_.insert(key, value);
        }
        ++size_;
        return true;
    }

    bool erase(std::uint64_t key) noexcept {
        if (key < kDirectLimit) {
            const std::size_t p = page_of(key);
            if (p < dir_.size() && dir_[p]) {
                std::uint32_t& slot = dir_[p]->slots[offset_of(key)];
                if (slot != kEmpty) {
                    slot = kEmpty;
                    --size_;
                    if (--dir_[p]->live == 0) {
                        dir_[p].reset();  // last live order on this page
                        --pages_;
                    }
                    return true;
                }
            }
        }
        if (far_.erase(key)) {
            --size_;
            return true;
        }
        return false;
    }

    void reserve(std::size_t) {}  // pages are created on demand
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t pages() const noexcept { return pages_; }
    [[nodiscard]] std::size_t fallback_size() const noexcept { return far_.size(); }

private:
    struct Page {
        Page() { slots.fill(kEmpty); }
        std::array<std::uint32_t, kPageSlots> slots;
        std::uint32_t live = 0;
    };

    static std::size_t page_of(std::uint64_t key) noexcept {
        return static_cast<std::size_t>(key >> kPageBits);
    }
    static std::size_t offset_of(std::uint64_t key) noexcept {
        return static_cast<std::size_t>(key & (kPageSlots - 1));
    }

    Page* page_for_insert(std::uint64_t key) {
        const std::size_t p = page_of(key);
        if (p < dir_.size() && dir_[p]) {
            return dir_[p].get();
        }
        if (pages_ >= max_pages_) {
            return nullptr;  // budget spent: use the hash table
        }
        if (p >= dir_.size()) {
            dir_.resize(std::max(p + 1, dir_.size() * 2));
        }
        dir_[p] = std::make_unique<Page>();
        ++pages_;
        return dir_[p].get();
    }

    std::size_t max_pages_;
    std::vector<std::unique_ptr<Page>> dir_;  // indexed by key >> kPageBits
    OrderIndex far_;                          // references ≥ kDirectLimit or over budget
    std::size_t size_ = 0;
    std::size_t pages_ = 0;
};

}  // namespace enginex::itch
