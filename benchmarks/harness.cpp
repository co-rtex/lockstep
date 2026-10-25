#include "harness.hpp"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>

#include <unistd.h>

#include <filesystem>

#include "enginex/build_info.hpp"
#include "enginex/canonical.hpp"
#include "enginex/durable.hpp"

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

// Private per-run directory (mkdtemp, mode 0700) for benchmark journals, removed afterwards.
// Using a fresh unpredictable directory avoids races with other users of a shared temp dir.
class JournalDir {
public:
    explicit JournalDir(const std::string& parent) {
        const std::filesystem::path base =
            parent.empty() ? std::filesystem::temp_directory_path() : std::filesystem::path(parent);
        std::string templ = (base / "enginex-bench-XXXXXX").string();
        if (::mkdtemp(templ.data()) == nullptr) {
            throw std::runtime_error("cannot create benchmark directory under " + base.string());
        }
        path_ = templ;
    }
    ~JournalDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    JournalDir(const JournalDir&) = delete;
    JournalDir& operator=(const JournalDir&) = delete;
    [[nodiscard]] std::string journal(std::size_t trial) const {
        return (path_ / ("trial-" + std::to_string(trial) + ".exj")).string();
    }

private:
    std::filesystem::path path_;
};

void remove_journal(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// Durable acknowledgment latency: each measured command goes through DurableExchange::process,
// which returns only after its record is written and synchronized (per --durability). The
// prefill is group-committed with one sync and is not timed.
PassResult durable_pass(const Workload& w, const BenchOptions& o, const std::string& path,
                        std::vector<std::uint64_t>& samples) {
    const auto mode = journal::parse_sync_mode(o.durability);
    if (!mode) {
        throw std::invalid_argument("--durability must be sync, fsync or none");
    }
    PassResult r;
    {
        DurableOptions opts;
        opts.journal_path = path;
        opts.sync = *mode;
        opts.state = w.config;
        auto ex = DurableExchange::open(opts);
        CommandResult out;
        for (const auto& cmd : w.prefill) {
            ex->stage(cmd, out);
        }
        if (ex->flush() != ErrorCode::None) {
            throw std::runtime_error("journal flush failed during prefill: " +
                                     ex->failure_reason());
        }
        samples.clear();
        samples.reserve(w.measured.size());
        const ExecSeq exec_before = ex->state().last_execution_sequence();
        const std::uint64_t cpu0 = metrics::process_cpu_ns();
        const auto t0 = metrics::Clock::now();
        for (const auto& cmd : w.measured) {
            const auto c0 = metrics::Clock::now();
            ex->process(cmd, out);
            const auto c1 = metrics::Clock::now();
            samples.push_back(metrics::elapsed_ns(c0, c1));
            if (out.error == ErrorCode::JournalIoError) {
                throw std::runtime_error("journal I/O failure during benchmark: " +
                                         ex->failure_reason());
            }
            r.accepted += out.accepted ? 1U : 0U;
        }
        r.elapsed_ns = metrics::elapsed_ns(t0, metrics::Clock::now());
        r.cpu_ns = metrics::process_cpu_ns() - cpu0;
        r.rejected = w.measured.size() - r.accepted;
        r.state_hash = state_hash(ex->state());
        r.executions = ex->state().last_execution_sequence() - exec_before;
    }
    remove_journal(path);
    return r;
}

// Writes the whole workload (prefill + measured) to a journal once, without timing.
void build_recovery_journal(const Workload& w, const std::string& path) {
    DurableOptions opts;
    opts.journal_path = path;
    opts.sync = journal::SyncMode::Full;
    opts.state = w.config;
    auto ex = DurableExchange::open(opts);
    CommandResult out;
    for (const auto* list : {&w.prefill, &w.measured}) {
        for (const auto& cmd : *list) {
            ex->stage(cmd, out);
        }
    }
    if (ex->flush() != ErrorCode::None) {
        throw std::runtime_error("cannot build recovery journal: " + ex->failure_reason());
    }
}

// Recovery time: DurableExchange::open on an existing journal (read, CRC-validate, decode,
// replay every record through the state machine). The journal was just written, so it is
// read from the OS page cache (warm-cache recovery; cold-cache disk reads are not measured).
PassResult recovery_pass(const Workload& w, const std::string& path, std::uint64_t& records) {
    PassResult r;
    DurableOptions opts;
    opts.journal_path = path;
    opts.state = w.config;
    opts.create_if_missing = false;
    RecoveryReport rep;
    const std::uint64_t cpu0 = metrics::process_cpu_ns();
    const auto t0 = metrics::Clock::now();
    auto ex = DurableExchange::open(opts, &rep);
    r.elapsed_ns = metrics::elapsed_ns(t0, metrics::Clock::now());
    r.cpu_ns = metrics::process_cpu_ns() - cpu0;
    records = rep.records_replayed;
    r.accepted = rep.records_replayed;
    r.executions = ex->state().last_execution_sequence();
    r.state_hash = state_hash(ex->state());
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

std::string measurement_label(const BenchOptions& options) {
    switch (options.measurement) {
        case Measurement::Core:
            return "core_in_process";
        case Measurement::Durable:
            return "durable_ack_" + options.durability;
        case Measurement::Recovery:
            return "recovery_replay";
    }
    return "unknown";
}

BenchReport run_benchmark(const BenchOptions& options) {
    BenchReport report;
    report.options = options;
    report.started_utc = utc_now();
    const Workload w = make_workload(options.scenario);
    report.description = w.description;
    report.stats = w.stats;
    report.timer = metrics::measure_timer();
    const double measured = static_cast<double>(w.measured.size());

    std::vector<std::uint64_t> samples;
    std::string expected_hash;
    std::optional<JournalDir> dir;
    std::string recovery_path;
    if (options.measurement != Measurement::Core) {
        dir.emplace(options.journal_dir);
    }
    if (options.measurement == Measurement::Recovery) {
        recovery_path = dir->journal(0);
        build_recovery_journal(w, recovery_path);
    }
    for (std::size_t i = 0; i < options.warmup + options.trials; ++i) {
        const bool warmup = i < options.warmup;
        TrialResult t;
        t.trial = i + 1 - options.warmup;
        PassResult tp;
        if (options.measurement == Measurement::Core) {
            tp = throughput_pass(w);
            t.throughput_per_s =
                tp.elapsed_ns == 0 ? 0.0 : measured * 1e9 / static_cast<double>(tp.elapsed_ns);
            if (options.latency && !warmup) {
                const PassResult lp = latency_pass(w, samples);
                if (lp.state_hash != tp.state_hash || lp.accepted != tp.accepted) {
                    throw std::runtime_error(
                        "nondeterminism: latency and throughput passes diverged");
                }
                t.latency = metrics::summarize(samples);
            }
            if (tp.accepted != w.stats.accepted || tp.executions != w.stats.executions) {
                throw std::runtime_error("benchmark run diverged from generator shadow run");
            }
        } else if (options.measurement == Measurement::Durable) {
            tp = durable_pass(w, options, dir->journal(i + 1), samples);
            t.throughput_per_s =
                tp.elapsed_ns == 0 ? 0.0 : measured * 1e9 / static_cast<double>(tp.elapsed_ns);
            t.latency = metrics::summarize(samples);
            if (tp.accepted != w.stats.accepted) {
                throw std::runtime_error("durable run diverged from generator shadow run");
            }
        } else {
            std::uint64_t records = 0;
            tp = recovery_pass(w, recovery_path, records);
            t.throughput_per_s = tp.elapsed_ns == 0 ? 0.0
                                                    : static_cast<double>(records) * 1e9 /
                                                          static_cast<double>(tp.elapsed_ns);
            t.latency.count = 1;
            t.latency.mean_ns = static_cast<double>(tp.elapsed_ns);
            t.latency.min_ns = t.latency.p50_ns = t.latency.p90_ns = t.latency.p95_ns =
                t.latency.p99_ns = t.latency.p999_ns = t.latency.max_ns = tp.elapsed_ns;
        }
        t.elapsed_ns = tp.elapsed_ns;
        t.cpu_ns = tp.cpu_ns;
        t.accepted = tp.accepted;
        t.rejected = tp.rejected;
        t.executions = tp.executions;
        t.state_hash = tp.state_hash;
        if (expected_hash.empty()) {
            expected_hash = t.state_hash;
        } else if (expected_hash != t.state_hash) {
            throw std::runtime_error("nondeterminism: trials ended in different states");
        }
        if (!warmup) {
            report.trials.push_back(std::move(t));
        }
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
            << ',' << build_info::kBuildType << ',' << build_info::kSystem << ','
            << measurement_label(report.options) << ','

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
    const Measurement kind = r.options.measurement;
    auto pct = [](std::size_t a, std::size_t b) {
        return b == 0 ? 0.0 : 100.0 * static_cast<double>(a) / static_cast<double>(b);
    };
    out << std::fixed;
    out << "[" << measurement_label(r.options) << "] scenario " << p.name << " (seed " << p.seed
        << ", " << p.orders << " measured commands, " << r.options.trials << " trials + "
        << r.options.warmup << " warm-up)\n"
        << "  workload: " << r.description << "\n"
        << "  prefill " << s.prefill_commands << " cmds; open orders "
        << s.open_orders_after_prefill << " -> " << s.open_orders_at_end << "\n"
        << std::setprecision(1) << "  mix: " << pct(s.new_orders, s.measured_commands) << "% new, "
        << pct(s.cancels, s.measured_commands) << "% cancel; "
        << pct(s.commands_with_fills, s.measured_commands) << "% of commands filled; "
        << s.executions << " executions; max levels swept " << s.max_levels_touched << "\n";
    if (kind == Measurement::Core) {
        out << std::setprecision(0) << "  throughput (untimed loop): median " << m.throughput_per_s
            << " cmd/s [min " << r.throughput_min << ", max " << r.throughput_max << "]\n";
        if (r.options.latency) {
            out << std::setprecision(1) << "  latency per command (ns, median of trials): mean "
                << m.latency.mean_ns << "  p50 " << m.latency.p50_ns << "  p90 " << m.latency.p90_ns
                << "  p99 " << m.latency.p99_ns << "  p99.9 " << m.latency.p999_ns << "  max "
                << m.latency.max_ns << "\n";
        }
    } else if (kind == Measurement::Durable) {
        out << std::setprecision(0) << "  durable acks (" << r.options.durability << "): median "
            << m.throughput_per_s << " cmd/s [min " << r.throughput_min << ", max "
            << r.throughput_max << "], "
            << (r.options.durability == "none" ? "write() per command, no sync"
                                               : "one write() + sync per command")
            << "\n"
            << std::setprecision(1) << "  ack latency (us, median of trials): mean "
            << m.latency.mean_ns / 1e3 << "  p50 " << static_cast<double>(m.latency.p50_ns) / 1e3
            << "  p90 " << static_cast<double>(m.latency.p90_ns) / 1e3 << "  p99 "
            << static_cast<double>(m.latency.p99_ns) / 1e3 << "  p99.9 "
            << static_cast<double>(m.latency.p999_ns) / 1e3 << "  max "
            << static_cast<double>(m.latency.max_ns) / 1e3 << "\n";
    } else {
        out << std::setprecision(1) << "  recovery of " << m.accepted << " records: median "
            << static_cast<double>(m.elapsed_ns) / 1e6 << " ms (" << std::setprecision(0)
            << m.throughput_per_s << " records/s) [min " << r.throughput_min << ", max "
            << r.throughput_max << " records/s]\n";
    }
    out << std::setprecision(1) << "  timer: now() call ~" << r.timer.mean_call_ns
        << " ns, smallest observed tick " << r.timer.min_nonzero_tick_ns << " ns; peak RSS "
        << r.peak_rss_kib / 1024 << " MiB\n"
        << "  state hash " << m.state_hash.substr(0, 16) << " (identical across all trials)\n";
}

}  // namespace enginex::bench
