#pragma once

// A deliberately naive, independent reference implementation of the matching rules, used only to
// cross-check the real engine on small random books. It stores every resting order in one flat
// vector in acceptance order and finds the best counterparty by linear scan, so it shares no
// data structures or loop structure with MatchingEngine.

#include <cstdint>
#include <string>
#include <vector>

#include "enginex/types.hpp"

namespace enginex::test {

struct RefOrder {
    OrderId id;
    std::string symbol;
    Side side;
    PriceCents price;
    Quantity remaining;
};

struct RefFill {
    OrderId incoming;
    OrderId resting;
    PriceCents price;
    Quantity quantity;
    friend bool operator==(const RefFill&, const RefFill&) = default;
};

class ReferenceBook {
public:
    // Matches and rests an already-validated order; returns fills in execution order.
    std::vector<RefFill> submit(OrderId id, const std::string& symbol, Side side, PriceCents price,
                                Quantity quantity) {
        std::vector<RefFill> fills;
        Quantity remaining = quantity;
        while (remaining > 0) {
            int best = -1;
            for (int i = 0; i < static_cast<int>(orders_.size()); ++i) {
                const RefOrder& o = orders_[static_cast<std::size_t>(i)];
                if (o.symbol != symbol || o.side == side) {
                    continue;
                }
                const bool compatible = side == Side::Buy ? o.price <= price : o.price >= price;
                if (!compatible) {
                    continue;
                }
                if (best < 0) {
                    best = i;
                    continue;
                }
                const RefOrder& b = orders_[static_cast<std::size_t>(best)];
                const bool better = side == Side::Buy ? o.price < b.price : o.price > b.price;
                // Equal price: the earlier element (earlier acceptance) wins, so keep `best`.
                if (better) {
                    best = i;
                }
            }
            if (best < 0) {
                break;
            }
            RefOrder& r = orders_[static_cast<std::size_t>(best)];
            const Quantity q = remaining < r.remaining ? remaining : r.remaining;
            fills.push_back(RefFill{id, r.id, r.price, q});
            remaining -= q;
            r.remaining -= q;
            if (r.remaining == 0) {
                orders_.erase(orders_.begin() + best);
            }
        }
        if (remaining > 0) {
            orders_.push_back(RefOrder{id, symbol, side, price, remaining});
        }
        return fills;
    }

    bool cancel(OrderId id) {
        for (auto it = orders_.begin(); it != orders_.end(); ++it) {
            if (it->id == id) {
                orders_.erase(it);
                return true;
            }
        }
        return false;
    }

    // Resting orders of one side in priority order (price, then acceptance).
    [[nodiscard]] std::vector<RefOrder> side_in_priority(const std::string& symbol,
                                                         Side side) const {
        std::vector<RefOrder> out;
        for (const auto& o : orders_) {
            if (o.symbol == symbol && o.side == side) {
                out.push_back(o);
            }
        }
        // Stable insertion sort by price priority keeps acceptance order for equal prices.
        for (std::size_t i = 1; i < out.size(); ++i) {
            for (std::size_t j = i; j > 0; --j) {
                const bool before = side == Side::Buy ? out[j].price > out[j - 1].price
                                                      : out[j].price < out[j - 1].price;
                if (!before) {
                    break;
                }
                std::swap(out[j], out[j - 1]);
            }
        }
        return out;
    }

    [[nodiscard]] bool is_resting(OrderId id) const {
        for (const auto& o : orders_) {
            if (o.id == id) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::size_t size() const { return orders_.size(); }

private:
    std::vector<RefOrder> orders_;
};

}  // namespace enginex::test
