#pragma once

// Seeded synthetic workloads (spec §12.2). Generation happens entirely outside timed sections.
// Cancel targets are chosen by running the commands through a shadow state machine while
// generating, so every workload is deterministic and its accepted/cancelled mix is known.

#include <cstdint>
#include <string>
#include <vector>

#include "enginex/command.hpp"
#include "enginex/state.hpp"

namespace enginex::bench {

struct ScenarioParams {
    std::string name;
    std::uint64_t seed = 42;
    std::size_t orders = 100'000;  // measured commands
    std::size_t symbols = 2;       // number of symbols for multi-symbol scenarios
};

// Observed properties of a generated workload, from the shadow run. Used to validate that a
// scenario actually exercises what it claims (e.g. high-cross really crosses).
struct WorkloadStats {
    std::size_t prefill_commands = 0;
    std::size_t measured_commands = 0;
    std::size_t new_orders = 0;
    std::size_t cancels = 0;
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    std::size_t executions = 0;
    std::size_t commands_with_fills = 0;
    std::size_t open_orders_after_prefill = 0;
    std::size_t open_orders_at_end = 0;
    std::size_t max_levels_touched = 0;  // most price levels consumed by one command
};

struct Workload {
    ScenarioParams params;
    std::string description;
    StateConfig config;
    std::vector<Command> prefill;   // applied before timing starts (initial book population)
    std::vector<Command> measured;  // timed commands
    WorkloadStats stats;
};

[[nodiscard]] const std::vector<std::string>& scenario_names();
[[nodiscard]] bool is_scenario(const std::string& name);

// Builds a workload; throws std::invalid_argument for unknown scenarios or bad parameters.
[[nodiscard]] Workload make_workload(const ScenarioParams& params);

// Symbol catalog used by benchmarks: AAPL, MSFT, then S002, S003, ...
[[nodiscard]] std::vector<std::string> benchmark_symbols(std::size_t count);

}  // namespace enginex::bench
