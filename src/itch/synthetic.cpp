#include "enginex/itch/synthetic.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "enginex/itch/decoder.hpp"
#include "enginex/rng.hpp"

namespace enginex::itch {

namespace {

constexpr Price4 kTick = 100;  // $0.01 in Price(4)

struct Live {
    std::uint16_t locate;
    Side side;
    Price4 price;
    std::uint32_t shares;
    std::uint64_t arrival;  // priority within a price level
    std::size_t slot;       // position in Generator::refs_
};

// Priority key: begin() of a side's set is its best order (best price, then earliest).
using Key = std::tuple<std::int64_t, std::uint64_t, OrderRef>;

class Generator {
public:
    explicit Generator(const SyntheticOptions& o) : o_(o), rng_(o.seed) {
        for (std::uint16_t i = 0; i < o_.symbols; ++i) {
            mid_.push_back(static_cast<Price4>(rng_.between(1000, 50000)) * kTick);
        }
        bids_.resize(o_.symbols);
        asks_.resize(o_.symbols);
    }

    std::vector<Message> run() {
        out_.reserve(o_.messages + 2 * o_.symbols + 8);
        system('O');
        for (std::uint16_t i = 0; i < o_.symbols; ++i) {
            directory(static_cast<std::uint16_t>(i + 1));
        }
        system('S');
        system('Q');
        while (lifecycle_ < o_.messages) {
            if (o_.anomalies_per_million != 0 && rng_.below(1'000'000) < o_.anomalies_per_million) {
                anomaly();
            } else {
                event();
            }
        }
        system('M');
        system('E');
        system('C');
        return std::move(out_);
    }

private:
    template <typename M>
    void emit(M&& m) {
        out_.emplace_back(std::forward<M>(m));
        ++lifecycle_;
    }

    Header header(char type, std::uint16_t locate) {
        ts_ += rng_.below(5000);
        return Header{type, locate, static_cast<std::uint16_t>(rng_.below(4)), ts_};
    }
    void system(char code) { out_.emplace_back(SystemEvent{header('S', 0), code}); }

    void directory(std::uint16_t locate) {
        StockDirectory d;
        d.h = header('R', locate);
        d.stock = symbol(locate);
        d.market_category = 'Q';
        d.financial_status = 'N';
        d.round_lot_size = 100;
        d.round_lots_only = 'N';
        d.issue_classification = 'C';
        d.issue_subtype = {'Z', ' '};
        d.authenticity = 'P';
        d.short_sale_threshold = 'N';
        d.ipo_flag = 'N';
        d.luld_tier = '1';
        d.etp_flag = 'N';
        d.etp_leverage_factor = 0;
        d.inverse_indicator = 'N';
        out_.emplace_back(d);
    }

    static Symbol symbol(std::uint16_t locate) {
        return make_symbol("SYM" + std::to_string(locate));
    }
    std::uint16_t random_locate() { return static_cast<std::uint16_t>(rng_.below(o_.symbols) + 1); }

    std::set<Key>& side_set(std::uint16_t locate, Side side) {
        return side == Side::Buy ? bids_[locate - 1U] : asks_[locate - 1U];
    }
    static Key key(Side side, Price4 price, std::uint64_t arrival, OrderRef ref) {
        const auto p = static_cast<std::int64_t>(price);
        return {side == Side::Buy ? -p : p, arrival, ref};
    }
    std::optional<Price4> best(std::uint16_t locate, Side side) {
        const auto& s = side_set(locate, side);
        if (s.empty()) {
            return std::nullopt;
        }
        return live_.at(std::get<2>(*s.begin())).price;
    }

    // Passive price for a new or replaced order: near the mid, never crossing the other side.
    Price4 passive_price(std::uint16_t locate, Side side) {
        Price4& mid = mid_[locate - 1U];
        if (rng_.chance(5)) {
            mid = rng_.chance(50) ? mid + kTick : std::max<Price4>(10 * kTick, mid - kTick);
        }
        const auto ticks = static_cast<Price4>(rng_.below(20));
        if (side == Side::Buy) {
            Price4 p = mid > (ticks + 1) * kTick ? mid - (ticks + 1) * kTick : kTick;
            if (const auto ask = best(locate, Side::Sell); ask && p >= *ask) {
                p = *ask > kTick ? *ask - kTick : kTick;
            }
            return p;
        }
        Price4 p = mid + ticks * kTick;
        if (const auto bid = best(locate, Side::Buy); bid && p <= *bid) {
            p = *bid + kTick;
        }
        return p;
    }

