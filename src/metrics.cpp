#include "enginex/metrics.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>

namespace enginex::metrics {

std::uint64_t percentile_sorted(const std::vector<std::uint64_t>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0;
    }
    const double rank = std::ceil(fraction * static_cast<double>(sorted.size()));
    const auto index = static_cast<std::size_t>(std::max(1.0, rank)) - 1;
    return sorted[std::min(index, sorted.size() - 1)];
}

LatencyStats summarize(std::vector<std::uint64_t>& samples) {
    LatencyStats s;
    if (samples.empty()) {
        return s;
    }
    std::sort(samples.begin(), samples.end());
    s.count = samples.size();
    const long double total =
        std::accumulate(samples.begin(), samples.end(), static_cast<long double>(0));
    s.mean_ns = static_cast<double>(total / static_cast<long double>(samples.size()));
    s.min_ns = samples.front();
    s.p50_ns = percentile_sorted(samples, 0.50);
    s.p90_ns = percentile_sorted(samples, 0.90);
    s.p95_ns = percentile_sorted(samples, 0.95);
    s.p99_ns = percentile_sorted(samples, 0.99);
    s.p999_ns = percentile_sorted(samples, 0.999);
    s.max_ns = samples.back();
    return s;
}

TimerCharacteristics measure_timer(std::size_t iterations) {
    std::vector<std::uint64_t> deltas;
    deltas.reserve(iterations);
    const auto start = Clock::now();
    auto prev = start;
    for (std::size_t i = 0; i < iterations; ++i) {
        const auto now = Clock::now();
        deltas.push_back(elapsed_ns(prev, now));
        prev = now;
    }
    TimerCharacteristics t;
    t.mean_call_ns = iterations == 0 ? 0.0
                                     : static_cast<double>(elapsed_ns(start, prev)) /
                                           static_cast<double>(iterations);
    std::sort(deltas.begin(), deltas.end());
    const auto nonzero = std::upper_bound(deltas.begin(), deltas.end(), std::uint64_t{0});
    t.min_nonzero_tick_ns = nonzero == deltas.end() ? 0 : *nonzero;
    return t;
}

std::uint64_t peak_rss_kib() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss) / 1024U;  // bytes on macOS
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss);  // KiB on Linux
#endif
}

std::uint64_t process_cpu_ns() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
    auto to_ns = [](const timeval& tv) {
        return static_cast<std::uint64_t>(tv.tv_sec) * 1'000'000'000ULL +
               static_cast<std::uint64_t>(tv.tv_usec) * 1'000ULL;
    };
    return to_ns(usage.ru_utime) + to_ns(usage.ru_stime);
}

}  // namespace enginex::metrics
