#include "enginex/protocol.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace enginex::net {

using codec::Json;

std::string encode_frame(std::string_view payload, std::size_t max) {
    if (payload.empty() || payload.size() > max) {
        throw std::length_error("frame payload size " + std::to_string(payload.size()) +
                                " outside [1, " + std::to_string(max) + "]");
    }
    const auto n = static_cast<std::uint32_t>(payload.size());
    std::string out;
    out.reserve(kFrameHeaderBytes + payload.size());
    out.push_back(static_cast<char>((n >> 24U) & 0xFFU));
    out.push_back(static_cast<char>((n >> 16U) & 0xFFU));
    out.push_back(static_cast<char>((n >> 8U) & 0xFFU));
    out.push_back(static_cast<char>(n & 0xFFU));
    out.append(payload);
    return out;
}

void FrameDecoder::feed(const char* data, std::size_t size) {
    if (failed_) {
        return;
    }
    // Compact consumed bytes occasionally so the buffer does not grow without bound.
    if (offset_ > 0 && offset_ >= buffer_.size() / 2) {
        buffer_.erase(0, offset_);
        offset_ = 0;
    }
    buffer_.append(data, size);
}

FrameDecoder::Status FrameDecoder::next(std::string& payload) {
    if (failed_) {
        return Status::Error;
    }
    const std::size_t available = buffer_.size() - offset_;
    if (available < kFrameHeaderBytes) {
        return Status::NeedMore;
    }
    const auto* p = reinterpret_cast<const unsigned char*>(buffer_.data() + offset_);
    const std::size_t length =
        (static_cast<std::size_t>(p[0]) << 24U) | (static_cast<std::size_t>(p[1]) << 16U) |
        (static_cast<std::size_t>(p[2]) << 8U) | static_cast<std::size_t>(p[3]);
    if (length == 0 || length > max_frame_) {
        failed_ = true;
        error_ = "invalid frame length " + std::to_string(length) + " (allowed 1.." +
                 std::to_string(max_frame_) + ")";
        return Status::Error;
    }
    if (available - kFrameHeaderBytes < length) {
        return Status::NeedMore;
    }
    payload.assign(buffer_, offset_ + kFrameHeaderBytes, length);
    offset_ += kFrameHeaderBytes + length;
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    }
    return Status::Frame;
}

namespace {

codec::ParseError bad(std::string message, const Json& obj = Json()) {
    codec::ParseError e{ErrorCode::InvalidRequest, std::move(message), {}, {}};
    if (obj.is_object()) {
        if (auto it = obj.find("request_id"); it != obj.end() && it->is_string()) {
            e.request_id = it->get<std::string>();
        }
        if (auto it = obj.find("type"); it != obj.end() && it->is_string()) {
            e.type = it->get<std::string>();
        }
    }
    return e;
}

std::optional<std::string> optional_request_id(const Json& obj, bool& ok) {
    ok = true;
    const auto it = obj.find("request_id");
    if (it == obj.end()) {
        return std::string();
    }
    if (!it->is_string() || it->get_ref<const std::string&>().size() > kMaxRequestIdBytes) {
        ok = false;
        return std::nullopt;
    }
    return it->get<std::string>();
}

Json base(std::string_view type) {
    Json j;
    j["protocol_version"] = kProtocolVersion;
    j["type"] = type;
    return j;
}

}  // namespace

codec::Parsed<ClientMessage> parse_client_message(std::string_view json) {
    bool repeated = false;
    Json obj = codec::parse_json(json, repeated);
    if (obj.is_discarded()) {
        return bad("malformed JSON");
    }
    if (repeated) {
        return bad("repeated field");
    }
    if (!obj.is_object()) {
        return bad("message must be a JSON object");
    }
    const auto version = obj.find("protocol_version");
    if (version == obj.end() || !version->is_number_integer() ||
        version->get<std::int64_t>() != kProtocolVersion) {
        return bad("unsupported or missing protocol_version (expected 1)", obj);
    }
    const auto type_it = obj.find("type");
    if (type_it == obj.end() || !type_it->is_string()) {
        return bad("missing or non-string \"type\"", obj);
    }
    const std::string& type = type_it->get_ref<const std::string&>();
    if (type == "new_limit" || type == "cancel") {
        auto cmd = codec::parse_command(obj, {"protocol_version"});
        if (auto* err = std::get_if<codec::ParseError>(&cmd)) {
            return std::move(*err);
        }
        return ClientMessage{std::get<Command>(std::move(cmd))};
    }
    bool ok = true;
    if (type == "snapshot_request") {
        for (const auto& [key, value] : obj.items()) {
            (void)value;
            if (key != "protocol_version" && key != "type" && key != "request_id" &&
                key != "symbol" && key != "depth") {
                return bad("unknown field \"" + key + "\"", obj);
            }
        }
        SnapshotRequest req;
        req.request_id = optional_request_id(obj, ok).value_or("");
        if (!ok) {
            return bad("\"request_id\" must be a string of at most 64 bytes", obj);
        }
        if (const auto it = obj.find("symbol"); it != obj.end()) {
            if (!it->is_string()) {
                codec::ParseError e = bad("\"symbol\" must be a string", obj);
                e.code = ErrorCode::InvalidSymbol;
                return e;
            }
            req.symbol = it->get<std::string>();
        }
        if (const auto it = obj.find("depth"); it != obj.end()) {
            if (!it->is_number_integer() || it->get<std::int64_t>() < 0 ||
                it->get<std::int64_t>() > static_cast<std::int64_t>(codec::kMaxSnapshotDepth)) {
                return bad("\"depth\" must be an integer in [0, 1000]", obj);
            }
            req.depth = static_cast<std::size_t>(it->get<std::int64_t>());
        }
        return ClientMessage{std::move(req)};
    }
    if (type == "subscribe") {
        for (const auto& [key, value] : obj.items()) {
            (void)value;
            if (key != "protocol_version" && key != "type" && key != "request_id") {
                return bad("unknown field \"" + key + "\"", obj);
            }
        }
        SubscribeRequest req;
        req.request_id = optional_request_id(obj, ok).value_or("");
        if (!ok) {
            return bad("\"request_id\" must be a string of at most 64 bytes", obj);
        }
        return ClientMessage{std::move(req)};
    }
    return bad("unknown message type \"" + type + "\"", obj);
}

