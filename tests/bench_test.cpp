// Workload generators and measurement helpers (spec §12): each scenario must actually exercise
// the behavior it is named for, and runs must be deterministic.
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

#include "enginex/canonical.hpp"
#include "enginex/metrics.hpp"
#include "harness.hpp"
#include "scenarios.hpp"

namespace enginex::bench {
namespace {

Workload small(const std::string& name, std::uint64_t seed = 42) {
    return make_workload(ScenarioParams{name, seed, 4000, 2});
}

double pct(std::size_t a, std::size_t b) {
    return 100.0 * static_cast<double>(a) / static_cast<double>(b);
}

TEST(Scenarios, NoCrossNeverTrades) {
    const auto w = small("no-cross");
    EXPECT_EQ(w.stats.executions, 0U);
    EXPECT_EQ(w.stats.cancels, 0U);
    EXPECT_EQ(w.stats.open_orders_at_end, 4000U);
}

TEST(Scenarios, HighCrossTradesFrequently) {
    const auto w = small("high-cross");
    EXPECT_GT(pct(w.stats.commands_with_fills, w.stats.measured_commands), 50.0);
    EXPECT_EQ(w.stats.prefill_commands, 1000U);
}

TEST(Scenarios, SamePriceBurstUsesOnePrice) {
    const auto w = small("same-price-burst");
    EXPECT_EQ(w.config.symbols.size(), 1U);
    std::set<PriceCents> prices;
    for (const auto& c : w.measured) {
        if (const auto* o = std::get_if<NewLimitOrder>(&c)) {
            prices.insert(o->price);
        }
    }
    EXPECT_EQ(prices.size(), 1U);
    EXPECT_GT(w.stats.cancels, 0U);
    EXPECT_GT(w.stats.executions, 0U);
    EXPECT_EQ(w.stats.max_levels_touched, 1U);
}

TEST(Scenarios, MultiLevelSweepCrossesSeveralLevels) {
    const auto w = small("multi-level-sweep");
    EXPECT_GE(w.stats.max_levels_touched, 3U);
    EXPECT_GT(w.stats.commands_with_fills, 0U);
}

TEST(Scenarios, CancelHeavyIsHalfCancels) {
    const auto w = small("cancel-heavy");
    EXPECT_NEAR(pct(w.stats.cancels, w.stats.measured_commands), 50.0, 5.0);
    EXPECT_EQ(w.stats.rejected, 0U);  // cancels only target open orders
}

TEST(Scenarios, MixedBlendsSubmitsCancelsAndFills) {
    const auto w = small("mixed");
    EXPECT_GT(w.stats.cancels, 0U);
    EXPECT_GT(w.stats.commands_with_fills, 0U);
    EXPECT_GT(w.stats.new_orders, w.stats.cancels);
    const auto w4 = make_workload(ScenarioParams{"mixed", 42, 4000, 4});
    EXPECT_EQ(w4.config.symbols.size(), 4U);
}

TEST(Scenarios, GenerationIsDeterministicPerSeed) {
    for (const auto& name : scenario_names()) {
        const auto a = small(name, 7);
        const auto b = small(name, 7);
        const auto c = small(name, 8);
        EXPECT_EQ(a.measured, b.measured) << name;
        EXPECT_EQ(a.prefill, b.prefill) << name;
        EXPECT_NE(a.measured, c.measured) << name;
        EXPECT_EQ(a.measured.size(), 4000U) << name;
    }
}

TEST(Scenarios, RejectsBadParameters) {
    EXPECT_THROW((void)make_workload(ScenarioParams{"nope", 1, 10, 2}), std::invalid_argument);
    EXPECT_THROW((void)make_workload(ScenarioParams{"mixed", 1, 0, 2}), std::invalid_argument);
    EXPECT_THROW((void)make_workload(ScenarioParams{"mixed", 1, 10, 0}), std::invalid_argument);
}

TEST(Harness, RunsAndWritesCsv) {
    BenchOptions opt;
    opt.scenario = ScenarioParams{"mixed", 3, 2000, 2};
    opt.trials = 2;
    opt.warmup = 0;
    const BenchReport report = run_benchmark(opt);
    ASSERT_EQ(report.trials.size(), 2U);
    EXPECT_GT(report.median.throughput_per_s, 0.0);
    EXPECT_EQ(report.median.latency.count, 2000U);
    EXPECT_LE(report.median.latency.p50_ns, report.median.latency.p99_ns);
    EXPECT_EQ(report.trials[0].state_hash, report.trials[1].state_hash);

    const auto path = std::filesystem::temp_directory_path() / "enginex_bench_test.csv";
    std::filesystem::remove(path);
    append_csv(report, path.string());
    append_csv(report, path.string());
    std::ifstream in(path);
    std::string line;
    std::size_t lines = 0;
    std::size_t headers = 0;
    while (std::getline(in, line)) {
        ++lines;
        headers += line == csv_header() ? 1U : 0U;
    }
    EXPECT_EQ(headers, 1U);
    EXPECT_EQ(lines, 1U + 2U * 3U);  // header + (2 trials + median) x 2 appends
    std::filesystem::remove(path);
}

TEST(Metrics, NearestRankPercentiles) {
    std::vector<std::uint64_t> v;
    for (std::uint64_t i = 100; i >= 1; --i) {
        v.push_back(i);
    }
    const auto s = metrics::summarize(v);
    EXPECT_EQ(s.count, 100U);
    EXPECT_EQ(s.min_ns, 1U);
    EXPECT_EQ(s.p50_ns, 50U);
    EXPECT_EQ(s.p90_ns, 90U);
    EXPECT_EQ(s.p99_ns, 99U);
    EXPECT_EQ(s.p999_ns, 100U);
    EXPECT_EQ(s.max_ns, 100U);
    EXPECT_DOUBLE_EQ(s.mean_ns, 50.5);
    std::vector<std::uint64_t> one{7};
    EXPECT_EQ(metrics::summarize(one).p99_ns, 7U);
    std::vector<std::uint64_t> none;
    EXPECT_EQ(metrics::summarize(none).count, 0U);
}

}  // namespace
}  // namespace enginex::bench
