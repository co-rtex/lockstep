#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace enginex::metrics {

using Clock = std::chrono::steady_clock;

[[nodiscard]] inline std::uint64_t elapsed_ns(Clock::time_point start, Clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

// Summary of a set of latency observations (nanoseconds). Percentiles use the nearest-rank
// method: the p-th percentile is the smallest sample with at least p% of samples <= it.
struct LatencyStats {
    std::uint64_t count = 0;
    double mean_ns = 0;
    std::uint64_t min_ns = 0;
    std::uint64_t p50_ns = 0;
    std::uint64_t p90_ns = 0;
    std::uint64_t p95_ns = 0;
    std::uint64_t p99_ns = 0;
    std::uint64_t p999_ns = 0;
    std::uint64_t max_ns = 0;
};

// Sorts `samples` in place and summarizes them. Empty input yields all zeros.
[[nodiscard]] LatencyStats summarize(std::vector<std::uint64_t>& samples);

// Nearest-rank percentile of already-sorted samples; `fraction` in (0, 1].
[[nodiscard]] std::uint64_t percentile_sorted(const std::vector<std::uint64_t>& sorted,
                                              double fraction);

// Measures back-to-back steady_clock::now() calls.
struct TimerCharacteristics {
    double mean_call_ns = 0;                // total time / calls for back-to-back now() calls
    std::uint64_t min_nonzero_tick_ns = 0;  // smallest observed nonzero delta (resolution bound)
};
[[nodiscard]] TimerCharacteristics measure_timer(std::size_t iterations = 200'000);

// Peak resident set size of this process in KiB (0 if unavailable).
[[nodiscard]] std::uint64_t peak_rss_kib();

// Process CPU time (user + system) in nanoseconds.
[[nodiscard]] std::uint64_t process_cpu_ns();

}  // namespace enginex::metrics
