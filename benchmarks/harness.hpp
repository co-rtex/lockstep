#pragma once

// In-process benchmark harness for the matching core (spec §12, measurement type 1: "pure
// matching latency"). It times ExchangeStateMachine::process — validation, sequencing,
// idempotency bookkeeping and matching — with no I/O, JSON, logging or durability.
//
// Each trial runs on a fresh state with the same pre-generated commands:
//   1. throughput pass: one timer around the whole measured command loop (no per-op timers);
//   2. latency pass:    steady_clock::now() around every command, samples stored in memory.
// Both passes must end in the same canonical state hash, which is checked after timing.

#include <ostream>
#include <string>
#include <vector>

#include "enginex/metrics.hpp"
#include "scenarios.hpp"

namespace enginex::bench {

enum class Measurement : std::uint8_t {
    Core,      // spec §12.1 (1): in-process ExchangeStateMachine::process
    Durable,   // spec §12.1 (2): DurableExchange::process = validate+apply+write+sync+response
    Recovery,  // spec §12.1 (4): open + validate + replay a journal of N records
};

struct BenchOptions {
    ScenarioParams scenario;
    std::size_t trials = 5;
    std::size_t warmup = 1;
    bool latency = true;
    Measurement measurement = Measurement::Core;
    std::string durability = "sync";  // Durable: sync | fsync | none
    std::string journal_dir;          // Durable/Recovery: where temporary journals go
};

struct TrialResult {
    std::size_t trial = 0;
    std::uint64_t elapsed_ns = 0;
    double throughput_per_s = 0;
    std::uint64_t cpu_ns = 0;
    metrics::LatencyStats latency;
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t executions = 0;
    std::string state_hash;
};

struct BenchReport {
    BenchOptions options;
    std::string description;
    WorkloadStats stats;
    metrics::TimerCharacteristics timer;
    std::vector<TrialResult> trials;
    TrialResult median;  // per-column median across measured trials
    double throughput_min = 0;
    double throughput_max = 0;
    std::uint64_t peak_rss_kib = 0;
    std::string started_utc;
};

[[nodiscard]] BenchReport run_benchmark(const BenchOptions& options);

// CSV `measurement` label, e.g. "core_in_process", "durable_ack_sync", "recovery_replay".
[[nodiscard]] std::string measurement_label(const BenchOptions& options);

// Appends one row per trial plus a "median" row; writes the header when the file is new/empty.
void append_csv(const BenchReport& report, const std::string& path);

void print_report(const BenchReport& report, std::ostream& out);

[[nodiscard]] std::string csv_header();

}  // namespace enginex::bench
