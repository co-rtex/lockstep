// enginex_bench: runs every core scenario with identical parameters and appends results to one
// CSV. Equivalent to calling `enginex benchmark --scenario X` for each scenario.
#include <charconv>
#include <cstring>
#include <iostream>
#include <string>

#include "harness.hpp"

namespace {

bool parse_u64(const char* text, std::uint64_t& out) {
    const char* end = text + std::strlen(text);
    const auto [ptr, ec] = std::from_chars(text, end, out);
    return ec == std::errc{} && ptr == end && end != text;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 42;
    std::uint64_t orders = 100'000;
    std::uint64_t trials = 5;
    std::uint64_t symbols = 2;
    std::string out;
    std::string only;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        bool ok = true;
        if (arg == "--seed" && has_value) {
            ok = parse_u64(argv[++i], seed);
        } else if (arg == "--orders" && has_value) {
            ok = parse_u64(argv[++i], orders);
        } else if (arg == "--trials" && has_value) {
            ok = parse_u64(argv[++i], trials);
        } else if (arg == "--symbols" && has_value) {
            ok = parse_u64(argv[++i], symbols);
        } else if (arg == "--out" && has_value) {
            out = argv[++i];
        } else if (arg == "--scenario" && has_value) {
            only = argv[++i];
        } else {
            ok = false;
        }
        if (!ok) {
            std::cerr << "usage: enginex_bench [--seed N] [--orders N] [--trials N] [--symbols N]"
                         " [--scenario NAME] [--out FILE.csv]\n";
            return 2;
        }
    }
    try {
        for (const auto& name : enginex::bench::scenario_names()) {
            if (!only.empty() && name != only) {
                continue;
            }
            enginex::bench::BenchOptions options;
            options.scenario = {name, seed, orders, symbols};
            options.trials = trials;
            const auto report = enginex::bench::run_benchmark(options);
            enginex::bench::print_report(report, std::cout);
            if (!out.empty()) {
                enginex::bench::append_csv(report, out);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "enginex_bench: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
