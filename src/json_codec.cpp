#include "enginex/json_codec.hpp"

#include <algorithm>
#include <limits>

#include "enginex/canonical.hpp"

namespace enginex::codec {

namespace {

ParseError make_error(ErrorCode code, std::string message, const Json& object) {
    ParseError e{code, std::move(message), {}, {}};
    if (object.is_object()) {
        if (const auto it = object.find("request_id"); it != object.end() && it->is_string()) {
            e.request_id = it->get<std::string>();
        }
        if (const auto it = object.find("type"); it != object.end() && it->is_string()) {
            e.type = it->get<std::string>();
        }
    }
    return e;
}

std::optional<std::string_view> unknown_key(const Json& object,
                                            std::initializer_list<std::string_view> own,
                                            std::initializer_list<std::string_view> extra) {
    for (const auto& [key, value] : object.items()) {
        (void)value;
        const bool known = std::find(own.begin(), own.end(), key) != own.end() ||
                           std::find(extra.begin(), extra.end(), key) != extra.end();
        if (!known) {
            return key;
        }
    }
    return std::nullopt;
}

// Reads an exact signed 64-bit integer. Floats (even 100.0), booleans, strings and integers
// outside int64 are rejected.
std::optional<std::int64_t> exact_int64(const Json& value) {
    if (value.is_number_integer() && !value.is_number_unsigned()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number_unsigned()) {
        const auto u = value.get<std::uint64_t>();
        if (u <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return static_cast<std::int64_t>(u);
        }
    }
    return std::nullopt;
}

std::optional<std::string> required_string(const Json& object, const std::string& key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

}  // namespace

Parsed<Command> parse_command(const Json& object,
                              std::initializer_list<std::string_view> extra_allowed) {
    if (!object.is_object()) {
        return ParseError{ErrorCode::InvalidRequest, "command must be a JSON object", {}, {}};
    }
    const auto type = required_string(object, "type");
    if (!type) {
        return make_error(ErrorCode::InvalidRequest, "missing or non-string \"type\"", object);
    }
    const auto request_id = required_string(object, "request_id");

    if (*type == "new_limit") {
        if (auto key =
                unknown_key(object, {"type", "request_id", "symbol", "side", "price", "quantity"},
                            extra_allowed)) {
            return make_error(ErrorCode::InvalidRequest,
                              "unknown field \"" + std::string(*key) + "\"", object);
        }
        if (!request_id) {
            return make_error(ErrorCode::InvalidRequest, "missing or non-string \"request_id\"",
                              object);
        }
        NewLimitOrder order;
        order.request_id = *request_id;
        const auto symbol = required_string(object, "symbol");
        if (!symbol) {
            return make_error(ErrorCode::InvalidSymbol, "missing or non-string \"symbol\"", object);
        }
        order.symbol = *symbol;
        const auto side_text = required_string(object, "side");
        const auto side = side_text ? parse_side(*side_text) : std::nullopt;
        if (!side) {
            return make_error(ErrorCode::InvalidSide, "\"side\" must be \"BUY\" or \"SELL\"",
                              object);
        }
        order.side = *side;
        const auto price_it = object.find("price");
        const auto price = price_it == object.end() ? std::nullopt : exact_int64(*price_it);
        if (!price) {
            return make_error(ErrorCode::InvalidPrice,
                              "\"price\" must be an integer number of cents", object);
        }
        order.price = *price;
        const auto qty_it = object.find("quantity");
        const auto quantity = qty_it == object.end() ? std::nullopt : exact_int64(*qty_it);
        if (!quantity) {
            return make_error(ErrorCode::InvalidQuantity, "\"quantity\" must be an integer",
                              object);
        }
        order.quantity = *quantity;
        return Command{std::move(order)};
    }

    if (*type == "cancel") {
        if (auto key = unknown_key(object, {"type", "request_id", "order_id"}, extra_allowed)) {
            return make_error(ErrorCode::InvalidRequest,
                              "unknown field \"" + std::string(*key) + "\"", object);
        }
        if (!request_id) {
            return make_error(ErrorCode::InvalidRequest, "missing or non-string \"request_id\"",
                              object);
        }
        const auto order_id = required_string(object, "order_id");
        if (!order_id) {
            return make_error(ErrorCode::InvalidRequest, "missing or non-string \"order_id\"",
                              object);
        }
        return Command{CancelOrder{*request_id, *order_id}};
    }

    return make_error(ErrorCode::InvalidRequest, "unknown command type \"" + *type + "\"", object);
}

Parsed<Input> parse_input(std::string_view text) {
    Json object = Json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (object.is_discarded()) {
        return ParseError{ErrorCode::InvalidRequest, "malformed JSON", {}, {}};
    }
    if (!object.is_object()) {
        return ParseError{ErrorCode::InvalidRequest, "input must be a JSON object", {}, {}};
    }
    const auto type = required_string(object, "type");
    if (type && *type == "snapshot") {
        if (auto key = unknown_key(object, {"type", "symbol", "depth"}, {})) {
            return make_error(ErrorCode::InvalidRequest,
                              "unknown field \"" + std::string(*key) + "\"", object);
        }
        SnapshotQuery q;
        const auto symbol = required_string(object, "symbol");
        if (!symbol) {
            return make_error(ErrorCode::InvalidSymbol, "missing or non-string \"symbol\"", object);
        }
        q.symbol = *symbol;
        if (const auto it = object.find("depth"); it != object.end()) {
            const auto depth = exact_int64(*it);
            if (!depth || *depth < 0 || *depth > static_cast<std::int64_t>(kMaxSnapshotDepth)) {
                return make_error(ErrorCode::InvalidRequest,
                                  "\"depth\" must be an integer in [0, 1000]", object);
            }
            q.depth = static_cast<std::size_t>(*depth);
        }
        return Input{std::move(q)};
    }
    if (type && *type == "executions") {
        if (auto key = unknown_key(object, {"type", "symbol"}, {})) {
            return make_error(ErrorCode::InvalidRequest,
                              "unknown field \"" + std::string(*key) + "\"", object);
        }
        ExecutionsQuery q;
        if (object.contains("symbol")) {
            const auto symbol = required_string(object, "symbol");
            if (!symbol) {
                return make_error(ErrorCode::InvalidSymbol, "non-string \"symbol\"", object);
            }
            q.symbol = *symbol;
        }
        return Input{std::move(q)};
    }
    auto command = parse_command(object);
    if (auto* err = std::get_if<ParseError>(&command)) {
        return std::move(*err);
    }
    return Input{std::get<Command>(std::move(command))};
}

Json to_json(const Execution& e, const ExchangeStateMachine& state) {
    Json j;
    j["execution_id"] = format_execution_id(e.execution_sequence);
    j["execution_sequence"] = e.execution_sequence;
    j["command_sequence"] = e.command_sequence;
    j["incoming_order_id"] = format_order_id(e.incoming_order_id);
    j["resting_order_id"] = format_order_id(e.resting_order_id);
    j["symbol"] = state.symbol_name(e.symbol);
    j["incoming_side"] = to_string(e.incoming_side);
    j["price"] = e.price;
    j["quantity"] = e.quantity;
    return j;
}

Json to_json(const CommandResult& r, const ExchangeStateMachine& state) {
    Json j;
    j["request_id"] = r.request_id;
    j["status"] = r.accepted ? "accepted" : "rejected";
    j["type"] = to_string(r.type);
    if (!r.accepted) {
        j["error"] = Json{{"code", to_string(r.error)}, {"message", r.message}};
        j["state_watermark"] = r.state_watermark;
        return j;
    }
    j["duplicate"] = r.duplicate;
    j["command_sequence"] = r.command_sequence;
    j["order_id"] = format_order_id(r.order_id);
    j["symbol"] = state.symbol_name(r.symbol);
    j["side"] = to_string(r.side);
    j["price"] = r.price;
    j["quantity"] = r.quantity;
    if (r.type == CommandType::NewLimit) {
        j["remaining"] = r.remaining;
    } else {
        j["cancelled_quantity"] = r.cancelled_quantity;
    }
    j["order_status"] = to_string(r.order_status);
    Json execs = Json::array();
    for (const auto& e : r.executions) {
        execs.push_back(to_json(e, state));
    }
    j["executions"] = std::move(execs);
    j["state_watermark"] = r.state_watermark;
    return j;
}

Json to_json(const ParseError& error) {
    Json j;
    j["request_id"] = error.request_id;
    j["status"] = "rejected";
    j["type"] = error.type;
    j["error"] = Json{{"code", to_string(error.code)}, {"message", error.message}};
    return j;
}

Json levels_to_json(const std::vector<BookLevel>& levels) {
    Json arr = Json::array();
    for (const auto& l : levels) {
        arr.push_back(Json{{"price", l.price},
                           {"quantity", l.aggregate_quantity},
                           {"order_count", l.resting_order_count}});
    }
    return arr;
}

Json snapshot_to_json(const ExchangeStateMachine& state, SymbolId symbol, std::size_t depth) {
    const BookSnapshot snap = state.engine().snapshot(symbol, depth);
    Json j;
    j["symbol"] = state.symbol_name(symbol);
    j["depth"] = depth;
    j["as_of_command_sequence"] = state.last_command_sequence();
    j["bids"] = levels_to_json(snap.bids);
    j["asks"] = levels_to_json(snap.asks);
    return j;
}

Json executions_to_json(const ExchangeStateMachine& state, std::optional<SymbolId> symbol) {
    Json arr = Json::array();
    for (const auto& e : state.executions()) {
        if (!symbol || e.symbol == *symbol) {
            arr.push_back(to_json(e, state));
        }
    }
    return arr;
}

Json summary_to_json(const ExchangeStateMachine& state) {
    Json j;
    j["last_command_sequence"] = state.last_command_sequence();
    j["last_execution_sequence"] = state.last_execution_sequence();
    j["open_orders"] = state.engine().open_order_count();
    Json books = Json::array();
    for (SymbolId s = 0; s < state.symbols().size(); ++s) {
        const BookSnapshot snap = state.engine().snapshot(s, 0);
        books.push_back(Json{{"symbol", state.symbol_name(s)},
                             {"bids", levels_to_json(snap.bids)},
                             {"asks", levels_to_json(snap.asks)}});
    }
    j["books"] = std::move(books);
    j["state_hash"] = state_hash(state);
    return j;
}

}  // namespace enginex::codec
