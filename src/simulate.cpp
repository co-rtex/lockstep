#include "enginex/simulate.hpp"

#include <string>

namespace enginex {

namespace {

bool is_blank(std::string_view line) {
    return line.find_first_not_of(" \t\r") == std::string_view::npos;
}

codec::Json query_error(ErrorCode code, std::string message, std::string type) {
    return codec::to_json(codec::ParseError{code, std::move(message), {}, std::move(type)});
}

}  // namespace

codec::Json simulate_line(ExchangeStateMachine& state, std::string_view line) {
    auto parsed = codec::parse_input(line);
    if (auto* err = std::get_if<codec::ParseError>(&parsed)) {
        return codec::to_json(*err);
    }
    auto& input = std::get<codec::Input>(parsed);

    if (auto* cmd = std::get_if<Command>(&input)) {
        return codec::to_json(state.process(*cmd), state);
    }
    if (auto* snap = std::get_if<codec::SnapshotQuery>(&input)) {
        const auto symbol = state.find_symbol(snap->symbol);
        if (!symbol) {
            return query_error(ErrorCode::InvalidSymbol, "unknown symbol", "snapshot");
        }
        codec::Json j;
        j["type"] = "snapshot";
        j["status"] = "ok";
        j.update(codec::snapshot_to_json(state, *symbol, snap->depth));
        return j;
    }
    const auto& query = std::get<codec::ExecutionsQuery>(input);
    std::optional<SymbolId> symbol;
    if (query.symbol) {
        symbol = state.find_symbol(*query.symbol);
        if (!symbol) {
            return query_error(ErrorCode::InvalidSymbol, "unknown symbol", "executions");
        }
    }
    codec::Json j;
    j["type"] = "executions";
    j["status"] = "ok";
    if (query.symbol) {
        j["symbol"] = *query.symbol;
    }
    j["as_of_command_sequence"] = state.last_command_sequence();
    j["executions"] = codec::executions_to_json(state, symbol);
    return j;
}

codec::Json run_simulation(std::istream& input, const StateConfig& config) {
    ExchangeStateMachine state(config);
    codec::Json responses = codec::Json::array();
    std::string line;
    std::uint64_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (is_blank(line)) {
            continue;
        }
        codec::Json response;
        response["line"] = line_number;
        response.update(simulate_line(state, line));
        responses.push_back(std::move(response));
    }
    codec::Json doc;
    doc["responses"] = std::move(responses);
    doc["final_state"] = codec::summary_to_json(state);
    return doc;
}

}  // namespace enginex
