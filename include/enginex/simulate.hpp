#pragma once

#include <istream>

#include "enginex/json_codec.hpp"
#include "enginex/state.hpp"

namespace enginex {

// Runs a JSON-lines command file against a fresh in-memory state (spec §8.1). Each nonblank line
// produces exactly one response object (in input order, with its 1-based `line` number); the
// document ends with a `final_state` summary that includes the canonical state hash.
//
// Output shape: {"responses": [...], "final_state": {...}}
[[nodiscard]] codec::Json run_simulation(std::istream& input, const StateConfig& config = {});

// Processes one input line against `state`, returning its response object (without `line`).
[[nodiscard]] codec::Json simulate_line(ExchangeStateMachine& state, std::string_view line);

}  // namespace enginex
