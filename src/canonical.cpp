#include "enginex/canonical.hpp"

#include <array>
#include <charconv>

#include "enginex/sha256.hpp"

namespace enginex {

void CanonicalWriter::separator() {
    if (need_comma_) {
        out_.push_back(',');
    }
    need_comma_ = true;
}

void CanonicalWriter::begin_object() {
    separator();
    out_.push_back('{');
    need_comma_ = false;
}

void CanonicalWriter::end_object() {
    out_.push_back('}');
    need_comma_ = true;
    if (sink_ != nullptr && out_.size() >= 64 * 1024) {
        flush();
    }
}

void CanonicalWriter::flush() {
    if (sink_ != nullptr) {
        sink_->update(out_);
        out_.clear();
    }
}

void CanonicalWriter::begin_array() {
    separator();
    out_.push_back('[');
    need_comma_ = false;
}

void CanonicalWriter::end_array() {
    out_.push_back(']');
    need_comma_ = true;
}

void CanonicalWriter::key(std::string_view name) {
    value(name);
    out_.push_back(':');
    need_comma_ = false;
}

void CanonicalWriter::value(std::string_view text) {
    separator();
    static constexpr char kHex[] = "0123456789abcdef";
    out_.push_back('"');
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':
                out_.append("\\\"");
                break;
            case '\\':
                out_.append("\\\\");
                break;
            case '\n':
                out_.append("\\n");
                break;
            case '\r':
                out_.append("\\r");
                break;
            case '\t':
                out_.append("\\t");
                break;
            default:
                if (c < 0x20U) {
                    out_.append("\\u00");
                    out_.push_back(kHex[c >> 4U]);
                    out_.push_back(kHex[c & 0x0FU]);
                } else {
                    out_.push_back(ch);
                }
        }
    }
    out_.push_back('"');
}

void CanonicalWriter::value(std::int64_t number) {
    separator();
    std::array<char, 24> buf{};
    auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), number);
    (void)ec;
    out_.append(buf.data(), end);
}

void CanonicalWriter::value(std::uint64_t number) {
    separator();
    std::array<char, 24> buf{};
    auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), number);
    (void)ec;
    out_.append(buf.data(), end);
}

void CanonicalWriter::value(bool flag) {
    separator();
    out_.append(flag ? "true" : "false");
}

namespace {

void write_level(CanonicalWriter& w, const PriceLevel& level) {
    w.begin_object();
    w.key("price");
    w.value(level.price);
    w.key("aggregate");
    w.value(level.aggregate);
    w.key("orders");
    w.begin_array();
    for (const RestingOrder& order : level.orders) {
        w.begin_object();
        w.key("order_id");
        w.value(format_order_id(order.id));
        w.key("original");
        w.value(order.original);
        w.key("remaining");
        w.value(order.remaining);
        w.end_object();
    }
    w.end_array();
    w.end_object();
}

}  // namespace

namespace {

void write_canonical(CanonicalWriter& w, const ExchangeStateMachine& state) {
    w.begin_object();
    w.key("format");
    w.value("enginex.canonical.v1");
    w.key("symbols");
    w.begin_array();
    for (const auto& sym : state.symbols()) {
        w.value(sym);
    }
    w.end_array();
    w.key("last_command_sequence");
    w.value(state.last_command_sequence());
    w.key("last_execution_sequence");
    w.value(state.last_execution_sequence());

    w.key("books");
    w.begin_array();
    for (SymbolId s = 0; s < state.symbols().size(); ++s) {
        const OrderBook& book = state.engine().book(s);
        w.begin_object();
        w.key("symbol");
        w.value(state.symbol_name(s));
        for (const Side side : {Side::Buy, Side::Sell}) {
            w.key(side == Side::Buy ? "bids" : "asks");
            w.begin_array();
            book.for_each_level(side, [&w](const PriceLevel& level) {
                write_level(w, level);
                return true;
            });
            w.end_array();
        }
        w.end_object();
    }
    w.end_array();

    w.key("commands");
    w.begin_array();
    for (CommandSeq seq = 1; seq <= state.last_command_sequence(); ++seq) {
        const CommandRecord& rec = state.command(seq);
        w.begin_object();
        w.key("command_sequence");
        w.value(rec.sequence);
        w.key("request_id");
        w.value(request_id_of(rec.command));
        w.key("type");
        w.value(to_string(type_of(rec.command)));
        w.key("symbol");
        w.value(state.symbol_name(rec.symbol));
        if (const auto* order = std::get_if<NewLimitOrder>(&rec.command)) {
            w.key("order_id");
            w.value(format_order_id(rec.sequence));
            w.key("side");
            w.value(to_string(order->side));
            w.key("price");
            w.value(order->price);
            w.key("quantity");
            w.value(order->quantity);
            w.key("remaining_after_command");
            w.value(rec.outcome_quantity);
            w.key("status");
            w.value(to_string(rec.status));
            w.key("first_execution_sequence");
            w.value(rec.first_execution);
            w.key("execution_count");
            w.value(rec.execution_count);
        } else {
            w.key("order_id");
            w.value(format_order_id(rec.target));
            w.key("cancelled_quantity");
            w.value(rec.outcome_quantity);
        }
        w.end_object();
    }
    w.end_array();

    w.key("executions");
    w.begin_array();
    for (const Execution& e : state.executions()) {
        w.begin_object();
        w.key("execution_id");
        w.value(format_execution_id(e.execution_sequence));
        w.key("execution_sequence");
        w.value(e.execution_sequence);
        w.key("command_sequence");
        w.value(e.command_sequence);
        w.key("symbol");
        w.value(state.symbol_name(e.symbol));
        w.key("incoming_order_id");
        w.value(format_order_id(e.incoming_order_id));
        w.key("resting_order_id");
        w.value(format_order_id(e.resting_order_id));
        w.key("incoming_side");
        w.value(to_string(e.incoming_side));
        w.key("price");
        w.value(e.price);
        w.key("quantity");
        w.value(e.quantity);
        w.end_object();
    }
    w.end_array();

    w.end_object();
}

}  // namespace

std::string canonical_state(const ExchangeStateMachine& state) {
    CanonicalWriter w;
    write_canonical(w, state);
    return w.take();
}

std::string state_hash(const ExchangeStateMachine& state) {
    Sha256 hasher;
    CanonicalWriter w(&hasher);
    write_canonical(w, state);
    w.flush();
    return Sha256::hex(hasher.finish());
}

}  // namespace enginex
