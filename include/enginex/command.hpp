#pragma once

#include <string>
#include <variant>
#include <vector>

#include "enginex/types.hpp"

namespace enginex {

// Mutating client commands (spec §6). Values arrive from codecs unvalidated; the state machine
// performs all validation.
struct NewLimitOrder {
    std::string request_id;  // caller-supplied idempotency key
    std::string symbol;
    Side side = Side::Buy;
    PriceCents price = 0;
    Quantity quantity = 0;

    friend bool operator==(const NewLimitOrder&, const NewLimitOrder&) = default;
};

struct CancelOrder {
    std::string request_id;
    std::string order_id;  // opaque id assigned by a prior accepted NewLimitOrder

    friend bool operator==(const CancelOrder&, const CancelOrder&) = default;
};

using Command = std::variant<NewLimitOrder, CancelOrder>;

[[nodiscard]] inline const std::string& request_id_of(const Command& cmd) noexcept {
    return std::visit([](const auto& c) -> const std::string& { return c.request_id; }, cmd);
}

[[nodiscard]] inline CommandType type_of(const Command& cmd) noexcept {
    return std::holds_alternative<NewLimitOrder>(cmd) ? CommandType::NewLimit : CommandType::Cancel;
}

// A committed execution. IDs are numeric; codecs render "exe-<execution_sequence>".
struct Execution {
    ExecSeq execution_sequence = 0;
    CommandSeq command_sequence = 0;
    OrderId incoming_order_id = 0;
    OrderId resting_order_id = 0;
    SymbolId symbol = 0;
    Side incoming_side = Side::Buy;
    PriceCents price = 0;
    Quantity quantity = 0;

    friend bool operator==(const Execution&, const Execution&) = default;
};

// Response to one mutating command.
struct CommandResult {
    std::string request_id;
    CommandType type = CommandType::NewLimit;
    bool accepted = false;
    bool duplicate = false;  // true when this is the stored result of an earlier identical request
    ErrorCode error = ErrorCode::None;
    std::string message;  // human-readable; never parse

    // Populated when accepted (including duplicates).
    CommandSeq command_sequence = 0;
    OrderId order_id = 0;  // new_limit: the new order; cancel: the cancelled order
    SymbolId symbol = 0;
    Side side = Side::Buy;
    PriceCents price = 0;
    Quantity quantity = 0;                         // new_limit: original quantity
    Quantity remaining = 0;                        // new_limit: remaining after matching; cancel: 0
    Quantity cancelled_quantity = 0;               // cancel only
    OrderStatus order_status = OrderStatus::Open;  // status immediately after this command
    std::vector<Execution> executions;

    // Last accepted command sequence of the state that produced this response.
    CommandSeq state_watermark = 0;

    void reset() {
        request_id.clear();
        type = CommandType::NewLimit;
        accepted = false;
        duplicate = false;
        error = ErrorCode::None;
        message.clear();
        command_sequence = 0;
        order_id = 0;
        symbol = 0;
        side = Side::Buy;
        price = 0;
        quantity = 0;
        remaining = 0;
        cancelled_quantity = 0;
        order_status = OrderStatus::Open;
        executions.clear();
        state_watermark = 0;
    }
};

}  // namespace enginex
