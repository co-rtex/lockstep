#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "enginex/command.hpp"
#include "enginex/engine.hpp"
#include "enginex/types.hpp"

namespace enginex {

struct StateConfig {
    // Symbol catalog. Order matters: it defines SymbolId values and canonical output order.
    std::vector<std::string> symbols{"AAPL", "MSFT"};
    // Maximum simultaneously resting orders; new orders beyond it get LIMIT_EXCEEDED.
    std::size_t max_open_orders = 10'000'000;
};

// Everything remembered about one accepted command: its body (for idempotency comparison),
// identity, and enough outcome data to rebuild the original response.
struct CommandRecord {
    Command command;
    CommandSeq sequence = 0;
    SymbolId symbol = 0;
    ExecSeq first_execution = 0;  // first execution sequence produced (0 if none)
    std::uint32_t execution_count = 0;
    // new_limit: remaining after matching; cancel: quantity removed from the book.
    Quantity outcome_quantity = 0;
    // new_limit: current lifecycle status of the order (updated by later commands).
    OrderStatus status = OrderStatus::Open;
    // cancel: target order id.
    OrderId target = 0;
};

// Result of the read-only validation step.
struct Decision {
    enum class Kind : std::uint8_t { Accept, Duplicate, Reject };
    Kind kind = Kind::Reject;
    ErrorCode error = ErrorCode::None;
    std::string message;
    CommandSeq sequence = 0;                  // Accept: sequence that commit() will assign
    const CommandRecord* original = nullptr;  // Duplicate: the earlier accepted command
    SymbolId symbol = 0;                      // Accept new_limit
    OrderId target = 0;                       // Accept cancel

    [[nodiscard]] bool accepted() const noexcept { return kind == Kind::Accept; }
};

// The single authoritative owner of exchange state (spec §7.4–7.5). Validation is a pure
// function of the current state (`prepare`), and `commit` applies exactly the decision that
// `prepare` returned. Nothing else mutates the order books.
class ExchangeStateMachine {
public:
    explicit ExchangeStateMachine(StateConfig config = {});

    ExchangeStateMachine(const ExchangeStateMachine&) = delete;
    ExchangeStateMachine& operator=(const ExchangeStateMachine&) = delete;
    ExchangeStateMachine(ExchangeStateMachine&&) = default;
    // Not move-assignable: the engine's node pool must outlive the containers using it (D26).
    ExchangeStateMachine& operator=(ExchangeStateMachine&&) = delete;

    // Validates `cmd` against the current state without mutating anything.
    [[nodiscard]] Decision prepare(const Command& cmd) const;

    // Applies an Accept decision produced by prepare() on the *current* state. Precondition:
    // no other command was committed in between. Throws InvariantViolation on internal errors.
    void commit(const Command& cmd, const Decision& decision, CommandResult& out);

    // Fills `out` for a Duplicate or Reject decision (no mutation).
    void describe(const Command& cmd, const Decision& decision, CommandResult& out) const;

    // prepare + commit/describe. Reuses `out`'s buffers.
    void process(const Command& cmd, CommandResult& out);
    [[nodiscard]] CommandResult process(const Command& cmd);

    [[nodiscard]] CommandSeq last_command_sequence() const noexcept { return last_command_; }
    [[nodiscard]] ExecSeq last_execution_sequence() const noexcept {
        return static_cast<ExecSeq>(executions_.size());
    }

    [[nodiscard]] const StateConfig& config() const noexcept { return config_; }
    [[nodiscard]] const std::vector<std::string>& symbols() const noexcept {
        return config_.symbols;
    }
    [[nodiscard]] std::optional<SymbolId> find_symbol(std::string_view symbol) const noexcept;
    [[nodiscard]] const std::string& symbol_name(SymbolId id) const {
        return config_.symbols.at(id);
    }

    [[nodiscard]] const MatchingEngine& engine() const noexcept { return engine_; }
    [[nodiscard]] const std::vector<Execution>& executions() const noexcept { return executions_; }
    // The accepted command with sequence `seq` (1 <= seq <= last_command_sequence()).
    [[nodiscard]] const CommandRecord& command(CommandSeq seq) const {
        return records_.at(static_cast<std::size_t>(seq - 1));
    }
    [[nodiscard]] const CommandRecord* find_request(std::string_view request_id) const;

    void reserve(std::size_t expected_commands);

private:
    [[nodiscard]] Decision reject(ErrorCode code, std::string message) const;
    void fill_from_record(const CommandRecord& record, CommandResult& out) const;

    StateConfig config_;
    MatchingEngine engine_;
    CommandSeq last_command_ = 0;
    std::vector<Execution> executions_;  // execution i has sequence i + 1
    // Every accepted command, by sequence (record i has sequence i + 1). A deque keeps
    // references stable as it grows and allocates in blocks, not one node per command.
    std::deque<CommandRecord> records_;
    // request_id -> record: open addressing with linear probing over 8-byte slots. A slot
    // holds 32 bits of the id's hash and the record index + 1 (0 = empty); the probe start is
    // derived from those bits, so growth never re-hashes strings. A lookup compares the full
    // request_id only on a fingerprint match, so a new id usually costs one cache miss. The
    // node-based unordered_map it replaces cost ~75 ns per miss at 3 M entries (D31).
    struct IdSlot {
        std::uint32_t fingerprint = 0;
        std::uint32_t index_plus_one = 0;
    };
    static constexpr std::size_t kMaxRecords = UINT32_MAX - 1;
    [[nodiscard]] static std::uint32_t fingerprint(std::string_view id) noexcept;
    void grow_index(std::size_t records);  // capacity for `records` entries (may throw)
    void place(std::uint32_t fp, std::uint32_t index_plus_one) noexcept;
    std::vector<IdSlot> id_index_;
    std::vector<Fill> scratch_fills_;
};

}  // namespace enginex
