#include "scenarios.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <set>
#include <stdexcept>

#include "enginex/rng.hpp"

namespace enginex::bench {

namespace {

constexpr PriceCents kMid = 10'000;  // $100.00

const std::vector<std::string> kNames{"no-cross",          "high-cross",   "same-price-burst",
                                      "multi-level-sweep", "cancel-heavy", "mixed"};

class Generator {
public:
    explicit Generator(Workload& w) : w_(w), rng_(w.params.seed), shadow_(w.config) {
        shadow_.reserve(w.params.orders + 50'000);
    }

    void begin_measured() {
        measuring_ = true;
        w_.stats.open_orders_after_prefill = shadow_.engine().open_order_count();
    }

    void finish() { w_.stats.open_orders_at_end = shadow_.engine().open_order_count(); }

    [[nodiscard]] std::size_t measured_count() const noexcept { return w_.measured.size(); }

    Rng& rng() noexcept { return rng_; }
    const ExchangeStateMachine& shadow() const noexcept { return shadow_; }

    const std::string& symbol(std::size_t i) const { return w_.config.symbols[i]; }
    const std::string& random_symbol() {
        return w_.config.symbols[rng_.below(w_.config.symbols.size())];
    }

    void new_order(const std::string& symbol, Side side, PriceCents price, Quantity qty) {
        emit(NewLimitOrder{next_request_id(), symbol, side, price, qty});
    }

    // Resting (non-crossing) order a few ticks away from the touch on a random side.
    void passive(const std::string& symbol, std::int64_t max_ticks) {
        const bool buy = rng_.chance(50);
        const std::int64_t ticks = rng_.between(1, max_ticks);
        new_order(symbol, buy ? Side::Buy : Side::Sell, buy ? kMid - ticks : kMid + ticks,
                  rng_.between(1, 100));
    }

    // Cancels a uniformly chosen open order; falls back to a passive order when none is open.
    void cancel_random(std::int64_t fallback_ticks) {
        while (!open_.empty()) {
            const std::size_t idx = static_cast<std::size_t>(rng_.below(open_.size()));
            const OrderId id = open_[idx];
            open_[idx] = open_.back();
            open_.pop_back();
            if (shadow_.engine().is_resting(id)) {
                emit(CancelOrder{next_request_id(), format_order_id(id)});
                return;
            }
        }
        passive(random_symbol(), fallback_ticks);
    }

    // Exact retry of an earlier accepted request (exercises the idempotency path).
    void retry_random() {
        if (accepted_.empty()) {
            passive(random_symbol(), 20);
            return;
        }
        emit(accepted_[static_cast<std::size_t>(rng_.below(accepted_.size()))]);
    }

    void emit(Command cmd) {
        shadow_.process(cmd, result_);
        WorkloadStats& st = w_.stats;
        if (measuring_) {
            ++st.measured_commands;
            if (std::holds_alternative<NewLimitOrder>(cmd)) {
                ++st.new_orders;
            } else {
                ++st.cancels;
            }
            if (result_.accepted) {
                ++st.accepted;
            } else {
                ++st.rejected;
            }
            if (!result_.duplicate) {
                st.executions += result_.executions.size();
                if (!result_.executions.empty()) {
                    ++st.commands_with_fills;
                    std::set<PriceCents> levels;
                    for (const auto& e : result_.executions) {
                        levels.insert(e.price);
                    }
                    st.max_levels_touched = std::max(st.max_levels_touched, levels.size());
                }
            }
        } else {
            ++st.prefill_commands;
        }
        if (result_.accepted && !result_.duplicate) {
            if (accepted_.size() < 4096) {
                accepted_.push_back(cmd);
            }
            if (result_.type == CommandType::NewLimit && result_.remaining > 0) {
                open_.push_back(result_.order_id);
            }
        }
        (measuring_ ? w_.measured : w_.prefill).push_back(std::move(cmd));
    }

private:
    std::string next_request_id() {
        char buf[32];
        std::snprintf(buf, sizeof buf, "r%zu", ++next_request_);
        return buf;
    }

    Workload& w_;
    Rng rng_;
    ExchangeStateMachine shadow_;
    CommandResult result_;
    std::vector<OrderId> open_;      // candidates for cancellation (lazily pruned)
    std::vector<Command> accepted_;  // candidates for retries
    std::size_t next_request_ = 0;
    bool measuring_ = false;
};

void gen_no_cross(Generator& g, std::size_t n) {
    g.begin_measured();
    while (g.measured_count() < n) {
        g.passive(g.random_symbol(), 500);
    }
}

void gen_high_cross(Generator& g, std::size_t n, std::size_t symbols) {
    for (std::size_t s = 0; s < symbols; ++s) {
        for (int i = 0; i < 500; ++i) {
            g.passive(g.symbol(s), 10);
        }
    }
    g.begin_measured();
    while (g.measured_count() < n) {
        const bool buy = g.rng().chance(50);
        const std::int64_t ticks = g.rng().between(1, 10);
        g.new_order(g.random_symbol(), buy ? Side::Buy : Side::Sell,
                    buy ? kMid + ticks : kMid - ticks, g.rng().between(1, 100));
    }
}

void gen_same_price_burst(Generator& g, std::size_t n) {
    const std::string& sym = g.symbol(0);
    for (int i = 0; i < 1000; ++i) {
        g.new_order(sym, Side::Sell, kMid, g.rng().between(1, 10));
    }
    g.begin_measured();
    while (g.measured_count() < n) {
        const auto roll = g.rng().below(100);
        if (roll < 60) {
            g.new_order(sym, Side::Sell, kMid, g.rng().between(1, 10));
        } else if (roll < 85) {
            g.new_order(sym, Side::Buy, kMid, g.rng().between(1, 30));
        } else {
            g.cancel_random(1);
        }
    }
}

void gen_multi_level_sweep(Generator& g, std::size_t n, std::size_t symbols) {
    for (std::size_t s = 0; s < symbols; ++s) {
        for (std::int64_t level = 1; level <= 100; ++level) {
            for (int k = 0; k < 5; ++k) {
                g.new_order(g.symbol(s), Side::Sell, kMid + level, g.rng().between(1, 20));
                g.new_order(g.symbol(s), Side::Buy, kMid - level, g.rng().between(1, 20));
            }
        }
    }
    g.begin_measured();
    while (g.measured_count() < n) {
        const std::size_t s = static_cast<std::size_t>(g.rng().below(symbols));
        if (g.rng().chance(90)) {
            g.passive(g.symbol(s), 100);
            continue;
        }
        // Sweep: target K levels and size the order to consume K-1 levels fully plus part of
        // the K-th, so the order never rests and the book never crosses mid.
        const Side side = g.rng().chance(50) ? Side::Buy : Side::Sell;
        const std::size_t want_levels = static_cast<std::size_t>(g.rng().between(3, 15));
        const OrderBook& book = g.shadow().engine().book(static_cast<SymbolId>(s));
        Quantity qty = 0;
        PriceCents price = 0;
        std::size_t levels = 0;
        book.for_each_level(opposite(side), [&](const PriceLevel& level) {
            ++levels;
            price = level.price;
            qty += level.aggregate;
            return levels < want_levels;
        });
        if (levels < 2 || qty < 2) {
            g.passive(g.symbol(s), 100);
            continue;
        }
        g.new_order(g.symbol(s), side, price, std::min<Quantity>(qty - 1, kMaxQuantity));
    }
}

void gen_cancel_heavy(Generator& g, std::size_t n, std::size_t symbols) {
    for (std::size_t s = 0; s < symbols; ++s) {
        for (int i = 0; i < 2000; ++i) {
            g.passive(g.symbol(s), 50);
        }
    }
    g.begin_measured();
    while (g.measured_count() < n) {
        if (g.rng().chance(50)) {
            g.passive(g.random_symbol(), 50);
        } else {
            g.cancel_random(50);
        }
    }
}

void gen_mixed(Generator& g, std::size_t n, std::size_t symbols) {
    for (std::size_t s = 0; s < symbols; ++s) {
        for (int i = 0; i < 500; ++i) {
            g.passive(g.symbol(s), 25);
        }
    }
    g.begin_measured();
    while (g.measured_count() < n) {
        const auto roll = g.rng().below(100);
        if (roll < 62) {
            const bool buy = g.rng().chance(50);
            const PriceCents price =
                buy ? kMid + g.rng().between(-15, 10) : kMid + g.rng().between(-10, 15);
            g.new_order(g.random_symbol(), buy ? Side::Buy : Side::Sell, price,
                        g.rng().between(1, 200));
        } else if (roll < 97) {
            g.cancel_random(25);
        } else {
            g.retry_random();
        }
    }
}

}  // namespace

const std::vector<std::string>& scenario_names() {
    return kNames;
}

bool is_scenario(const std::string& name) {
    return std::find(kNames.begin(), kNames.end(), name) != kNames.end();
}

std::vector<std::string> benchmark_symbols(std::size_t count) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < count; ++i) {
        if (i == 0) {
            out.emplace_back("AAPL");
        } else if (i == 1) {
            out.emplace_back("MSFT");
        } else {
            std::string digits = std::to_string(i);
            out.push_back("S" + std::string(digits.size() < 3 ? 3 - digits.size() : 0, '0') +
                          digits);
        }
    }
    return out;
}

