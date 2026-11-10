// enginex_itch_bench: in-process ITCH 5.0 measurements, each a separate workload:
//
//   itch_decode       StreamFramer + decode() over an in-memory file (no book)
//   itch_book_apply   BookBuilder::apply over pre-decoded messages (no framing/decoding)
//   itch_replay       replay(): framing + decoding + book, from an in-memory file
//
// Input is a seeded synthetic stream (enginex/itch/synthetic.hpp), generated before timing.
// Each trial uses a fresh BookBuilder and the identical bytes/messages; one untimed warm-up
// pass precedes the trials and is reported separately as the "first pass". All trials must
// end with the same book state hash. This binary replaces global operator new/delete with
// counting wrappers (malloc/free) to report heap allocations inside the timed region.
//
// Not measured here: file I/O, cold page cache (cannot be dropped without root on macOS),
// network delivery. In-process numbers are not wire-to-wire latencies.
#include <algorithm>
#include <array>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <new>
#include <string>
#include <vector>

#include "enginex/build_info.hpp"
#include "enginex/itch/book.hpp"
#include "enginex/itch/replay.hpp"
#include "enginex/itch/synthetic.hpp"
#include "enginex/metrics.hpp"

// ---- allocation counting (this executable only) ---------------------------------------------
namespace {
std::uint64_t g_allocs = 0;
std::uint64_t g_alloc_bytes = 0;
}  // namespace

