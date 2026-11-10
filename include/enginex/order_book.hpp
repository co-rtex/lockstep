#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <map>
#include <memory_resource>
#include <optional>
#include <utility>
#include <vector>

#include "enginex/types.hpp"

namespace enginex {

// One resting order inside a price level's FIFO queue.
struct RestingOrder {
    OrderId id = 0;
    Quantity original = 0;
    Quantity remaining = 0;
};

// All resting orders on one side at one price, oldest (highest time priority) first.
//
// Containers use polymorphic allocators so the engine can place every book node (levels, FIFO
// entries, index entries) in one per-engine pool (DECISIONS.md D26). PriceLevel is
// allocator-aware, so a level created inside a LevelMap gets the map's memory resource for its
// order list (uses-allocator construction).
struct PriceLevel {
    using allocator_type = std::pmr::polymorphic_allocator<>;

    PriceLevel() = default;
    explicit PriceLevel(const allocator_type& alloc) : orders(alloc) {}
    PriceLevel(const PriceLevel& other, const allocator_type& alloc)
        : price(other.price), aggregate(other.aggregate), orders(other.orders, alloc) {}
    PriceLevel(PriceLevel&& other, const allocator_type& alloc)
        : price(other.price), aggregate(other.aggregate), orders(std::move(other.orders), alloc) {}

    PriceCents price = 0;
    Quantity aggregate = 0;  // sum of `remaining` over `orders`
    std::pmr::list<RestingOrder> orders;
};

// Both sides use the same ascending map type (see DECISIONS.md D2): the best ask is `begin()`,
// the best bid is `std::prev(end())`. A shared iterator type lets the order index store level
// iterators for either side.
using LevelMap = std::pmr::map<PriceCents, PriceLevel>;
using OrderNode = std::pmr::list<RestingOrder>::iterator;

// Aggregated view of one price level (spec §6 `BookLevel`).
struct BookLevel {
    PriceCents price = 0;
    Quantity aggregate_quantity = 0;
    std::uint32_t resting_order_count = 0;

    friend bool operator==(const BookLevel&, const BookLevel&) = default;
};

// Depth-limited aggregated snapshot: bids descending by price, asks ascending.
struct BookSnapshot {
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;

    friend bool operator==(const BookSnapshot&, const BookSnapshot&) = default;
};

// Location of a resting order. Iterators remain valid until their own element is erased:
// the index entry is always removed before the list node, and a level is erased only when its
// list is empty.
struct OrderLocation {
    SymbolId symbol = 0;
    Side side = Side::Buy;
    LevelMap::iterator level;
    OrderNode node;
};

// Price levels for one symbol. Holds data only; the MatchingEngine owns the matching loop and
// the order index so that every mutation keeps book and index consistent.
class OrderBook {
public:
    OrderBook() = default;
    explicit OrderBook(std::pmr::memory_resource* resource) : bids_(resource), asks_(resource) {}
    // A copy would silently allocate from the default resource instead of the engine's pool.
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) noexcept = default;
    OrderBook& operator=(OrderBook&&) = delete;

    [[nodiscard]] LevelMap& levels(Side side) noexcept { return side == Side::Buy ? bids_ : asks_; }
    [[nodiscard]] const LevelMap& levels(Side side) const noexcept {
        return side == Side::Buy ? bids_ : asks_;
    }

    // Best (highest-priority) level for `side`, or end() when the side is empty.
    [[nodiscard]] LevelMap::iterator best_level(Side side) noexcept;
    [[nodiscard]] LevelMap::const_iterator best_level(Side side) const noexcept;

    [[nodiscard]] std::optional<PriceCents> best_price(Side side) const noexcept;

    // Visits levels of `side` from best to worst; stops when `fn` returns false.
    template <typename Fn>
    void for_each_level(Side side, Fn&& fn) const {
        const LevelMap& map = levels(side);
        if (side == Side::Sell) {
            for (auto it = map.begin(); it != map.end(); ++it) {
                if (!fn(it->second)) {
                    return;
                }
            }
        } else {
            for (auto it = map.rbegin(); it != map.rend(); ++it) {
                if (!fn(it->second)) {
                    return;
                }
            }
        }
    }

    // Appends `depth` best levels per side (0 = all levels).
    void snapshot(std::size_t depth, BookSnapshot& out) const;

    [[nodiscard]] std::size_t level_count(Side side) const noexcept { return levels(side).size(); }

private:
    LevelMap bids_;
    LevelMap asks_;
};

}  // namespace enginex
