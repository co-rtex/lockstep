#pragma once

// Seeded synthetic ITCH 5.0 message streams for tests, demos and benchmarks.
//
// These are *synthetic fixtures*, generated from the public message formats: they are not
// historical Nasdaq data and say nothing about real market behavior. The generator keeps its
// own model of live orders so that, with anomalies disabled, every modify message refers to a
// live order with enough shares (a "clean" stream that a correct reconstructor replays with
// zero anomalies). Output is a pure function of the options (portable PRNG, no clocks).
//
// Workload (per lifecycle event, after the start-of-day directory):
//   add 40% (A 34%, F 6%) · delete 22% · replace (U) 10% · partial cancel (X) 10% ·
//   execute (E) 10% · execute-with-price (C) 3% · non-displayed trade (P) 3% · other 2%
//   (cross trade Q, broken trade B, NOII I, trading action H)
// Prices: per-symbol mid in [$10, $500] doing a ±1-tick random walk; adds 0–19 ticks away
// from mid on their own side (≈1% cross the mid). Timestamps increase by 0–4999 ns.

#include <cstdint>
#include <vector>

#include "enginex/itch/messages.hpp"

namespace enginex::itch {

struct SyntheticOptions {
    std::uint64_t seed = 1;
    std::size_t messages = 100'000;  // lifecycle messages (excluding start/end-of-day framing)
    std::uint16_t symbols = 8;
    // Per million lifecycle messages: inject a message that contradicts the book (unknown
    // reference, over-execution, duplicate add, wrong locate). 0 = clean stream.
    std::uint32_t anomalies_per_million = 0;
};

[[nodiscard]] std::vector<Message> generate(const SyntheticOptions& options);

// The same stream in the 2-byte length-prefixed file convention.
[[nodiscard]] std::vector<std::uint8_t> generate_file(const SyntheticOptions& options);

[[nodiscard]] Symbol make_symbol(std::string_view text);

}  // namespace enginex::itch
