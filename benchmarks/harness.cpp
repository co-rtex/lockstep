#include "harness.hpp"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

#include "enginex/build_info.hpp"
#include "enginex/canonical.hpp"

namespace enginex::bench {

namespace {

struct PassResult {
    std::uint64_t elapsed_ns = 0;
    std::uint64_t cpu_ns = 0;
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t executions = 0;
    std::string state_hash;
};

std::unique_ptr<ExchangeStateMachine> prepared_state(const Workload& w) {
    auto state = std::make_unique<ExchangeStateMachine>(w.config);
    state->reserve(w.prefill.size() + w.measured.size());
    CommandResult scratch;
    for (const auto& cmd : w.prefill) {
        state->process(cmd, scratch);
    }
    return state;
}

PassResult throughput_pass(const Workload& w) {
    auto state = prepared_state(w);
    CommandResult out;
    out.executions.reserve(64);
    PassResult r;
    const ExecSeq exec_before = state->last_execution_sequence();

    const std::uint64_t cpu0 = metrics::process_cpu_ns();
    const auto t0 = metrics::Clock::now();
    for (const auto& cmd : w.measured) {
        state->process(cmd, out);
        r.accepted += out.accepted ? 1U : 0U;
    }
    const auto t1 = metrics::Clock::now();
    const std::uint64_t cpu1 = metrics::process_cpu_ns();

    r.elapsed_ns = metrics::elapsed_ns(t0, t1);
    r.cpu_ns = cpu1 - cpu0;
    r.rejected = w.measured.size() - r.accepted;
    r.executions = state->last_execution_sequence() - exec_before;
    r.state_hash = state_hash(*state);
    return r;
}

PassResult latency_pass(const Workload& w, std::vector<std::uint64_t>& samples) {
    auto state = prepared_state(w);
    CommandResult out;
    out.executions.reserve(64);
    samples.clear();
    samples.reserve(w.measured.size());
    PassResult r;
    for (const auto& cmd : w.measured) {
        const auto t0 = metrics::Clock::now();
        state->process(cmd, out);
        const auto t1 = metrics::Clock::now();
        samples.push_back(metrics::elapsed_ns(t0, t1));
        r.accepted += out.accepted ? 1U : 0U;
    }
    r.state_hash = state_hash(*state);
    return r;
}

template <typename T, typename Fn>
T median_of(const std::vector<TrialResult>& trials, Fn fn) {
    std::vector<T> v;
    for (const auto& t : trials) {
        v.push_back(fn(t));
    }
    std::sort(v.begin(), v.end());
    if (v.empty()) {
        return T{};
    }
    // Lower median for even counts keeps values equal to an observed trial.
    return v[(v.size() - 1) / 2];
}

std::string utc_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string csv_escape(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) {
        return s;
    }
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"') {
            out += "\"\"";
        } else {
            out += c;
        }
    }
    return out + "\"";
}

}  // namespace

BenchReport run_benchmark(const BenchOptions& options) {
    BenchReport report;
    report.options = options;
    report.started_utc = utc_now();
    const Workload w = make_workload(options.scenario);
    report.description = w.description;
    report.stats = w.stats;
    report.timer = metrics::measure_timer();

    for (std::size_t i = 0; i < options.warmup; ++i) {
        (void)throughput_pass(w);
    }

    std::vector<std::uint64_t> samples;
    std::string expected_hash;
    for (std::size_t i = 0; i < options.trials; ++i) {
        const PassResult tp = throughput_pass(w);
        TrialResult t;
        t.trial = i + 1;
        t.elapsed_ns = tp.elapsed_ns;
        t.cpu_ns = tp.cpu_ns;
        t.throughput_per_s = tp.elapsed_ns == 0 ? 0.0
                                                : static_cast<double>(w.measured.size()) * 1e9 /
                                                      static_cast<double>(tp.elapsed_ns);
        t.accepted = tp.accepted;
        t.rejected = tp.rejected;
        t.executions = tp.executions;
        t.state_hash = tp.state_hash;
        if (options.latency) {
            const PassResult lp = latency_pass(w, samples);
            if (lp.state_hash != tp.state_hash || lp.accepted != tp.accepted) {
                throw std::runtime_error("nondeterminism: latency and throughput passes diverged");
            }
            t.latency = metrics::summarize(samples);
        }
        if (expected_hash.empty()) {
            expected_hash = t.state_hash;
        } else if (expected_hash != t.state_hash) {
            throw std::runtime_error("nondeterminism: trials ended in different states");
        }
        if (t.accepted != w.stats.accepted || t.executions != w.stats.executions) {
            throw std::runtime_error("benchmark run diverged from generator shadow run");
        }
        report.trials.push_back(std::move(t));
    }

    const auto& tr = report.trials;
    TrialResult& m = report.median;
    m.elapsed_ns = median_of<std::uint64_t>(tr, [](const auto& t) { return t.elapsed_ns; });
    m.cpu_ns = median_of<std::uint64_t>(tr, [](const auto& t) { return t.cpu_ns; });
    m.throughput_per_s = median_of<double>(tr, [](const auto& t) { return t.throughput_per_s; });
    m.latency.count = tr.empty() ? 0 : tr.front().latency.count;
    m.latency.mean_ns = median_of<double>(tr, [](const auto& t) { return t.latency.mean_ns; });
    m.latency.min_ns = median_of<std::uint64_t>(tr, [](const auto& t) { return t.latency.min_ns; });
    m.latency.p50_ns = median_of<std::uint64_t>(tr, [](const auto& t) { return t.latency.p50_ns; });
    m.latency.p90_ns = median_of<std::uint64_t>(tr, [](const auto& t) { return t.latency.p90_ns; });
    m.latency.p95_ns = median_of<std::uint64_t>(tr, [](const auto& t) { return t.latency.p95_ns; });
    m.latency.p99_ns = median_of<std::uint64_t>(tr, [](const auto& t) { return t.latency.p99_ns; });
    m.latency.p999_ns =
        median_of<std::uint64_t>(tr, [](const auto& t) { return t.latency.p999_ns; });
    m.latency.max_ns = median_of<std::uint64_t>(tr, [](const auto& t) { return t.latency.max_ns; });
    if (!tr.empty()) {
        m.accepted = tr.front().accepted;
        m.rejected = tr.front().rejected;
        m.executions = tr.front().executions;
        m.state_hash = tr.front().state_hash;
        const auto [lo, hi] = std::minmax_element(
            tr.begin(), tr.end(),
            [](const auto& a, const auto& b) { return a.throughput_per_s < b.throughput_per_s; });
        report.throughput_min = lo->throughput_per_s;
        report.throughput_max = hi->throughput_per_s;
    }
    report.peak_rss_kib = metrics::peak_rss_kib();
    return report;
}

