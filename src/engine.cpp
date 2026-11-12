#include "enginex/engine.hpp"

#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "enginex/checked.hpp"

namespace enginex {

namespace {

// Per-engine pool for book nodes (D26). -DENGINEX_CORE_STD_ALLOC=ON keeps the same container
// types but forwards every allocation to operator new/delete, for same-commit comparisons.
std::unique_ptr<std::pmr::memory_resource> make_pool() {
#ifdef ENGINEX_CORE_STD_ALLOC
    return nullptr;
#else
    return std::make_unique<std::pmr::unsynchronized_pool_resource>();
#endif
}

}  // namespace

MatchingEngine::MatchingEngine(std::size_t symbol_count)
    : pool_(make_pool()), index_(pool_ ? pool_.get() : std::pmr::new_delete_resource()) {
    if (symbol_count == 0) {
        throw std::invalid_argument("MatchingEngine requires at least one symbol");
    }
    std::pmr::memory_resource* resource = pool_ ? pool_.get() : std::pmr::new_delete_resource();
    books_.reserve(symbol_count);
    for (std::size_t i = 0; i < symbol_count; ++i) {
        books_.emplace_back(resource);
    }
}

MatchingEngine::~MatchingEngine() = default;
MatchingEngine::MatchingEngine(MatchingEngine&&) noexcept = default;

template <Side IncomingSide>
Quantity MatchingEngine::match(OrderBook& book, PriceCents limit, Quantity remaining,
                               std::vector<Fill>& fills) {
    constexpr Side kRestingSide = opposite(IncomingSide);
    LevelMap& resting = book.levels(kRestingSide);

    while (remaining > 0 && !resting.empty()) {
        // Best opposite level: lowest ask for an incoming BUY, highest bid for an incoming SELL.
        auto level_it = kRestingSide == Side::Sell ? resting.begin() : std::prev(resting.end());
        const PriceCents level_price = level_it->first;
        const bool marketable =
            IncomingSide == Side::Buy ? level_price <= limit : level_price >= limit;
        if (!marketable) {
            break;
        }

        PriceLevel& level = level_it->second;
        while (remaining > 0 && !level.orders.empty()) {
            RestingOrder& head = level.orders.front();
            const Quantity traded = std::min(remaining, head.remaining);
            remaining -= traded;
            head.remaining -= traded;
            level.aggregate -= traded;
            fills.push_back(Fill{head.id, level_price, traded, remaining, head.remaining});
            if (head.remaining == 0) {
                // Erase the index entry before the list node it points to.
                index_.erase(head.id);
                level.orders.pop_front();
            }
        }
        if (level.orders.empty()) {
            if (level.aggregate != 0) {
                throw InvariantViolation("empty price level has nonzero aggregate");
            }
            resting.erase(level_it);
        }
    }
    return remaining;
}

void MatchingEngine::rest(const AcceptedOrder& order, OrderBook& book, Quantity remaining) {
    LevelMap& own = book.levels(order.side);
    auto [level_it, inserted] = own.try_emplace(order.price);
    PriceLevel& level = level_it->second;
    if (inserted) {
        level.price = order.price;
    }
    const auto aggregate = checked_add(level.aggregate, remaining);
    if (!aggregate) {
        if (inserted) {
            own.erase(level_it);
        }
        throw InvariantViolation("price level aggregate overflow");
    }
    level.orders.push_back(RestingOrder{order.id, order.quantity, remaining});
    level.aggregate = *aggregate;
    auto node = std::prev(level.orders.end());
    const auto [it, fresh] =
        index_.try_emplace(order.id, OrderLocation{order.symbol, order.side, level_it, node});
    (void)it;
    if (!fresh) {
        // Roll back so the book never contains an unindexed order.
        level.aggregate -= remaining;
        level.orders.pop_back();
        if (level.orders.empty()) {
            own.erase(level_it);
        }
        throw InvariantViolation("duplicate order id in index");
    }
}

MatchOutcome MatchingEngine::submit(const AcceptedOrder& order, std::vector<Fill>& fills) {
    OrderBook& book = books_.at(order.symbol);
    const Quantity remaining = order.side == Side::Buy
                                   ? match<Side::Buy>(book, order.price, order.quantity, fills)
                                   : match<Side::Sell>(book, order.price, order.quantity, fills);
    MatchOutcome outcome{remaining, false};
    if (remaining > 0) {
        rest(order, book, remaining);
        outcome.rested = true;
    }
    return outcome;
}

std::optional<CancelOutcome> MatchingEngine::cancel(OrderId id) {
    const auto found = index_.find(id);
    if (found == index_.end()) {
        return std::nullopt;
    }
    const OrderLocation loc = found->second;
    index_.erase(found);

    PriceLevel& level = loc.level->second;
    const CancelOutcome outcome{loc.symbol, loc.side, level.price, loc.node->original,
                                loc.node->remaining};
    level.aggregate -= loc.node->remaining;
    level.orders.erase(loc.node);
    if (level.orders.empty()) {
        books_[loc.symbol].levels(loc.side).erase(loc.level);
    }
    return outcome;
}

void MatchingEngine::snapshot(SymbolId symbol, std::size_t depth, BookSnapshot& out) const {
    books_.at(symbol).snapshot(depth, out);
}

BookSnapshot MatchingEngine::snapshot(SymbolId symbol, std::size_t depth) const {
    BookSnapshot out;
    snapshot(symbol, depth, out);
    return out;
}

std::string MatchingEngine::check_invariants() const {
    std::ostringstream err;
    std::size_t resting_total = 0;
    std::unordered_set<OrderId> seen;

    for (std::size_t s = 0; s < books_.size(); ++s) {
        const OrderBook& book = books_[s];
        for (const Side side : {Side::Buy, Side::Sell}) {
            for (const auto& [price, level] : book.levels(side)) {
                if (price != level.price) {
                    err << "symbol " << s << " level key " << price << " != level price "
                        << level.price;
                    return err.str();
                }
                if (price < kMinPrice || price > kMaxPrice) {
                    err << "symbol " << s << " level price out of range: " << price;
                    return err.str();
                }
                if (level.orders.empty()) {
                    err << "symbol " << s << " empty level at " << price;
                    return err.str();
                }
                Quantity sum = 0;
                for (auto node = level.orders.begin(); node != level.orders.end(); ++node) {
                    if (node->remaining <= 0 || node->remaining > node->original) {
                        err << "order " << node->id << " has invalid remaining " << node->remaining
                            << " (original " << node->original << ")";
                        return err.str();
                    }
                    if (node != level.orders.begin() && std::prev(node)->id >= node->id) {
                        err << "FIFO violated at price " << price << ": " << std::prev(node)->id
                            << " before " << node->id;
                        return err.str();
                    }
                    if (!seen.insert(node->id).second) {
                        err << "order " << node->id << " rests more than once";
                        return err.str();
                    }
                    const auto idx = index_.find(node->id);
                    if (idx == index_.end()) {
                        err << "resting order " << node->id << " missing from index";
                        return err.str();
                    }
                    const OrderLocation& loc = idx->second;
                    if (loc.symbol != s || loc.side != side || loc.level->first != price ||
                        loc.node != node) {
                        err << "index entry for order " << node->id << " is stale";
                        return err.str();
                    }
                    sum += node->remaining;
                    ++resting_total;
                }
                if (sum != level.aggregate) {
                    err << "aggregate mismatch at price " << price << ": " << level.aggregate
                        << " != " << sum;
                    return err.str();
                }
            }
        }
        const auto bid = book.best_price(Side::Buy);
        const auto ask = book.best_price(Side::Sell);
        if (bid && ask && *bid >= *ask) {
            err << "symbol " << s << " crossed book: bid " << *bid << " >= ask " << *ask;
            return err.str();
        }
    }
    if (resting_total != index_.size()) {
        err << "index has " << index_.size() << " entries but " << resting_total << " orders rest";
        return err.str();
    }
    return {};
}

}  // namespace enginex