void* operator new(std::size_t n) {
    ++g_allocs;
    g_alloc_bytes += n;
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

namespace {

using namespace enginex;
using namespace enginex::itch;
namespace bi = enginex::build_info;

struct Options {
    std::uint64_t seed = 42;
    std::size_t messages = 1'000'000;
    std::uint16_t symbols = 8;
    std::size_t trials = 7;
    bool latency = true;
    // Before every timed pass, stream through this many MiB (0 = off) so the workload and book
    // structures start outside the CPU caches ("CPU-cache-cold"; the page cache stays warm).
    std::size_t flush_mib = 0;
    std::string out;
    std::string only;  // run a single measurement
    std::size_t reserve = 0;  // BookBuilder expected_orders hint (0 = none)
};

struct Trial {
    std::string trial;  // "first_pass", "1".., "median"
    std::uint64_t elapsed_ns = 0;
    double msgs_per_s = 0;
    double mib_per_s = 0;
    std::uint64_t allocs = 0;
    std::uint64_t alloc_bytes = 0;
    metrics::LatencyStats latency;
    std::uint64_t live_orders = 0;
    std::uint64_t peak_live_orders = 0;
    std::string state_hash;
    // Latency pass only: per message type, total ns and count (a profile substitute where no
    // sampling profiler can attach; each sample includes one timer call).
    std::array<std::uint64_t, 256> type_ns{};
    std::array<std::uint64_t, 256> type_count{};
};

struct Workload {
    std::vector<std::uint8_t> file;
    std::vector<Message> messages;
};

std::string utc_now() {
    const std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

// Median of each column independently (for an even count: mean of the two middle values).
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 == 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}
std::uint64_t median_u(std::vector<std::uint64_t> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 == 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

// One pass of a measurement. `with_latency` adds per-message timers (book_apply only).
Trial run_pass(const std::string& m, const Workload& w, bool with_latency, std::size_t reserve) {
    Trial t;
    const std::uint64_t a0 = g_allocs;
    const std::uint64_t b0 = g_alloc_bytes;
    std::uint64_t frames = 0;
    if (m == "itch_decode") {
        std::uint64_t checksum = 0;
        StreamFramer f;
        const auto start = metrics::Clock::now();
        (void)f.feed(w.file, [&](std::span<const std::uint8_t> p) {
            const DecodeResult r = decode(p);
            if (const auto* msg = std::get_if<Message>(&r)) {
                checksum += header_of(*msg).timestamp_ns ^ msg->index();
            }
        });
        const auto end = metrics::Clock::now();
        t.elapsed_ns = metrics::elapsed_ns(start, end);
        t.allocs = g_allocs - a0;
        t.alloc_bytes = g_alloc_bytes - b0;
        frames = f.frames();
        t.state_hash = std::to_string(checksum);  // must match across trials
    } else if (m == "itch_book_apply") {
        BookBuilder book(reserve);
        const std::uint64_t a1 = g_allocs;  // exclude the builder's constructor
        const std::uint64_t b1 = g_alloc_bytes;
        std::vector<std::uint64_t> samples;
        if (with_latency) {
            samples.reserve(w.messages.size());
        }
        const auto start = metrics::Clock::now();
        if (with_latency) {
            for (const Message& msg : w.messages) {
                const auto s = metrics::Clock::now();
                (void)book.apply(msg);
                const std::uint64_t ns = metrics::elapsed_ns(s, metrics::Clock::now());
                samples.push_back(ns);
                const auto type = static_cast<unsigned char>(header_of(msg).type);
                t.type_ns[type] += ns;
                ++t.type_count[type];
            }
        } else {
            for (const Message& msg : w.messages) {
                (void)book.apply(msg);
            }
        }
        const auto end = metrics::Clock::now();
        t.elapsed_ns = metrics::elapsed_ns(start, end);
        t.allocs = g_allocs - a1 - (with_latency ? 1 : 0);
        t.alloc_bytes = g_alloc_bytes - b1;
        frames = w.messages.size();
        if (with_latency) {
            t.latency = metrics::summarize(samples);
        }
        t.live_orders = book.stats().live_orders;
        t.peak_live_orders = book.stats().peak_live_orders;
        t.state_hash = book.state_hash();
    } else {  // itch_replay
        BookBuilder book(reserve);
        const std::uint64_t a1 = g_allocs;
        const std::uint64_t b1 = g_alloc_bytes;
        const auto start = metrics::Clock::now();
        const ReplayResult r = replay(w.file, book);
        const auto end = metrics::Clock::now();
        t.elapsed_ns = metrics::elapsed_ns(start, end);
        t.allocs = g_allocs - a1;
        t.alloc_bytes = g_alloc_bytes - b1;
        frames = r.frames;
        if (!r.ok() || book.stats().anomaly_count() != 0) {
            throw std::runtime_error("replay of the synthetic stream was not clean");
        }
        t.live_orders = book.stats().live_orders;
        t.peak_live_orders = book.stats().peak_live_orders;
        t.state_hash = book.state_hash();
    }
    const double secs = static_cast<double>(t.elapsed_ns) / 1e9;
    t.msgs_per_s = static_cast<double>(frames) / secs;
    t.mib_per_s = m == "itch_book_apply" ? 0 : static_cast<double>(w.file.size()) / secs / 1048576.0;
    return t;
}

// Evicts CPU caches by writing and reading a buffer much larger than the last-level cache.
void flush_caches(std::size_t mib) {
    if (mib == 0) {
        return;
    }
    static std::vector<std::uint64_t> junk;
    junk.resize(mib * 1024 * 1024 / sizeof(std::uint64_t));
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < junk.size(); ++i) {
        junk[i] += i;
    }
    for (const std::uint64_t v : junk) {
        sum += v;
    }
    volatile std::uint64_t sink = sum;
    (void)sink;
}

std::string csv_header() {
    return "started_utc,git_sha,compiler,build_type,system,measurement,seed,messages,symbols,"
           "file_bytes,trial,elapsed_ns,msgs_per_s,ns_per_msg,mib_per_s,allocs,alloc_bytes,"
           "lat_mean_ns,lat_p50_ns,lat_p90_ns,lat_p99_ns,lat_p999_ns,lat_max_ns,timer_call_ns,"
           "timer_tick_ns,peak_rss_kib,live_orders,peak_live_orders,state_hash_prefix,order_index,"
           "reserve,level_alloc,flush_cache_mib";
}

void write_row(std::ostream& out, const std::string& started, const std::string& m,
               const Options& o, const Workload& w, const Trial& t,
               const metrics::TimerCharacteristics& timer, std::uint64_t rss) {
    char buf[4096];
    const int len = std::snprintf(
        buf, sizeof buf,
        "%s,%s,%s %s,%s,%s,%s,%llu,%zu,%u,%zu,%s,%llu,%.0f,%.2f,%.1f,%llu,%llu,%.1f,%llu,%llu,%llu,"
        "%llu,%llu,%.1f,%llu,%llu,%llu,%llu,%s,%s,%zu,%s,%zu\n",
        started.c_str(), bi::kGitSha, bi::kCompilerId, bi::kCompilerVersion, bi::kBuildType,
        bi::kSystem, m.c_str(), static_cast<unsigned long long>(o.seed), w.messages.size(),
        static_cast<unsigned>(o.symbols), w.file.size(), t.trial.c_str(),
        static_cast<unsigned long long>(t.elapsed_ns), t.msgs_per_s, 1e9 / t.msgs_per_s,
        t.mib_per_s, static_cast<unsigned long long>(t.allocs),
        static_cast<unsigned long long>(t.alloc_bytes), t.latency.mean_ns,
        static_cast<unsigned long long>(t.latency.p50_ns),
        static_cast<unsigned long long>(t.latency.p90_ns),
        static_cast<unsigned long long>(t.latency.p99_ns),
        static_cast<unsigned long long>(t.latency.p999_ns),
        static_cast<unsigned long long>(t.latency.max_ns), timer.mean_call_ns,
        static_cast<unsigned long long>(timer.min_nonzero_tick_ns),
        static_cast<unsigned long long>(rss), static_cast<unsigned long long>(t.live_orders),
        static_cast<unsigned long long>(t.peak_live_orders), t.state_hash.substr(0, 16).c_str(),
        enginex::itch::kOrderIndexName, o.reserve, enginex::itch::kLevelAllocName, o.flush_mib);
    if (len < 0 || static_cast<std::size_t>(len) >= sizeof buf) {
        throw std::runtime_error("CSV row does not fit the buffer");
    }
    out << buf;
}

int run(const Options& o) {
    SyntheticOptions so;
    so.seed = o.seed;
    so.messages = o.messages;
    so.symbols = o.symbols;
    Workload w;
    w.messages = generate(so);
    for (const Message& m : w.messages) {
        append_framed(w.file, m);
    }
    const metrics::TimerCharacteristics timer = metrics::measure_timer();
    const std::string started = utc_now();
    std::ofstream csv;
    if (!o.out.empty()) {
        std::ifstream probe(o.out);
        const bool fresh = !probe || probe.peek() == std::ifstream::traits_type::eof();
        if (!fresh) {
            std::string header;
            std::getline(probe, header);
            if (header != csv_header()) {
                throw std::runtime_error(o.out + ": existing CSV has a different header; "
                                                 "write to a new file");
            }
        }
        csv.open(o.out, std::ios::app);
        if (fresh) {
            csv << csv_header() << "\n";
        }
    }
    std::printf("enginex_itch_bench: git %s, %s %s %s, %s\n", bi::kGitSha, bi::kCompilerId,
                bi::kCompilerVersion, bi::kBuildType, bi::kSystem);
    std::printf("workload: synthetic ITCH 5.0, seed %llu, %zu messages (%zu lifecycle), %u "
                "symbols, %zu bytes; %zu trials + 1 warm-up\n",
                static_cast<unsigned long long>(o.seed), w.messages.size(), o.messages,
                static_cast<unsigned>(o.symbols), w.file.size(), o.trials);
    std::printf("book reserve hint: %zu orders; order index: %s; level nodes: %s; cache flush "
                "before each pass: %zu MiB\n",
                o.reserve, enginex::itch::kOrderIndexName, enginex::itch::kLevelAllocName,
                o.flush_mib);
    std::printf("timer: now() ~%.1f ns, smallest tick %llu ns\n", timer.mean_call_ns,
                static_cast<unsigned long long>(timer.min_nonzero_tick_ns));
    for (const std::string m : {"itch_decode", "itch_book_apply", "itch_replay"}) {
        if (!o.only.empty() && o.only != m) {
            continue;
        }
        flush_caches(o.flush_mib);
        Trial first = run_pass(m, w, false, o.reserve);
        first.trial = "first_pass";
        std::vector<Trial> trials;
        for (std::size_t i = 0; i < o.trials; ++i) {
            flush_caches(o.flush_mib);
            Trial t = run_pass(m, w, false, o.reserve);
            if (o.latency && m == "itch_book_apply") {
                flush_caches(o.flush_mib);
                const Trial lat = run_pass(m, w, true, o.reserve);
                if (lat.state_hash != t.state_hash) {
                    throw std::runtime_error("latency pass diverged");
                }
                t.latency = lat.latency;
                t.type_ns = lat.type_ns;
                t.type_count = lat.type_count;
            }
            t.trial = std::to_string(i + 1);
            if (t.state_hash != first.state_hash) {
                throw std::runtime_error(m + ": trial state diverged");
            }
            trials.push_back(t);
        }
        Trial med = trials.front();
        med.trial = "median";
        auto col = [&](auto f) {
            std::vector<double> v;
            for (const auto& t : trials) v.push_back(static_cast<double>(f(t)));
            return median(v);
        };
        auto colu = [&](auto f) {
            std::vector<std::uint64_t> v;
            for (const auto& t : trials) v.push_back(f(t));
            return median_u(v);
        };
        med.elapsed_ns = colu([](const Trial& t) { return t.elapsed_ns; });
        med.msgs_per_s = col([](const Trial& t) { return t.msgs_per_s; });
        med.mib_per_s = col([](const Trial& t) { return t.mib_per_s; });
        med.allocs = colu([](const Trial& t) { return t.allocs; });
        med.alloc_bytes = colu([](const Trial& t) { return t.alloc_bytes; });
        med.latency.mean_ns = col([](const Trial& t) { return t.latency.mean_ns; });
        med.latency.p50_ns = colu([](const Trial& t) { return t.latency.p50_ns; });
        med.latency.p90_ns = colu([](const Trial& t) { return t.latency.p90_ns; });
        med.latency.p99_ns = colu([](const Trial& t) { return t.latency.p99_ns; });
        med.latency.p999_ns = colu([](const Trial& t) { return t.latency.p999_ns; });
        med.latency.max_ns = colu([](const Trial& t) { return t.latency.max_ns; });
        double lo = trials.front().msgs_per_s;
        double hi = lo;
        for (const auto& t : trials) {
            lo = std::min(lo, t.msgs_per_s);
            hi = std::max(hi, t.msgs_per_s);
        }
        const std::uint64_t rss = metrics::peak_rss_kib();
        std::printf("[%s]\n  first pass %.2f M msg/s; trials median %.2f M msg/s [min %.2f, max %.2f]"
                    " = %.1f ns/msg",
                    m.c_str(), first.msgs_per_s / 1e6, med.msgs_per_s / 1e6, lo / 1e6, hi / 1e6,
                    1e9 / med.msgs_per_s);
        if (med.mib_per_s > 0) {
            std::printf(", %.0f MiB/s", med.mib_per_s);
        }
        std::printf("\n  heap allocations in timed region: median %llu (%.3f per message), "
                    "first pass %llu\n",
                    static_cast<unsigned long long>(med.allocs),
                    static_cast<double>(med.allocs) / static_cast<double>(w.messages.size()),
                    static_cast<unsigned long long>(first.allocs));
        if (m == "itch_book_apply" && o.latency) {
            std::printf("  per-message latency (ns, median of trials, includes one now() call): "
                        "mean %.1f p50 %llu p90 %llu p99 %llu p99.9 %llu max %llu\n",
                        med.latency.mean_ns, static_cast<unsigned long long>(med.latency.p50_ns),
                        static_cast<unsigned long long>(med.latency.p90_ns),
                        static_cast<unsigned long long>(med.latency.p99_ns),
                        static_cast<unsigned long long>(med.latency.p999_ns),
                        static_cast<unsigned long long>(med.latency.max_ns));
        }
        if (m == "itch_book_apply" && o.latency) {
            std::printf("  mean ns by type (trial 1 latency pass; incl. ~%.0f ns timer):",
                        timer.mean_call_ns);
            for (const char c : kKnownTypes) {
                const auto u = static_cast<unsigned char>(c);
                if (trials.front().type_count[u] != 0) {
                    std::printf(" %c=%.0f(%.0f%%)", c,
                                static_cast<double>(trials.front().type_ns[u]) /
                                    static_cast<double>(trials.front().type_count[u]),
                                100.0 * static_cast<double>(trials.front().type_count[u]) /
                                    static_cast<double>(w.messages.size()));
                }
            }
            std::printf("\n");
        }
        if (m != "itch_decode") {
            std::printf("  live orders at end %llu (peak %llu); state hash %.16s (identical "
                        "across trials)\n",
                        static_cast<unsigned long long>(med.live_orders),
                        static_cast<unsigned long long>(med.peak_live_orders),
                        med.state_hash.c_str());
        }
        std::printf("  peak RSS %llu MiB (process high-water mark so far, includes the workload)\n",
                    static_cast<unsigned long long>(rss / 1024));
        if (csv.is_open()) {
            write_row(csv, started, m, o, w, first, timer, rss);
            for (const auto& t : trials) write_row(csv, started, m, o, w, t, timer, rss);
            write_row(csv, started, m, o, w, med, timer, rss);
        }
    }
    return 0;
}

std::uint64_t parse_u64(const std::string& s) {
    std::size_t pos = 0;
    const unsigned long long v = std::stoull(s, &pos);
    if (pos != s.size()) {
        throw std::invalid_argument("bad number: " + s);
    }
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options o;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::invalid_argument("missing value for " + a);
                return argv[++i];
            };
            if (a == "--seed") o.seed = parse_u64(value());
            else if (a == "--messages") o.messages = parse_u64(value());
            else if (a == "--symbols") o.symbols = static_cast<std::uint16_t>(parse_u64(value()));
            else if (a == "--trials") o.trials = parse_u64(value());
            else if (a == "--out") o.out = value();
            else if (a == "--only") o.only = value();
            else if (a == "--no-latency") o.latency = false;
            else if (a == "--reserve") o.reserve = parse_u64(value());
            else if (a == "--flush-cache-mib") o.flush_mib = parse_u64(value());
            else throw std::invalid_argument("unknown option " + a);
        }
        if (o.trials == 0 || o.symbols == 0) {
            throw std::invalid_argument("--trials and --symbols must be > 0");
        }
        return run(o);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "enginex_itch_bench: %s\n", e.what());
        return 1;
    }
}
