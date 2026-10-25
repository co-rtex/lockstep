#include "enginex/order_book.hpp"

#include <iterator>

namespace enginex {

LevelMap::iterator OrderBook::best_level(Side side) noexcept {
    LevelMap& map = levels(side);
    if (map.empty()) {
        return map.end();
    }
    return side == Side::Sell ? map.begin() : std::prev(map.end());
}

LevelMap::const_iterator OrderBook::best_level(Side side) const noexcept {
    const LevelMap& map = levels(side);
    if (map.empty()) {
        return map.end();
    }
    return side == Side::Sell ? map.begin() : std::prev(map.end());
}

std::optional<PriceCents> OrderBook::best_price(Side side) const noexcept {
    const auto it = best_level(side);
    if (it == levels(side).end()) {
        return std::nullopt;
    }
    return it->first;
}

void OrderBook::snapshot(std::size_t depth, BookSnapshot& out) const {
    out.bids.clear();
    out.asks.clear();
    auto collect = [depth](std::vector<BookLevel>& dest) {
        return [&dest, depth](const PriceLevel& level) {
            dest.push_back(BookLevel{level.price, level.aggregate,
                                     static_cast<std::uint32_t>(level.orders.size())});
            return depth == 0 || dest.size() < depth;
        };
    };
    for_each_level(Side::Buy, collect(out.bids));
    for_each_level(Side::Sell, collect(out.asks));
}

}  // namespace enginex
