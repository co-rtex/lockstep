#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "enginex/order_book.hpp"
#include "enginex/types.hpp"

namespace enginex {

// Thrown when an internal invariant is violated. Validation makes these unreachable for valid
// inputs; callers treat them as fail-stop internal errors, never as user rejections.
class InvariantViolation : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

// A limit order that already passed validation and was assigned its identity.
struct AcceptedOrder {
    OrderId id = 0;
    SymbolId symbol = 0;
    Side side = Side::Buy;
    PriceCents price = 0;
    Quantity quantity = 0;
};

// One match between the incoming order and one resting order, in execution order.
struct Fill {
    OrderId resting_order_id = 0;
    PriceCents price = 0;  // always the resting order's limit price
    Quantity quantity = 0;
    Quantity incoming_remaining = 0;  // after this fill
    Quantity resting_remaining = 0;   // after this fill
};

struct MatchOutcome {
    Quantity remaining = 0;  // incoming quantity left after matching
    bool rested = false;     // remainder was placed on the book
};

struct CancelOutcome {
    SymbolId symbol = 0;
    Side side = Side::Buy;
    PriceCents price = 0;
    Quantity original = 0;
    Quantity cancelled_quantity = 0;  // remaining quantity removed from the book
};

// Price-time-priority matcher over a fixed set of symbol books.
//
// Not thread-safe by design: exactly one owner (the sequencer) calls mutating methods. The
// engine does not validate business rules; ExchangeStateMachine does that before calling it.
class MatchingEngine {
public:
    explicit MatchingEngine(std::size_t symbol_count);
    ~MatchingEngine();
    MatchingEngine(const MatchingEngine&) = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    // Moving keeps every container's memory resource valid: the pool lives on the heap and
    // moves with its owner. Move *assignment* would destroy the target's pool while its
    // containers still hold pool memory, so it is not provided.
    MatchingEngine(MatchingEngine&&) noexcept;
    MatchingEngine& operator=(MatchingEngine&&) = delete;

    // Matches `order` against the opposite side, appending one Fill per incoming/resting pair to
    // `fills` (which is not cleared). Rests any positive remainder at the tail of its level.
    MatchOutcome submit(const AcceptedOrder& order, std::vector<Fill>& fills);

    // Removes a resting order. Returns nullopt when `id` is not currently resting.
    std::optional<CancelOutcome> cancel(OrderId id);

    [[nodiscard]] bool is_resting(OrderId id) const { return index_.contains(id); }
    [[nodiscard]] std::size_t open_order_count() const noexcept { return index_.size(); }
    [[nodiscard]] std::size_t symbol_count() const noexcept { return books_.size(); }
    [[nodiscard]] const OrderBook& book(SymbolId symbol) const { return books_.at(symbol); }

    void snapshot(SymbolId symbol, std::size_t depth, BookSnapshot& out) const;
    [[nodiscard]] BookSnapshot snapshot(SymbolId symbol, std::size_t depth) const;

    // Pre-sizes the order index to avoid rehashing during benchmarks.
    void reserve(std::size_t expected_open_orders) { index_.reserve(expected_open_orders); }

    // Full O(n) consistency check of books and index. Returns an empty string when consistent,
    // otherwise a description of the first violation. Intended for tests.
    [[nodiscard]] std::string check_invariants() const;

private:
    template <Side IncomingSide>
    Quantity match(OrderBook& book, PriceCents limit, Quantity remaining, std::vector<Fill>& fills);

    void rest(const AcceptedOrder& order, OrderBook& book, Quantity remaining);

    // Declared first: destroyed after the containers that allocate from it.
    std::unique_ptr<std::pmr::memory_resource> pool_;
    std::vector<OrderBook> books_;
    std::pmr::unordered_map<OrderId, OrderLocation> index_;
};

}  // namespace enginex
