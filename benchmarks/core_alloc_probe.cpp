// enginex_core_alloc_probe: heap allocations per command in the synthetic exchange's core
// (ExchangeStateMachine::process), per benchmark scenario. Evidence-gathering for a possible
// allocation/layout change; it measures, it does not change the engine.
//
// Same workloads as enginex_bench (seeded, prefill untimed). Counts calls to global operator
// new inside the measured loop only, split by command outcome. One run per scenario; counts
// are deterministic (no timing involved).
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

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

int main(int argc, char** argv) {
    using namespace enginex;
    const std::size_t orders = argc > 1 ? std::stoull(argv[1]) : 100'000;
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
