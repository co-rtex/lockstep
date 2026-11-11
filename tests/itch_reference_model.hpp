#pragma once

// Deliberately naive ITCH book model for differential testing. It shares no code with
// src/itch/book.cpp: orders are one std::map keyed by reference, priority is an explicit
// arrival counter, and every query recomputes levels by scanning all orders. Slow by design.

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

#include "enginex/itch/book.hpp"

namespace enginex::itch::test {

class ReferenceBook {
public:
    struct Order {
        std::uint16_t locate;
        Side side;
        Price4 price;
        std::uint32_t shares;
        std::uint64_t arrival;
    };

    // Returns the anomaly the specification-level rules imply (None when applied or ignored).
    Anomaly apply(const Message& m) {
        const Header& h = header_of(m);
        if (const auto* a = std::get_if<AddOrder>(&m)) {
            if (orders_.count(a->order_ref) != 0)
                return Anomaly::DuplicateOrderRef;
            if (a->shares == 0)
                return Anomaly::ZeroShares;
            orders_[a->order_ref] = Order{h.stock_locate, a->side, a->price, a->shares, ++clock_};
            return Anomaly::None;
        }
        if (const auto* e = std::get_if<OrderExecuted>(&m)) {
            return take(e->order_ref, h.stock_locate, e->executed_shares, &executed_);
        }
        if (const auto* c = std::get_if<OrderExecutedWithPrice>(&m)) {
            return take(c->order_ref, h.stock_locate, c->executed_shares, &executed_);
        }
        if (const auto* x = std::get_if<OrderCancel>(&m)) {
            return take(x->order_ref, h.stock_locate, x->canceled_shares, &canceled_);
        }
        if (const auto* d = std::get_if<OrderDelete>(&m)) {
            auto it = orders_.find(d->order_ref);
            if (it == orders_.end())
                return Anomaly::UnknownOrderRef;
            if (it->second.locate != h.stock_locate)
                return Anomaly::LocateMismatch;
            orders_.erase(it);
            return Anomaly::None;
        }
        if (const auto* u = std::get_if<OrderReplace>(&m)) {
            auto it = orders_.find(u->original_ref);
            if (it == orders_.end())
                return Anomaly::UnknownOrderRef;
            if (it->second.locate != h.stock_locate)
                return Anomaly::LocateMismatch;
            if (u->new_ref != u->original_ref && orders_.count(u->new_ref) != 0) {
                return Anomaly::DuplicateOrderRef;
            }
            if (u->shares == 0)
                return Anomaly::ZeroShares;
            const Side side = it->second.side;
            orders_.erase(it);
            orders_[u->new_ref] = Order{h.stock_locate, side, u->price, u->shares, ++clock_};
            return Anomaly::None;
        }
        return Anomaly::None;  // no displayed-book effect
    }

    // Orders of one side in priority order (best price first, then arrival).
    std::vector<OrderView> orders(std::uint16_t locate, Side side) const {
        std::vector<std::tuple<std::int64_t, std::uint64_t, OrderView>> v;
        for (const auto& [ref, o] : orders_) {
            if (o.locate == locate && o.side == side) {
                const std::int64_t key = side == Side::Buy ? -static_cast<std::int64_t>(o.price)
                                                           : static_cast<std::int64_t>(o.price);
                v.emplace_back(key, o.arrival, OrderView{ref, o.side, o.price, o.shares});
            }
        }
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            return std::tie(std::get<0>(a), std::get<1>(a)) <
                   std::tie(std::get<0>(b), std::get<1>(b));
        });
        std::vector<OrderView> out;
        for (const auto& t : v)
            out.push_back(std::get<2>(t));
        return out;
    }

    std::vector<LevelView> levels(std::uint16_t locate, Side side) const {
        std::vector<LevelView> out;
        for (const OrderView& o : orders(locate, side)) {
            if (out.empty() || out.back().price != o.price)
                out.push_back(LevelView{o.price, 0, 0});
            out.back().shares += o.shares;
            ++out.back().orders;
        }
        return out;
    }

    std::size_t live() const { return orders_.size(); }

    // Every live order grouped by (locate, side) in priority order, built in one pass (for
    // comparing large books without one full scan per query).
    std::map<std::pair<std::uint16_t, char>, std::vector<OrderView>> grouped() const {
        std::map<std::pair<std::uint16_t, char>,
                 std::vector<std::tuple<std::int64_t, std::uint64_t, OrderView>>>
            tmp;
        for (const auto& [ref, o] : orders_) {
            const std::int64_t key = o.side == Side::Buy ? -static_cast<std::int64_t>(o.price)
                                                         : static_cast<std::int64_t>(o.price);
            tmp[{o.locate, static_cast<char>(o.side)}].emplace_back(
                key, o.arrival, OrderView{ref, o.side, o.price, o.shares});
        }
        std::map<std::pair<std::uint16_t, char>, std::vector<OrderView>> out;
        for (auto& [k, v] : tmp) {
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
                return std::tie(std::get<0>(a), std::get<1>(a)) <
                       std::tie(std::get<0>(b), std::get<1>(b));
            });
            auto& dst = out[k];
            for (const auto& t : v)
                dst.push_back(std::get<2>(t));
        }
        return out;
    }
    std::uint64_t executed_shares() const { return executed_; }
    std::uint64_t canceled_shares() const { return canceled_; }

private:
    Anomaly take(OrderRef ref, std::uint16_t locate, std::uint32_t shares, std::uint64_t* total) {
        auto it = orders_.find(ref);
        if (it == orders_.end())
            return Anomaly::UnknownOrderRef;
        if (it->second.locate != locate)
            return Anomaly::LocateMismatch;
        if (shares == 0)
            return Anomaly::ZeroShares;
        if (shares > it->second.shares)
            return Anomaly::OverExecution;
        it->second.shares -= shares;
        *total += shares;
        if (it->second.shares == 0)
            orders_.erase(it);
        return Anomaly::None;
    }

    std::map<OrderRef, Order> orders_;
    std::uint64_t clock_ = 0;
    std::uint64_t executed_ = 0;
    std::uint64_t canceled_ = 0;
};

}  // namespace enginex::itch::test