std::string result_message(const CommandResult& result, const ExchangeStateMachine& state) {
    if (!result.accepted) {
        return error_message(result.error, result.message, result.request_id,
                             to_string(result.type));
    }
    Json body = codec::to_json(result, state);
    Json j = base("ack");
    j["request_id"] = body["request_id"];
    j["command_type"] = to_string(result.type);
    for (auto it = body.begin(); it != body.end(); ++it) {
        if (it.key() != "request_id" && it.key() != "status" && it.key() != "type") {
            j[it.key()] = it.value();
        }
    }
    return j.dump();
}

std::string error_message(ErrorCode code, std::string_view message, std::string_view request_id,
                          std::string_view command_type) {
    Json j = base("error");
    j["request_id"] = request_id;
    if (!command_type.empty()) {
        j["command_type"] = command_type;
    }
    j["code"] = to_string(code);
    j["message"] = message;
    return j.dump();
}

std::string error_message(const codec::ParseError& error) {
    return error_message(error.code, error.message, error.request_id, error.type);
}

std::string snapshot_message(const ExchangeStateMachine& state, std::string_view request_id,
                             std::optional<SymbolId> symbol, std::size_t depth,
                             EventSeq event_watermark, bool subscribed) {
    Json j = base("snapshot_response");
    j["request_id"] = request_id;
    j["event_watermark"] = event_watermark;
    j["command_sequence"] = state.last_command_sequence();
    j["execution_sequence"] = state.last_execution_sequence();
    j["depth"] = depth;
    j["subscribed"] = subscribed;
    Json books = Json::array();
    for (SymbolId s = 0; s < state.symbols().size(); ++s) {
        if (symbol && *symbol != s) {
            continue;
        }
        const BookSnapshot snap = state.engine().snapshot(s, depth);
        books.push_back(Json{{"symbol", state.symbol_name(s)},
                             {"bids", codec::levels_to_json(snap.bids)},
                             {"asks", codec::levels_to_json(snap.asks)}});
    }
    j["books"] = std::move(books);
    return j.dump();
}

std::string book_update_message(const ExchangeStateMachine& state, const CommandResult& r,
                                EventSeq event_sequence) {
    const OrderBook& book = state.engine().book(r.symbol);
    // Price levels whose aggregate may have changed, in a deterministic order.
    std::vector<std::pair<Side, PriceCents>> touched;
    if (r.type == CommandType::NewLimit) {
        for (const auto& e : r.executions) {
            touched.emplace_back(opposite(r.side), e.price);
        }
        if (r.remaining > 0) {
            touched.emplace_back(r.side, r.price);
        }
    } else {
        touched.emplace_back(r.side, r.price);
    }
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());

    Json j = base("book_update");
    j["event_sequence"] = event_sequence;
    j["command_sequence"] = r.command_sequence;
    j["command_type"] = to_string(r.type);
    j["symbol"] = state.symbol_name(r.symbol);
    j["order_id"] = format_order_id(r.order_id);
    j["side"] = to_string(r.side);
    j["price"] = r.price;
    Json execs = Json::array();
    for (const auto& e : r.executions) {
        execs.push_back(Json{{"execution_id", format_execution_id(e.execution_sequence)},
                             {"execution_sequence", e.execution_sequence},
                             {"incoming_order_id", format_order_id(e.incoming_order_id)},
                             {"resting_order_id", format_order_id(e.resting_order_id)},
                             {"incoming_side", to_string(e.incoming_side)},
                             {"price", e.price},
                             {"quantity", e.quantity}});
    }
    j["executions"] = std::move(execs);
    Json levels = Json::array();
    for (const auto& [side, price] : touched) {
        const LevelMap& map = book.levels(side);
        const auto it = map.find(price);
        const Quantity qty = it == map.end() ? 0 : it->second.aggregate;
        const auto count =
            it == map.end() ? 0U : static_cast<std::uint32_t>(it->second.orders.size());
        levels.push_back(Json{{"side", to_string(side)},
                              {"price", price},
                              {"quantity", qty},
                              {"order_count", count}});
    }
    j["levels"] = std::move(levels);
    for (const Side side : {Side::Buy, Side::Sell}) {
        const auto best = book.best_level(side);
        const char* key = side == Side::Buy ? "best_bid" : "best_ask";
        if (best == book.levels(side).end()) {
            j[key] = nullptr;
        } else {
            j[key] = Json{{"price", best->first}, {"quantity", best->second.aggregate}};
        }
    }
    return j.dump();
}

std::string resync_message(std::string_view reason, EventSeq last_event_queued) {
    Json j = base("resync_required");
    j["reason"] = reason;
    j["last_event_sequence"] = last_event_queued;
    return j.dump();
}

}  // namespace enginex::net
