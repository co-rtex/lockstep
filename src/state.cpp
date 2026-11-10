#include "enginex/state.hpp"

#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace enginex {

namespace {

OrderStatus status_for(Quantity original, Quantity remaining) noexcept {
    if (remaining == 0) {
        return OrderStatus::Filled;
    }
    return remaining == original ? OrderStatus::Open : OrderStatus::PartiallyFilled;
}

std::optional<std::string> request_id_problem(const std::string& id) {
    if (id.empty()) {
        return "request_id must be nonempty";
    }
    if (id.size() > kMaxRequestIdBytes) {
        return "request_id exceeds 64 bytes";
    }
    if (!is_valid_utf8(id)) {
        return "request_id is not valid UTF-8";
    }
    return std::nullopt;
}

}  // namespace

ExchangeStateMachine::ExchangeStateMachine(StateConfig config)
    : config_(std::move(config)), engine_(config_.symbols.size()) {
    std::unordered_set<std::string> unique;
    for (const auto& sym : config_.symbols) {
        if (sym.empty() || !unique.insert(sym).second) {
            throw std::invalid_argument("symbol catalog entries must be nonempty and unique");
        }
    }
    if (config_.symbols.size() > std::numeric_limits<SymbolId>::max()) {
        throw std::invalid_argument("too many symbols");
    }
    // Secret index key: placement only, never results (see header). Fails closed: if the OS
    // entropy source is unavailable, std::random_device throws and construction fails loudly
    // rather than running with a guessable key.
    std::random_device rd;
    auto word = [&rd] { return (static_cast<std::uint64_t>(rd()) << 32U) | rd(); };
    id_key_ = SipKey{word(), word()};
}

std::optional<SymbolId> ExchangeStateMachine::find_symbol(std::string_view symbol) const noexcept {
    for (std::size_t i = 0; i < config_.symbols.size(); ++i) {
        if (config_.symbols[i] == symbol) {
            return static_cast<SymbolId>(i);
        }
    }
    return std::nullopt;
}

std::uint32_t ExchangeStateMachine::fingerprint(std::string_view id) const noexcept {
    const std::uint64_t h = siphash24(id_key_, id);
    return static_cast<std::uint32_t>(h ^ (h >> 32U));
}

const CommandRecord* ExchangeStateMachine::find_request(std::string_view request_id) const {
    return find_request(request_id, fingerprint(request_id));
}

const CommandRecord* ExchangeStateMachine::find_request(std::string_view request_id,
                                                        std::uint32_t fp) const noexcept {
    if (id_index_.empty()) {
        return nullptr;
    }
    const std::size_t mask = id_index_.size() - 1;
    for (std::size_t i = fp & mask;; i = (i + 1) & mask) {
        const IdSlot& slot = id_index_[i];
        if (slot.index_plus_one == 0) {
            return nullptr;
        }
        if (slot.fingerprint == fp) {
            const CommandRecord& r = records_[slot.index_plus_one - 1];
            if (request_id_of(r.command) == request_id) {
                return &r;
            }
        }
    }
}

void ExchangeStateMachine::grow_index(std::size_t records) {
    // Keep the load factor at or below 1/2, so a miss (a new id) is a short probe.
    std::size_t cap = id_index_.empty() ? 16 : id_index_.size();
    while (cap < 2 * records) cap *= 2;
    if (cap == id_index_.size()) {
        return;
    }
    std::vector<IdSlot> grown(cap);  // may throw: nothing has changed yet
    for (const IdSlot& s : id_index_) {
        if (s.index_plus_one != 0) {
            std::size_t j = s.fingerprint & (cap - 1);
            while (grown[j].index_plus_one != 0) j = (j + 1) & (cap - 1);
            grown[j] = s;
        }
    }
    id_index_.swap(grown);
}

void ExchangeStateMachine::place(std::uint32_t fp, std::uint32_t index_plus_one) noexcept {
    const std::size_t mask = id_index_.size() - 1;
    std::size_t i = fp & mask;
    while (id_index_[i].index_plus_one != 0) i = (i + 1) & mask;
    id_index_[i] = IdSlot{fp, index_plus_one};
}

