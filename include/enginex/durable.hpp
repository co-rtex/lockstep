#pragma once

// DurableExchange: the R2 journaled state machine (spec §9).
//
// Pipeline for every accepted command (DECISIONS.md D8):
//   prepare (pure validation) → commit (apply in memory) → encode into the batch buffer
//   → flush(): write() the batch → synchronize (per SyncMode) → release responses.
// A response for an accepted command is valid to send only after flush() succeeded. Any
// write/sync/apply failure enters fail-stop: later requests get JOURNAL_IO_ERROR until a
// restart rebuilds state from the journal (the failed batch's outcome is unknown, see D9).
//
// Startup (open): scan the journal, replay every valid record through the same state machine
// (verifying each is accepted with exactly its logged sequence), handle an incomplete tail
// according to TailPolicy, then accept new commands.

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "enginex/journal.hpp"
#include "enginex/state.hpp"

namespace enginex {

enum class TailPolicy : std::uint8_t {
    Repair,  // preserve tail bytes in <journal>.tail-<offset>.bin, truncate, continue
    Refuse,  // throw JOURNAL_CORRUPT; leave the file untouched
};

struct DurableOptions {
    std::string journal_path;
    journal::SyncMode sync = journal::SyncMode::Full;
    TailPolicy tail_policy = TailPolicy::Repair;
    StateConfig state;
    journal::FaultPlan faults;
    bool create_if_missing = true;
};

struct RecoveryReport {
    bool created = false;
    std::uint64_t records_replayed = 0;
    std::uint64_t file_bytes = 0;
    std::uint64_t valid_bytes = 0;
    journal::TailState tail = journal::TailState::Clean;
    std::string tail_reason;
    bool tail_repaired = false;
    std::string preserved_tail_path;
    CommandSeq last_command_sequence = 0;
    std::uint64_t elapsed_ns = 0;  // open + scan + replay + repair
};

// Replays `bytes` (a complete journal image) into `state`, verifying that every record is
// accepted with exactly its logged sequence. Throws journal::JournalError(JOURNAL_CORRUPT) on
// integrity failures or replay divergence. Used by recovery and by `enginex replay --verify`.
journal::ScanResult replay_into(std::string_view bytes, ExchangeStateMachine& state);

class DurableExchange {
public:
    // Opens or creates the journal, recovers state, and returns a ready exchange. Throws
    // journal::JournalError on corruption, refused tails, lock conflicts or I/O errors.
    static std::unique_ptr<DurableExchange> open(DurableOptions options,
                                                 RecoveryReport* report = nullptr);

    // Validates and applies one command, buffering its journal record. Results of accepted
    // commands must not be released until flush() returns ErrorCode::None.
    void stage(const Command& cmd, CommandResult& out);

    // Writes and synchronizes all staged records. Returns ErrorCode::None on success or
    // JOURNAL_IO_ERROR after entering fail-stop.
    [[nodiscard]] ErrorCode flush();

    // stage() + flush(); on flush failure `out` becomes a JOURNAL_IO_ERROR result.
    void process(const Command& cmd, CommandResult& out);

    // Rewrites `out` as the JOURNAL_IO_ERROR "outcome unknown" response (used for a failed
    // batch's pending results and for every request after fail-stop).
    void make_io_error(CommandResult& out) const;

    // Safe from any thread (Server::journal_failed() reads it while the sequencer runs).
    [[nodiscard]] bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
    [[nodiscard]] const std::string& failure_reason() const noexcept { return failure_; }
    [[nodiscard]] const ExchangeStateMachine& state() const noexcept { return state_; }
    [[nodiscard]] std::size_t staged_records() const noexcept { return staged_count_; }
    [[nodiscard]] std::uint64_t durable_bytes() const noexcept { return durable_bytes_; }
    [[nodiscard]] journal::SyncMode sync_mode() const noexcept { return file_->sync_mode(); }
    [[nodiscard]] const std::string& path() const noexcept { return file_->path(); }

private:
    DurableExchange(std::unique_ptr<journal::JournalFile> file, StateConfig config);
    void enter_fail_stop(std::string reason);
    void maybe_crash(journal::CrashPoint point, CommandSeq first, CommandSeq last) const;

    std::unique_ptr<journal::JournalFile> file_;
    ExchangeStateMachine state_;
    std::string batch_;
    std::size_t staged_count_ = 0;
    CommandSeq batch_first_ = 0;
    CommandSeq batch_last_ = 0;
    std::uint64_t durable_bytes_ = 0;
    // Written once by the sequencer thread (after failure_, release); read anywhere (acquire).
    std::atomic<bool> failed_{false};
    std::string failure_;
};

}  // namespace enginex
