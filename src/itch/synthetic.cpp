#include "enginex/itch/synthetic.hpp"

#include <algorithm>
#include <string>

#include "enginex/itch/decoder.hpp"
#include "enginex/rng.hpp"

namespace enginex::itch {

namespace {

constexpr Price4 kTick = 100;  // $0.01 in Price(4)

struct Live {
    OrderRef ref;
    std::uint16_t locate;
    Side side;
    Price4 price;
    std::uint32_t shares;
};

class Generator {
public:
    explicit Generator(const SyntheticOptions& o) : o_(o), rng_(o.seed) {
        for (std::uint16_t i = 0; i < o_.symbols; ++i) {
            mid_.push_back(static_cast<Price4>(rng_.between(1000, 50000)) * kTick);
        }
    }

    std::vector<Message> run() {
        out_.reserve(o_.messages + 2 * o_.symbols + 8);
        system('O');
        for (std::uint16_t i = 0; i < o_.symbols; ++i) {
            directory(static_cast<std::uint16_t>(i + 1));
        }
        system('S');
        system('Q');
        for (std::size_t n = 0; n < o_.messages; ++n) {
            if (o_.anomalies_per_million != 0 &&
                rng_.below(1'000'000) < o_.anomalies_per_million) {
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
    Header header(char type, std::uint16_t locate) {
        ts_ += rng_.below(5000);
        return Header{type, locate, static_cast<std::uint16_t>(rng_.below(4)), ts_};
    }
    void system(char code) { out_.emplace_back(SystemEvent{header('S', 0), code}); }

    void directory(std::uint16_t locate) {
        StockDirectory d;
        d.h = header('R', locate);
        d.stock = make_symbol("SYM" + std::to_string(locate));
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

    std::uint16_t random_locate() { return static_cast<std::uint16_t>(rng_.below(o_.symbols) + 1); }

    Price4 price_for(std::uint16_t locate, Side side) {
        Price4& mid = mid_[locate - 1U];
        if (rng_.chance(5)) {
            mid = rng_.chance(50) ? mid + kTick : std::max<Price4>(kTick, mid - kTick);
        }
        auto ticks = static_cast<std::int64_t>(rng_.below(20));
        if (rng_.chance(1)) {
            ticks = -ticks;  // occasionally through the mid: temporarily crossed books
        }
        const std::int64_t p = side == Side::Buy ? static_cast<std::int64_t>(mid) - ticks * kTick
                                                 : static_cast<std::int64_t>(mid) + ticks * kTick;
        return static_cast<Price4>(std::max<std::int64_t>(kTick, p));
    }

    void add() {
        const std::uint16_t locate = random_locate();
        AddOrder a;
        const bool mpid = rng_.chance(15);
        a.h = header(mpid ? 'F' : 'A', locate);
        a.order_ref = next_ref_++;
        a.side = rng_.chance(50) ? Side::Buy : Side::Sell;
        a.shares = static_cast<std::uint32_t>(rng_.chance(70) ? 100 * rng_.between(1, 10)
                                                               : rng_.between(1, 5000));
        a.stock = make_symbol("SYM" + std::to_string(locate));
        a.price = price_for(locate, a.side);
        if (mpid) {
            a.attribution = Mpid{'M', 'P', static_cast<char>('A' + rng_.below(26)), 'X'};
        }
        live_.push_back(Live{a.order_ref, locate, a.side, a.price, a.shares});
        out_.emplace_back(a);
    }

    std::size_t pick() { return static_cast<std::size_t>(rng_.below(live_.size())); }
    void drop(std::size_t i) {
        live_[i] = live_.back();
        live_.pop_back();
    }

    void event() {
        if (live_.empty()) {
            add();
            return;
        }
        const std::uint64_t r = rng_.below(100);
        if (r < 40) {
            add();
        } else if (r < 62) {
            const std::size_t i = pick();
            out_.emplace_back(OrderDelete{header('D', live_[i].locate), live_[i].ref});
            drop(i);
        } else if (r < 72) {
            const std::size_t i = pick();
            Live& o = live_[i];
            OrderReplace u{header('U', o.locate), o.ref, next_ref_++, 0, 0};
            u.shares = static_cast<std::uint32_t>(rng_.between(1, 5000));
            u.price = rng_.chance(30) ? o.price : price_for(o.locate, o.side);
            o.ref = u.new_ref;
            o.shares = u.shares;
            o.price = u.price;
            out_.emplace_back(u);
        } else if (r < 82) {
            reduce('X');
        } else if (r < 92) {
            reduce('E');
        } else if (r < 95) {
            reduce('C');
        } else if (r < 98) {
            const std::uint16_t locate = random_locate();
            out_.emplace_back(Trade{header('P', locate), 0, 'B',
                                    static_cast<std::uint32_t>(rng_.between(1, 1000)),
                                    make_symbol("SYM" + std::to_string(locate)),
                                    price_for(locate, Side::Buy), next_match_++});
        } else {
            other();
        }
    }

    // X/E/C on a random live order: partial or (for executions) full.
    void reduce(char type) {
        const std::size_t i = pick();
        Live& o = live_[i];
        std::uint32_t n = o.shares;
        if (type == 'X' || rng_.chance(60)) {
            if (o.shares == 1) {
                n = 1;
            } else {
                n = static_cast<std::uint32_t>(rng_.between(1, o.shares - 1));  // partial
            }
        }
        const Header h = header(type, o.locate);
        if (type == 'X') {
            out_.emplace_back(OrderCancel{h, o.ref, n});
        } else if (type == 'E') {
            out_.emplace_back(OrderExecuted{h, o.ref, n, next_match_++});
        } else {
            out_.emplace_back(OrderExecutedWithPrice{h, o.ref, n, next_match_++, rng_.chance(80),
                                                     o.price + kTick});
        }
        o.shares -= n;
        if (o.shares == 0) {
            drop(i);
        }
    }

    void other() {
        const std::uint16_t locate = random_locate();
        const Symbol s = make_symbol("SYM" + std::to_string(locate));
        switch (rng_.below(4)) {
            case 0:
                out_.emplace_back(CrossTrade{header('Q', locate), rng_.below(100000), s,
                                             mid_[locate - 1U], next_match_++, 'O'});
                break;
            case 1:
                out_.emplace_back(BrokenTrade{header('B', locate),
                                              next_match_ > 1 ? rng_.below(next_match_ - 1) + 1
                                                              : 1});
                break;
            case 2:
                out_.emplace_back(Noii{header('I', locate), rng_.below(10000), rng_.below(1000),
                                       'B', s, 0, 0, mid_[locate - 1U], 'C', 'L'});
                break;
            default:
                out_.emplace_back(StockTradingAction{header('H', locate), s, 'T', ' ',
                                                     {' ', ' ', ' ', ' '}});
                break;
        }
    }

    // A message that contradicts the generator's own model of the book.
    void anomaly() {
        const std::uint16_t locate = random_locate();
        switch (rng_.below(4)) {
            case 0:  // unknown reference (never issued)
                out_.emplace_back(OrderExecuted{header('E', locate), next_ref_ + 1'000'000'000ULL,
                                                1, next_match_++});
                break;
            case 1:
                if (!live_.empty()) {  // over-cancel a live order
                    const Live& o = live_[pick()];
                    out_.emplace_back(OrderCancel{header('X', o.locate), o.ref, o.shares + 1});
                }
                break;
            case 2:
                if (!live_.empty()) {  // duplicate add of a live reference
                    const Live& o = live_[pick()];
                    AddOrder a;
                    a.h = header('A', o.locate);
                    a.order_ref = o.ref;
                    a.side = o.side;
                    a.shares = 100;
                    a.stock = make_symbol("SYM" + std::to_string(o.locate));
                    a.price = o.price;
                    out_.emplace_back(a);
                }
                break;
            default:
                if (!live_.empty() && o_.symbols > 1) {  // delete with the wrong locate
                    const Live& o = live_[pick()];
                    const auto wrong = static_cast<std::uint16_t>(o.locate % o_.symbols + 1);
                    out_.emplace_back(OrderDelete{header('D', wrong), o.ref});
                }
                break;
        }
    }

    SyntheticOptions o_;
    Rng rng_;
    std::vector<Message> out_;
    std::vector<Live> live_;
    std::vector<Price4> mid_;
    std::uint64_t ts_ = 34'200'000'000'000ULL;  // 09:30:00 in ns since midnight
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