void ExchangeStateMachine::reserve(std::size_t expected_commands) {
    grow_index(expected_commands);
    executions_.reserve(expected_commands);
    engine_.reserve(expected_commands);
}

Decision ExchangeStateMachine::reject(ErrorCode code, std::string message) const {
    Decision d;
    d.kind = Decision::Kind::Reject;
    d.error = code;
    d.message = std::move(message);
    return d;
}

Decision ExchangeStateMachine::prepare(const Command& cmd) const {
    const std::string& rid = request_id_of(cmd);
    if (auto problem = request_id_problem(rid)) {
        return reject(ErrorCode::InvalidRequest, std::move(*problem));
    }

    // Idempotency: an accepted request id either replays its stored result (identical body) or
    // is a conflict. This check precedes all other validation.
    if (const CommandRecord* original = find_request(rid)) {
        if (original->command == cmd) {
            Decision d;
            d.kind = Decision::Kind::Duplicate;
            d.original = original;
            return d;
        }
        return reject(ErrorCode::DuplicateRequestIdConflict,
                      "request_id was already accepted with a different command");
    }

    if (last_command_ == std::numeric_limits<CommandSeq>::max() ||
        records_.size() >= kMaxRecords) {
        return reject(ErrorCode::LimitExceeded, "command sequence exhausted");
    }

    Decision d;
    d.kind = Decision::Kind::Accept;
    d.sequence = last_command_ + 1;

    if (const auto* order = std::get_if<NewLimitOrder>(&cmd)) {
        const auto symbol = find_symbol(order->symbol);
        if (!symbol) {
            return reject(ErrorCode::InvalidSymbol, "unknown symbol");
        }
        if (!is_valid_side(order->side)) {
            return reject(ErrorCode::InvalidSide, "side must be BUY or SELL");
        }
        if (order->price < kMinPrice || order->price > kMaxPrice) {
            return reject(ErrorCode::InvalidPrice, "price must be an integer in [1, 100000000]");
        }
        if (order->quantity < kMinQuantity || order->quantity > kMaxQuantity) {
            return reject(ErrorCode::InvalidQuantity,
                          "quantity must be an integer in [1, 1000000]");
        }
        if (engine_.open_order_count() >= config_.max_open_orders) {
            return reject(ErrorCode::LimitExceeded, "open order limit reached");
        }
        d.symbol = *symbol;
        return d;
    }

    const auto& cancel = std::get<CancelOrder>(cmd);
    const auto target = parse_order_id(cancel.order_id);
    if (!target || *target > last_command_) {
        return reject(ErrorCode::UnknownOrder, "unknown order_id");
    }
    const CommandRecord& record = records_[static_cast<std::size_t>(*target - 1)];
    if (!std::holds_alternative<NewLimitOrder>(record.command)) {
        return reject(ErrorCode::UnknownOrder, "unknown order_id");
    }
    if (!engine_.is_resting(*target)) {
        return reject(ErrorCode::OrderNotOpen, "order is already filled or cancelled");
    }
    d.symbol = record.symbol;
    d.target = *target;
    return d;
}

