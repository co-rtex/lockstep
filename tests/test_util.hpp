#pragma once

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "enginex/canonical.hpp"
#include "enginex/state.hpp"

namespace enginex::test {

inline NewLimitOrder order(std::string request_id, std::string symbol, Side side, PriceCents price,
                           Quantity quantity) {
    return NewLimitOrder{std::move(request_id), std::move(symbol), side, price, quantity};
}

inline NewLimitOrder buy(std::string request_id, PriceCents price, Quantity quantity,
                         std::string symbol = "AAPL") {
    return order(std::move(request_id), std::move(symbol), Side::Buy, price, quantity);
}

inline NewLimitOrder sell(std::string request_id, PriceCents price, Quantity quantity,
                          std::string symbol = "AAPL") {
    return order(std::move(request_id), std::move(symbol), Side::Sell, price, quantity);
}

inline CancelOrder cancel(std::string request_id, std::string order_id) {
    return CancelOrder{std::move(request_id), std::move(order_id)};
}

inline CancelOrder cancel(std::string request_id, OrderId order_id) {
    return CancelOrder{std::move(request_id), format_order_id(order_id)};
}

// Resting orders of one side in priority order: (order id, remaining, price).
struct Resting {
    OrderId id;
    Quantity remaining;
    PriceCents price;
    friend bool operator==(const Resting&, const Resting&) = default;
};

inline void PrintTo(const Resting& r, std::ostream* os) {
    *os << "{ord-" << r.id << " " << r.remaining << " @ " << r.price << "}";
}

inline std::vector<Resting> resting(const ExchangeStateMachine& state, const std::string& symbol,
                                    Side side) {
    std::vector<Resting> out;
    const auto id = state.find_symbol(symbol);
    if (!id) {
        ADD_FAILURE() << "unknown symbol " << symbol;
        return out;
    }
    state.engine().book(*id).for_each_level(side, [&out](const PriceLevel& level) {
        for (const auto& o : level.orders) {
            out.push_back(Resting{o.id, o.remaining, level.price});
        }
        return true;
    });
    return out;
}

// Execution tuple used for compact expectations.
struct Exec {
    OrderId incoming;
    OrderId resting;
    PriceCents price;
    Quantity quantity;
    friend bool operator==(const Exec&, const Exec&) = default;
};

inline void PrintTo(const Exec& e, std::ostream* os) {
    *os << "{in ord-" << e.incoming << " rest ord-" << e.resting << " " << e.quantity << " @ "
        << e.price << "}";
}

inline std::vector<Exec> execs(const CommandResult& r) {
    std::vector<Exec> out;
    for (const auto& e : r.executions) {
        out.push_back(Exec{e.incoming_order_id, e.resting_order_id, e.price, e.quantity});
    }
    return out;
}

// Processes a command and asserts it was accepted.
inline CommandResult accept(ExchangeStateMachine& state, const Command& cmd) {
    CommandResult r = state.process(cmd);
    EXPECT_TRUE(r.accepted) << "request " << r.request_id << " rejected: " << to_string(r.error)
                            << " " << r.message;
    EXPECT_TRUE(state.engine().check_invariants().empty()) << state.engine().check_invariants();
    return r;
}

// Processes a command and asserts it was rejected with `code` and changed nothing.
inline CommandResult reject(ExchangeStateMachine& state, const Command& cmd, ErrorCode code) {
    const std::string before = canonical_state(state);
    CommandResult r = state.process(cmd);
    EXPECT_FALSE(r.accepted) << "request " << r.request_id << " unexpectedly accepted";
    EXPECT_EQ(r.error, code) << "got " << to_string(r.error) << " " << r.message;
    EXPECT_EQ(r.command_sequence, 0U);
    EXPECT_EQ(canonical_state(state), before) << "rejected command mutated state";
    return r;
}

}  // namespace enginex::test
