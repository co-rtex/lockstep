// enginex_core_alloc_probe: heap allocations per command in the synthetic exchange's core
// (ExchangeStateMachine::process), per benchmark scenario. Evidence-gathering for a possible
// allocation/layout change; it measures, it does not change the engine.
//
// Same workloads as enginex_bench (seeded, prefill untimed). Counts calls to global operator
// new inside the measured loop only, split by command outcome. One run per scenario; counts
// are deterministic (no timing involved).
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <system_error>

#include "enginex/state.hpp"
#include "scenarios.hpp"

namespace {
std::uint64_t g_allocs = 0;
std::uint64_t g_bytes = 0;
}  // namespace

void* operator new(std::size_t n) {
    ++g_allocs;
    g_bytes += n;
    if (void* p = std::malloc(n == 0 ? 1 : n)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int run_probe(std::size_t orders);

int main(int argc, char** argv) {
    // Same bound as the benchmark workloads (benchmarks/scenarios.cpp), checked here so a bad
    // argument is a usage error rather than an uncaught exception.
    constexpr std::size_t kMaxOrders = 20'000'000;
    std::size_t orders = 100'000;
    if (argc > 2) {
        std::fprintf(stderr, "usage: enginex_core_alloc_probe [COMMANDS]   (1..20000000, default 100000)\n");
        return 2;
    }
    if (argc == 2) {
        const std::string_view text(argv[1]);
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), orders);
        if (ec != std::errc{} || ptr != text.data() + text.size() || orders == 0 ||
            orders > kMaxOrders) {
            std::fprintf(stderr, "enginex_core_alloc_probe: COMMANDS must be an integer in [1, 20000000]\n");
            return 2;
        }
    }
    try {
        return run_probe(orders);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "enginex_core_alloc_probe: %s\n", e.what());
        return 1;
    }
}

int run_probe(std::size_t orders) {
    using namespace enginex;
    std::printf("%-18s %10s %12s %12s %12s %12s %12s\n", "scenario", "commands", "allocs/cmd",
                "bytes/cmd", "rest/new", "fill/new", "cancel");
    for (const std::string& name : bench::scenario_names()) {
        bench::ScenarioParams p;
        p.name = name;
        p.seed = 42;
        p.orders = orders;
        const bench::Workload w = bench::make_workload(p);
        ExchangeStateMachine state(w.config);
        state.reserve(w.prefill.size() + w.measured.size());
        CommandResult out;
        out.executions.reserve(64);
        for (const auto& cmd : w.prefill) {
            state.process(cmd, out);
        }
        // Allocations by outcome: new order that rests (any remainder), new order fully filled,
        // cancel (accepted or not).
        std::uint64_t a_rest = 0, n_rest = 0, a_fill = 0, n_fill = 0, a_cancel = 0, n_cancel = 0;
        const std::uint64_t a0 = g_allocs;
        const std::uint64_t b0 = g_bytes;
        for (const auto& cmd : w.measured) {
            const std::uint64_t before = g_allocs;
            state.process(cmd, out);
            const std::uint64_t used = g_allocs - before;
            if (std::holds_alternative<CancelOrder>(cmd)) {
                a_cancel += used;
                ++n_cancel;
            } else if (out.accepted && out.remaining > 0) {
                a_rest += used;
                ++n_rest;
            } else {
                a_fill += used;
                ++n_fill;
            }
        }
        const double n = static_cast<double>(w.measured.size());
        auto per = [](std::uint64_t a, std::uint64_t k) {
            return k == 0 ? 0.0 : static_cast<double>(a) / static_cast<double>(k);
        };
        std::printf("%-18s %10zu %12.2f %12.1f %12.2f %12.2f %12.2f\n", name.c_str(),
                    w.measured.size(), static_cast<double>(g_allocs - a0) / n,
                    static_cast<double>(g_bytes - b0) / n, per(a_rest, n_rest),
                    per(a_fill, n_fill), per(a_cancel, n_cancel));
    }
    return 0;
}