std::string csv_header() {
    return "started_utc,git_sha,compiler,build_type,system,measurement,scenario,seed,orders,"
           "symbols,prefill,trial,accepted,rejected,executions,commands_with_fills,cancels,"
           "elapsed_ns,throughput_cmd_per_s,cpu_ns,lat_mean_ns,lat_p50_ns,lat_p90_ns,lat_p95_ns,"
           "lat_p99_ns,lat_p999_ns,lat_max_ns,timer_call_ns,timer_tick_ns,peak_rss_kib,"
           "state_hash_prefix";
}

void append_csv(const BenchReport& report, const std::string& path) {
    bool need_header = true;
    {
        std::ifstream existing(path, std::ios::binary | std::ios::ate);
        need_header = !existing || existing.tellg() <= 0;
    }
    std::ofstream out(path, std::ios::app);
    if (!out) {
        throw std::runtime_error("cannot open CSV output: " + path);
    }
    if (need_header) {
        out << csv_header() << "\n";
    }
    const auto& p = report.options.scenario;
    const std::string compiler =
        std::string(build_info::kCompilerId) + " " + build_info::kCompilerVersion;
    auto row = [&](const std::string& trial, const TrialResult& t) {
        out << report.started_utc << ',' << build_info::kGitSha << ',' << csv_escape(compiler)
            << ',' << build_info::kBuildType << ',' << build_info::kSystem << ",core_in_process,"
            << p.name << ',' << p.seed << ',' << p.orders << ',' << p.symbols << ','
            << report.stats.prefill_commands << ',' << trial << ',' << t.accepted << ','
            << t.rejected << ',' << t.executions << ',' << report.stats.commands_with_fills << ','
            << report.stats.cancels << ',' << t.elapsed_ns << ',' << std::fixed
            << std::setprecision(0) << t.throughput_per_s << ',' << t.cpu_ns << ','
            << std::setprecision(1) << t.latency.mean_ns << ',' << t.latency.p50_ns << ','
            << t.latency.p90_ns << ',' << t.latency.p95_ns << ',' << t.latency.p99_ns << ','
            << t.latency.p999_ns << ',' << t.latency.max_ns << ',' << std::setprecision(1)
            << report.timer.mean_call_ns << ',' << report.timer.min_nonzero_tick_ns << ','
            << report.peak_rss_kib << ',' << t.state_hash.substr(0, 16) << "\n";
    };
    for (const auto& t : report.trials) {
        row(std::to_string(t.trial), t);
    }
    row("median", report.median);
    if (!out) {
        throw std::runtime_error("failed writing CSV output: " + path);
    }
}

void print_report(const BenchReport& r, std::ostream& out) {
    const auto& p = r.options.scenario;
    const auto& s = r.stats;
    const auto& m = r.median;
    auto pct = [](std::size_t a, std::size_t b) {
        return b == 0 ? 0.0 : 100.0 * static_cast<double>(a) / static_cast<double>(b);
    };
    out << std::fixed;
    out << "scenario " << p.name << " (seed " << p.seed << ", " << p.orders
        << " measured commands, " << r.options.trials << " trials + " << r.options.warmup
        << " warm-up)\n"
        << "  workload: " << r.description << "\n"
        << "  prefill " << s.prefill_commands << " cmds; open orders "
        << s.open_orders_after_prefill << " -> " << s.open_orders_at_end << "\n"
        << std::setprecision(1) << "  mix: " << pct(s.new_orders, s.measured_commands) << "% new, "
        << pct(s.cancels, s.measured_commands) << "% cancel; "
        << pct(s.commands_with_fills, s.measured_commands) << "% of commands filled; "
        << s.executions << " executions; max levels swept " << s.max_levels_touched << "\n"
        << std::setprecision(0) << "  throughput (untimed loop): median " << m.throughput_per_s
        << " cmd/s [min " << r.throughput_min << ", max " << r.throughput_max << "]\n";
    if (r.options.latency) {
        out << std::setprecision(1) << "  latency per command (ns, median of trials): mean "
            << m.latency.mean_ns << "  p50 " << m.latency.p50_ns << "  p90 " << m.latency.p90_ns
            << "  p99 " << m.latency.p99_ns << "  p99.9 " << m.latency.p999_ns << "  max "
            << m.latency.max_ns << "\n";
    }
    out << std::setprecision(1) << "  timer: now() call ~" << r.timer.mean_call_ns
        << " ns, smallest observed tick " << r.timer.min_nonzero_tick_ns << " ns; peak RSS "
        << r.peak_rss_kib / 1024 << " MiB\n"
        << "  state hash " << m.state_hash.substr(0, 16) << " (identical across all trials)\n";
}

}  // namespace enginex::bench
