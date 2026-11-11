// MoldUDP64 sequencer cost, in process (no sockets): synthetic ITCH messages packed into
// 1400-byte packets and fed to the Sequencer either in order, or with every 20th packet
// (5 %) arriving late by D packets, which forces the reorder buffer.
//
//   enginex_mold_bench [--messages N] [--trials T] [--out FILE.csv]
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <system_error>
#include <vector>

#include "enginex/build_info.hpp"
#include "enginex/itch/decoder.hpp"
#include "enginex/itch/moldudp64.hpp"
#include "enginex/itch/synthetic.hpp"

namespace {

using namespace enginex::itch;

struct Result {
    double ns_per_msg = 0;
    std::uint64_t delivered = 0;
    std::uint64_t peak_buffered = 0;
};

Result run(const mold::PacketStore& store, std::size_t late_every, std::size_t delay) {
    mold::Sequencer::Options o;
    o.max_buffered_messages = std::size_t{1} << 24U;
    o.max_buffered_bytes = std::size_t{1} << 31U;
    mold::Sequencer q(o);
    std::uint64_t n = 0;
    auto sink = [&](std::uint64_t, std::span<const std::uint8_t>) { ++n; };
    std::deque<std::size_t> late;
    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < store.size(); ++i) {
        if (late_every != 0 && i % late_every == 0) {
            late.push_back(i);
        } else {
            q.on_packet(store.packet(i), sink);
        }
        while (!late.empty() && i >= late.front() + delay) {
            q.on_packet(store.packet(late.front()), sink);
            late.pop_front();
        }
    }
    for (const std::size_t j : late) {
        q.on_packet(store.packet(j), sink);
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return Result{s * 1e9 / static_cast<double>(n), n, q.stats().buffered_peak};
}

// An unsigned integer in [1, max], or 0 if the text is not one (std::stoull would abort on
// garbage and silently wrap "-1").
std::uint64_t bounded_arg(const char* text, std::uint64_t max) {
    std::uint64_t v = 0;
    const char* end = text + std::strlen(text);
    const auto [ptr, ec] = std::from_chars(text, end, v);
    return ec == std::errc{} && ptr == end && v >= 1 && v <= max ? v : 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t messages = 2'000'000;
    int trials = 5;
    std::string out;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            std::fprintf(stderr,
                         "usage: enginex_mold_bench [--messages N] [--trials T] [--out FILE]\n");
            return 2;
        }
        if (std::strcmp(argv[i], "--messages") == 0) {
            messages = bounded_arg(argv[i + 1], 100'000'000);
        } else if (std::strcmp(argv[i], "--trials") == 0) {
            trials = static_cast<int>(bounded_arg(argv[i + 1], 1000));
        } else if (std::strcmp(argv[i], "--out") == 0) {
            out = argv[i + 1];
        } else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
        if (messages == 0 || trials == 0) {
            std::fprintf(stderr,
                         "enginex_mold_bench: --messages must be in [1, 100000000] and "
                         "--trials in [1, 1000]\n");
            return 2;
        }
    }
    SyntheticOptions so;
    so.seed = 1;
    so.messages = messages;
    so.symbols = 8;
    const auto stream = generate(so);
    mold::PacketStore store(mold::make_session("BENCH"), 1400);
    for (const Message& m : stream)
        store.append(encode(m));
    store.finish();
    namespace bi = enginex::build_info;
    char started[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(started, sizeof started, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
    std::FILE* csv = nullptr;
    if (!out.empty()) {
        std::FILE* probe = std::fopen(out.c_str(), "r");
        const bool fresh = probe == nullptr;
        if (probe != nullptr)
            std::fclose(probe);
        csv = std::fopen(out.c_str(), "a");
        if (csv == nullptr) {
            std::perror(out.c_str());
            return 1;
        }
        if (fresh) {
            std::fprintf(csv,
                         "started_utc,git_sha,compiler,build_type,system,measurement,messages,"
                         "packets,trial,ns_per_msg,delivered,peak_buffered\n");
        }
    }
    std::printf("enginex_mold_bench: git %s, %s %s %s; %zu packets of %llu messages\n", bi::kGitSha,
                bi::kCompilerId, bi::kCompilerVersion, bi::kBuildType, store.size(),
                static_cast<unsigned long long>(messages));
    struct Case {
        const char* name;
        std::size_t every;
        std::size_t delay;
    };
    const Case cases[] = {
        {"in_order", 0, 0}, {"late5pct_by200", 20, 200}, {"late5pct_by2000", 20, 2000}};
    for (const Case& c : cases) {
        std::vector<Result> rs;
        for (int t = 0; t < trials; ++t)
            rs.push_back(run(store, c.every, c.delay));
        std::vector<Result> sorted = rs;
        std::sort(sorted.begin(), sorted.end(),
                  [](const Result& a, const Result& b) { return a.ns_per_msg < b.ns_per_msg; });
        const Result med = sorted[sorted.size() / 2];
        std::printf("  %-16s median %6.1f ns/msg [min %.1f, max %.1f], peak buffered %llu\n",
                    c.name, med.ns_per_msg, sorted.front().ns_per_msg, sorted.back().ns_per_msg,
                    static_cast<unsigned long long>(med.peak_buffered));
        if (csv != nullptr) {
            for (int t = 0; t <= trials; ++t) {
                const Result& r = t < trials ? rs[static_cast<std::size_t>(t)] : med;
                std::fprintf(csv, "%s,%s,%s %s,%s,%s,mold_sequence_%s,%llu,%zu,%s,%.2f,%llu,%llu\n",
                             started, bi::kGitSha, bi::kCompilerId, bi::kCompilerVersion,
                             bi::kBuildType, bi::kSystem, c.name,
                             static_cast<unsigned long long>(messages), store.size(),
                             t < trials ? std::to_string(t + 1).c_str() : "median", r.ns_per_msg,
                             static_cast<unsigned long long>(r.delivered),
                             static_cast<unsigned long long>(r.peak_buffered));
            }
        }
    }
    if (csv != nullptr)
        std::fclose(csv);
    return 0;
}