Workload make_workload(const ScenarioParams& params) {
    if (!is_scenario(params.name)) {
        throw std::invalid_argument("unknown scenario: " + params.name);
    }
    if (params.orders == 0) {
        throw std::invalid_argument("--orders must be positive");
    }
    if (params.symbols == 0 || params.symbols > 999) {
        throw std::invalid_argument("--symbols must be in [1, 999]");
    }
    Workload w;
    w.params = params;
    const bool single_symbol = params.name == "same-price-burst";
    w.config.symbols = benchmark_symbols(single_symbol ? 1 : params.symbols);
    w.config.max_open_orders = std::max<std::size_t>(params.orders + 100'000, 1'000'000);
    w.measured.reserve(params.orders);

    Generator g(w);
    const std::size_t n = params.orders;
    const std::size_t s = w.config.symbols.size();
    if (params.name == "no-cross") {
        w.description = "passive orders 1-500 ticks from mid on random side; book grows, no fills";
        gen_no_cross(g, n);
    } else if (params.name == "high-cross") {
        w.description =
            "500 resting per side per symbol, then aggressive orders 1-10 ticks through mid";
        gen_high_cross(g, n, s);
    } else if (params.name == "same-price-burst") {
        w.description = "one symbol, one price: 60% sells rest, 25% buys consume FIFO, 15% cancels";
        gen_same_price_burst(g, n);
    } else if (params.name == "multi-level-sweep") {
        w.description = "100 levels x 5 orders per side; 90% replenish, 10% sweep 3-15 levels";
        gen_multi_level_sweep(g, n, s);
    } else if (params.name == "cancel-heavy") {
        w.description = "2000 resting per symbol; 50% passive inserts, 50% cancels of open orders";
        gen_cancel_heavy(g, n, s);
    } else {
        w.description = "62% limit orders around mid (partial crosses), 35% cancels, 3% retries";
        gen_mixed(g, n, s);
    }
    g.finish();
    return w;
}

}  // namespace enginex::bench
