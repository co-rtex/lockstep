#include <iostream>

#include "cli.hpp"
#include "harness.hpp"

namespace enginex::cli {

int cmd_benchmark(const std::vector<std::string>& argv) {
    const Args args(argv, {"scenario", "seed", "orders", "trials", "warmup", "symbols", "out"},
                    {"no-latency"});
    bench::BenchOptions options;
    options.scenario.name = args.get_or("scenario", "mixed");
    if (!bench::is_scenario(options.scenario.name)) {
        throw UsageError("unknown scenario: " + options.scenario.name);
    }
    options.scenario.seed = args.get_u64("seed", 42);
    options.scenario.orders = args.get_u64("orders", 100'000);
    options.scenario.symbols = args.get_u64("symbols", 2);
    options.trials = args.get_u64("trials", 5);
    options.warmup = args.get_u64("warmup", 1);
    options.latency = !args.flag("no-latency");
    if (options.trials == 0) {
        throw UsageError("--trials must be positive");
    }

    const bench::BenchReport report = bench::run_benchmark(options);
    bench::print_report(report, std::cout);
    if (const auto out = args.get("out")) {
        bench::append_csv(report, *out);
        std::cout << "  appended " << report.trials.size() + 1 << " rows to " << *out << "\n";
    }
    return 0;
}

}  // namespace enginex::cli