    void rest(OrderRef ref, std::uint16_t locate, Side side, Price4 price, std::uint32_t shares) {
        const std::uint64_t arrival = ++arrival_;
        live_[ref] = Live{locate, side, price, shares, arrival, refs_.size()};
        refs_.push_back(ref);
        side_set(locate, side).insert(key(side, price, arrival, ref));
    }
    void forget(OrderRef ref) {
        const Live o = live_.at(ref);
        side_set(o.locate, o.side).erase(key(o.side, o.price, o.arrival, ref));
        const OrderRef moved = refs_.back();
        refs_[o.slot] = moved;
        live_.at(moved).slot = o.slot;
        refs_.pop_back();
        live_.erase(ref);
    }
    OrderRef random_ref() { return refs_[static_cast<std::size_t>(rng_.below(refs_.size()))]; }

    void add() {
        const std::uint16_t locate = random_locate();
        const Side side = rng_.chance(50) ? Side::Buy : Side::Sell;
        const auto shares = static_cast<std::uint32_t>(rng_.chance(70) ? 100 * rng_.between(1, 10)
                                                                       : rng_.between(1, 5000));
        if (rng_.chance(4) && best(locate, side == Side::Buy ? Side::Sell : Side::Buy)) {
            aggress(locate, side, shares);  // marketable: executes, never displayed
            return;
        }
        AddOrder a;
        const bool mpid = rng_.chance(15);
        a.h = header(mpid ? 'F' : 'A', locate);
        a.order_ref = next_ref_++;
        a.side = side;
        a.shares = shares;
        a.stock = symbol(locate);
        a.price = passive_price(locate, side);
        if (mpid) {
            a.attribution = Mpid{'M', 'P', static_cast<char>('A' + rng_.below(26)), 'X'};
        }
        rest(a.order_ref, locate, side, a.price, a.shares);
        emit(a);
    }

    // An incoming marketable order (not displayed, as on the real feed): executes against the
    // contra side's best orders in priority order until filled or that side is empty.
    void aggress(std::uint16_t locate, Side side, std::uint32_t shares) {
        const Side contra = side == Side::Buy ? Side::Sell : Side::Buy;
        auto& book = side_set(locate, contra);
        while (shares > 0 && !book.empty()) {
            const OrderRef ref = std::get<2>(*book.begin());
            Live& o = live_.at(ref);
            const std::uint32_t n = std::min(shares, o.shares);
            execute(ref, n, rng_.chance(85) ? 'E' : 'C');
            shares -= n;
        }
    }

    void execute(OrderRef ref, std::uint32_t n, char type) {
        Live& o = live_.at(ref);
        const Header h = header(type, o.locate);
        if (type == 'E') {
            emit(OrderExecuted{h, ref, n, next_match_++});
        } else {
            emit(OrderExecutedWithPrice{h, ref, n, next_match_++, rng_.chance(80),
                                        o.price + (o.side == Side::Buy ? 0 : kTick)});
        }
        o.shares -= n;
        if (o.shares == 0) {
            forget(ref);
        }
    }

    void event() {
        if (refs_.empty()) {
            add();
            return;
        }
        const std::uint64_t r = rng_.below(100);
        if (r < 40) {
            add();
        } else if (r < 62) {
            const OrderRef ref = random_ref();
            emit(OrderDelete{header('D', live_.at(ref).locate), ref});
            forget(ref);
        } else if (r < 72) {
            const OrderRef ref = random_ref();
            const Live o = live_.at(ref);
            forget(ref);
            OrderReplace u{header('U', o.locate), ref, next_ref_++, 0, 0};
            u.shares = static_cast<std::uint32_t>(rng_.between(1, 5000));
            u.price = rng_.chance(30) ? o.price : passive_price(o.locate, o.side);
            rest(u.new_ref, o.locate, o.side, u.price, u.shares);
            emit(u);
        } else if (r < 82) {
            const OrderRef ref = random_ref();
            const Live& o = live_.at(ref);
            const auto n =
                o.shares == 1 ? 1U : static_cast<std::uint32_t>(rng_.between(1, o.shares - 1));
            emit(OrderCancel{header('X', o.locate), ref, n});
            live_.at(ref).shares -= n;
            if (live_.at(ref).shares == 0) {
                forget(ref);
            }
        } else if (r < 95) {
            // Execution at the top of a random book (partial 60%, else the whole order).
            const std::uint16_t locate = random_locate();
            const Side side = rng_.chance(50) ? Side::Buy : Side::Sell;
            const auto& book = side_set(locate, side);
            if (book.empty()) {
                add();
                return;
            }
            const OrderRef ref = std::get<2>(*book.begin());
            const std::uint32_t have = live_.at(ref).shares;
            const std::uint32_t n = have > 1 && rng_.chance(60)
                                        ? static_cast<std::uint32_t>(rng_.between(1, have - 1))
                                        : have;
            execute(ref, n, r < 92 ? 'E' : 'C');
        } else if (r < 98) {
            const std::uint16_t locate = random_locate();
            emit(Trade{header('P', locate), 0, 'B',
                       static_cast<std::uint32_t>(rng_.between(1, 1000)), symbol(locate),
                       mid_[locate - 1U], next_match_++});
        } else {
            other();
        }
    }

