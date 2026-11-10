#pragma once

// JSON conversion at the command/transport boundary (spec §8). The matching core never sees
// JSON; this layer turns text into typed commands and typed results into ordered JSON objects.

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <nlohmann/json.hpp>

#include "enginex/command.hpp"
#include "enginex/order_book.hpp"
#include "enginex/state.hpp"

namespace enginex::codec {

using Json = nlohmann::ordered_json;

inline constexpr std::size_t kDefaultSnapshotDepth = 10;
inline constexpr std::size_t kMaxSnapshotDepth = 1000;

// Read-only queries accepted by `simulate` (they never consume a command sequence).
struct SnapshotQuery {
    std::string symbol;
    std::size_t depth = kDefaultSnapshotDepth;  // 0 = full depth
};
struct ExecutionsQuery {
    std::optional<std::string> symbol;  // nullopt = all symbols
};

using Input = std::variant<Command, SnapshotQuery, ExecutionsQuery>;

// A boundary-level rejection (malformed JSON, wrong types, unknown fields).
struct ParseError {
    ErrorCode code = ErrorCode::InvalidRequest;
    std::string message;
    std::string request_id;  // echoed when it could be extracted
    std::string type;        // echoed when it could be extracted
};

template <typename T>
using Parsed = std::variant<T, ParseError>;

// Parses one JSON text for a command input. Returns a discarded value if the text is malformed;
// sets `repeated_key` if any object in it repeats a key. Such a message is ambiguous (a reader
// keeping the first value and one keeping the last would disagree, e.g. about the idempotency
// key), and the plain parser would silently keep the last value.
[[nodiscard]] Json parse_json(std::string_view text, bool& repeated_key);

// Parses one JSON text into a command or query. Field type problems map to specific codes:
// symbol -> INVALID_SYMBOL, side -> INVALID_SIDE, price -> INVALID_PRICE (also non-integers),
// quantity -> INVALID_QUANTITY; everything else -> INVALID_REQUEST. Range checks are left to
// the state machine so there is one validation rule set.
[[nodiscard]] Parsed<Input> parse_input(std::string_view text);

// Parses a new_limit/cancel command from an already-decoded object. Keys in `extra_allowed`
// (e.g. "protocol_version") are permitted in addition to the command's own fields.
[[nodiscard]] Parsed<Command> parse_command(
    const Json& object, std::initializer_list<std::string_view> extra_allowed = {});

// Typed result -> JSON (field order is fixed).
[[nodiscard]] Json to_json(const CommandResult& result, const ExchangeStateMachine& state);
[[nodiscard]] Json to_json(const Execution& execution, const ExchangeStateMachine& state);
[[nodiscard]] Json to_json(const ParseError& error);
[[nodiscard]] Json levels_to_json(const std::vector<BookLevel>& levels);
[[nodiscard]] Json snapshot_to_json(const ExchangeStateMachine& state, SymbolId symbol,
                                    std::size_t depth);
[[nodiscard]] Json executions_to_json(const ExchangeStateMachine& state,
                                      std::optional<SymbolId> symbol);
// Compact end-of-run summary: watermarks, aggregated books, counts and the canonical hash.
[[nodiscard]] Json summary_to_json(const ExchangeStateMachine& state);

}  // namespace enginex::codec