void ExchangeStateMachine::commit(const Command& cmd, const Decision& decision,
                                  CommandResult& out) {
    if (!decision.accepted() || decision.sequence != last_command_ + 1) {
        throw InvariantViolation("commit called without a current Accept decision");
    }
    out.reset();
    const CommandSeq seq = decision.sequence;

    const std::string& rid = request_id_of(cmd);
    const std::uint32_t fp = fingerprint(rid);  // hashed once for the check and the insert
    if (find_request(rid, fp) != nullptr) {
        throw InvariantViolation("request id already recorded");
    }
    if (records_.size() >= kMaxRecords) {
        throw InvariantViolation("command history capacity exceeded");
    }
    // Allocate first (index growth, then the record with its copy of the command), and only
    // then index it, which cannot fail: an allocation failure here never leaves a record
    // unindexed. (As before this index existed, a failure later in this function, inside the
    // engine or while recording executions, is not rolled back; see the commit contract.)
    grow_index(records_.size() + 1);
    CommandRecord& record = records_.emplace_back(CommandRecord{cmd, seq, decision.symbol});
    place(fp, static_cast<std::uint32_t>(records_.size()));
    last_command_ = seq;

    if (const auto* order = std::get_if<NewLimitOrder>(&cmd)) {
        scratch_fills_.clear();
        const AcceptedOrder accepted{seq, decision.symbol, order->side, order->price,
                                     order->quantity};
        const MatchOutcome outcome = engine_.submit(accepted, scratch_fills_);

        record.first_execution = scratch_fills_.empty() ? 0 : executions_.size() + 1;
        record.execution_count = static_cast<std::uint32_t>(scratch_fills_.size());
        record.outcome_quantity = outcome.remaining;
        record.status = status_for(order->quantity, outcome.remaining);

        for (const Fill& fill : scratch_fills_) {
            executions_.push_back(Execution{executions_.size() + 1, seq, seq, fill.resting_order_id,
                                            decision.symbol, order->side, fill.price,
                                            fill.quantity});
            CommandRecord& resting =
                records_[static_cast<std::size_t>(fill.resting_order_id - 1)];
            resting.status =
                fill.resting_remaining == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
        }
    } else {
        const auto cancelled = engine_.cancel(decision.target);
        if (!cancelled) {
            throw InvariantViolation("accepted cancel target is not resting");
        }
        record.target = decision.target;
        record.outcome_quantity = cancelled->cancelled_quantity;
        records_[static_cast<std::size_t>(decision.target - 1)].status = OrderStatus::Cancelled;
    }

    fill_from_record(record, out);
}

void ExchangeStateMachine::fill_from_record(const CommandRecord& record, CommandResult& out) const {
    out.request_id = request_id_of(record.command);
    out.type = type_of(record.command);
    out.accepted = true;
    out.command_sequence = record.sequence;
    out.symbol = record.symbol;
    out.state_watermark = last_command_;
    if (const auto* order = std::get_if<NewLimitOrder>(&record.command)) {
        out.order_id = record.sequence;
        out.side = order->side;
        out.price = order->price;
        out.quantity = order->quantity;
        out.remaining = record.outcome_quantity;
        out.order_status = status_for(order->quantity, record.outcome_quantity);
        if (record.execution_count > 0) {
            const auto first = static_cast<std::ptrdiff_t>(record.first_execution - 1);
            out.executions.assign(executions_.begin() + first,
                                  executions_.begin() + first + record.execution_count);
        }
    } else {
        const CommandRecord& target = records_[static_cast<std::size_t>(record.target - 1)];
        const auto& target_order = std::get<NewLimitOrder>(target.command);
        out.order_id = record.target;
        out.side = target_order.side;
        out.price = target_order.price;
        out.quantity = target_order.quantity;
        out.remaining = 0;
        out.cancelled_quantity = record.outcome_quantity;
        out.order_status = OrderStatus::Cancelled;
    }
}

void ExchangeStateMachine::describe(const Command& cmd, const Decision& decision,
                                    CommandResult& out) const {
    out.reset();
    if (decision.kind == Decision::Kind::Duplicate) {
        fill_from_record(*decision.original, out);
        out.duplicate = true;
        return;
    }
    if (decision.kind == Decision::Kind::Accept) {
        throw InvariantViolation("describe called with an Accept decision");
    }
    out.request_id = request_id_of(cmd);
    out.type = type_of(cmd);
    out.accepted = false;
    out.error = decision.error;
    out.message = decision.message;
    out.state_watermark = last_command_;
}

void ExchangeStateMachine::process(const Command& cmd, CommandResult& out) {
    const Decision decision = prepare(cmd);
    if (decision.accepted()) {
        commit(cmd, decision, out);
    } else {
        describe(cmd, decision, out);
    }
}

CommandResult ExchangeStateMachine::process(const Command& cmd) {
    CommandResult out;
    process(cmd, out);
    return out;
}

}  // namespace enginex