    void other() {
        const std::uint16_t locate = random_locate();
        const Symbol s = symbol(locate);
        switch (rng_.below(4)) {
            case 0:
                emit(CrossTrade{header('Q', locate), rng_.below(100000), s, mid_[locate - 1U],
                                next_match_++, 'O'});
                break;
            case 1:
                emit(BrokenTrade{header('B', locate),
                                 next_match_ > 1 ? rng_.below(next_match_ - 1) + 1 : 1});
                break;
            case 2:
                emit(Noii{header('I', locate), rng_.below(10000), rng_.below(1000), 'B', s, 0, 0,
                          mid_[locate - 1U], 'C', 'L'});
                break;
            default:
                emit(StockTradingAction{header('H', locate), s, 'T', ' ', {' ', ' ', ' ', ' '}});
                break;
        }
    }

    // A message that contradicts the generator's own model of the book.
    void anomaly() {
        const std::uint16_t locate = random_locate();
        switch (rng_.below(4)) {
            case 0:  // unknown reference (never issued)
                emit(OrderExecuted{header('E', locate), next_ref_ + 1'000'000'000ULL, 1,
                                   next_match_++});
                return;
            case 1:
                if (!refs_.empty()) {  // over-cancel a live order
                    const OrderRef ref = random_ref();
                    const Live& o = live_.at(ref);
                    emit(OrderCancel{header('X', o.locate), ref, o.shares + 1});
                    return;
                }
                break;
            case 2:
                if (!refs_.empty()) {  // duplicate add of a live reference
                    const OrderRef ref = random_ref();
                    const Live& o = live_.at(ref);
                    AddOrder a;
                    a.h = header('A', o.locate);
                    a.order_ref = ref;
                    a.side = o.side;
                    a.shares = 100;
                    a.stock = symbol(o.locate);
                    a.price = o.price;
                    emit(a);
                    return;
                }
                break;
            default:
                if (!refs_.empty() && o_.symbols > 1) {  // modify with the wrong locate
                    const OrderRef ref = random_ref();
                    const auto wrong =
                        static_cast<std::uint16_t>(live_.at(ref).locate % o_.symbols + 1);
                    switch (rng_.below(4)) {
                        case 0:
                            emit(OrderDelete{header('D', wrong), ref});
                            break;
                        case 1:
                            emit(OrderExecuted{header('E', wrong), ref, 1, next_match_++});
                            break;
                        case 2:
                            emit(OrderCancel{header('X', wrong), ref, 1});
                            break;
                        default:
                            emit(OrderReplace{header('U', wrong), ref, next_ref_++, 100,
                                              live_.at(ref).price});
                            break;
                    }
                    return;
                }
                break;
        }
        event();  // no anomaly possible yet: emit a normal event instead
    }

    SyntheticOptions o_;
    Rng rng_;
    std::vector<Message> out_;
    std::size_t lifecycle_ = 0;
    std::unordered_map<OrderRef, Live> live_;  // lookups only; never iterated
    std::vector<OrderRef> refs_;               // live references, for uniform random picks
    std::vector<std::set<Key>> bids_;
    std::vector<std::set<Key>> asks_;
    std::vector<Price4> mid_;
    std::uint64_t ts_ = 34'200'000'000'000ULL;  // 09:30:00 in ns since midnight
    std::uint64_t arrival_ = 0;
    OrderRef next_ref_ = 1;
    std::uint64_t next_match_ = 1;
};

}  // namespace

Symbol make_symbol(std::string_view text) {
    Symbol s;
    s.fill(' ');
    std::copy_n(text.begin(), std::min(text.size(), s.size()), s.begin());
    return s;
}

std::vector<Message> generate(const SyntheticOptions& options) {
    if (options.symbols == 0) {
        throw std::invalid_argument("synthetic ITCH: symbols must be > 0");
    }
    return Generator(options).run();
}

std::vector<std::uint8_t> generate_file(const SyntheticOptions& options) {
    std::vector<std::uint8_t> out;
    for (const Message& m : generate(options)) {
        append_framed(out, m);
    }
    return out;
}

}  // namespace enginex::itch
