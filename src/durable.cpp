#include "enginex/durable.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>

#include "enginex/metrics.hpp"

namespace enginex {

using journal::JournalError;

journal::ScanResult replay_into(std::string_view bytes, ExchangeStateMachine& state) {
    CommandResult scratch;
    return journal::scan_bytes(bytes, [&](const journal::Record& rec, std::uint64_t offset) {
        const Decision d = state.prepare(rec.command);
        if (!d.accepted() || d.sequence != rec.sequence) {
            std::string why = d.kind == Decision::Kind::Duplicate
                                  ? "duplicate request id"
                                  : std::string(to_string(d.error)) + " " + d.message;
            throw JournalError(ErrorCode::JournalCorrupt,
                               "replay divergence for sequence " + std::to_string(rec.sequence) +
                                   " at offset " + std::to_string(offset) + ": " + why +
                                   " (was the journal written with a different symbol catalog?)",
                               offset);
        }
        state.commit(rec.command, d, scratch);
    });
}

namespace {

// Writes the bytes after the last valid record to a sidecar file before truncation so the
// original data is preserved for inspection. Returns the sidecar path.
std::string preserve_tail(const std::string& journal_path, std::string_view tail,
                          std::uint64_t offset) {
    std::string base = journal_path + ".tail-" + std::to_string(offset);
    std::string candidate = base + ".bin";
    for (int n = 1; std::filesystem::exists(candidate); ++n) {
        candidate = base + "." + std::to_string(n) + ".bin";
    }
    const int fd = ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        throw JournalError(ErrorCode::JournalIoError, "cannot preserve journal tail to " +
                                                          candidate + ": " + std::strerror(errno));
    }
    std::size_t done = 0;
    int err = 0;
    while (done < tail.size()) {
        const ssize_t n = ::write(fd, tail.data() + done, tail.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            err = errno;
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    if (err == 0 && ::fsync(fd) != 0) {
        err = errno;
    }
    ::close(fd);
    if (err == 0) {
        err = journal::sync_parent_directory(candidate);
    }
    if (err != 0) {
        throw JournalError(ErrorCode::JournalIoError,
                           "cannot preserve journal tail: " + std::string(std::strerror(err)));
    }
    return candidate;
}

}  // namespace

DurableExchange::DurableExchange(std::unique_ptr<journal::JournalFile> file, StateConfig config)
    : file_(std::move(file)), state_(std::move(config)) {}

std::unique_ptr<DurableExchange> DurableExchange::open(DurableOptions options,
                                                       RecoveryReport* report) {
    const auto start = metrics::Clock::now();
    RecoveryReport local;
    RecoveryReport& rep = report != nullptr ? *report : local;
    rep = RecoveryReport{};

    auto file = journal::JournalFile::open(options.journal_path, options.create_if_missing,
                                           options.sync, options.faults);
    rep.created = file->created();
    std::unique_ptr<DurableExchange> ex(
        new DurableExchange(std::move(file), std::move(options.state)));

    const std::string bytes = ex->file_->read_all();
    const journal::ScanResult scan = replay_into(bytes, ex->state_);
    rep.records_replayed = scan.records;
    rep.file_bytes = scan.file_bytes;
    rep.valid_bytes = scan.valid_bytes;
    rep.tail = scan.tail;
    rep.tail_reason = scan.tail_reason;

    if (scan.tail == journal::TailState::Incomplete) {
        if (options.tail_policy == TailPolicy::Refuse) {
            throw JournalError(ErrorCode::JournalCorrupt,
                               "journal has an incomplete tail (" + scan.tail_reason +
                                   "); restart with tail repair to truncate it",
                               scan.valid_bytes);
        }
        if (scan.valid_bytes < bytes.size()) {
            rep.preserved_tail_path =
                preserve_tail(ex->file_->path(), std::string_view(bytes).substr(scan.valid_bytes),
                              scan.valid_bytes);
        }
        if (const int err = ex->file_->truncate_and_sync(scan.valid_bytes); err != 0) {
            throw JournalError(ErrorCode::JournalIoError,
                               "cannot truncate journal tail: " + std::string(std::strerror(err)));
        }
        if (!scan.header_complete) {
            const std::string header = journal::encode_header();
            if (int err = ex->file_->write_all(header);
                err != 0 || (err = ex->file_->sync()) != 0) {
                throw JournalError(ErrorCode::JournalIoError, "cannot rewrite journal header: " +
                                                                  std::string(std::strerror(err)));
            }
            ex->durable_bytes_ = journal::kHeaderSize;
        } else {
            ex->durable_bytes_ = scan.valid_bytes;
        }
        rep.tail_repaired = true;
    } else {
        ex->durable_bytes_ = scan.valid_bytes;
    }
    rep.last_command_sequence = ex->state_.last_command_sequence();
    rep.elapsed_ns = metrics::elapsed_ns(start, metrics::Clock::now());
    return ex;
}

void DurableExchange::enter_fail_stop(std::string reason) {
    failed_ = true;
    failure_ = std::move(reason);
    batch_.clear();
    staged_count_ = 0;
}

void DurableExchange::maybe_crash(journal::CrashPoint point, CommandSeq first,
                                  CommandSeq last) const {
    const auto& f = file_->faults();
    if (f.crash_point == point && f.crash_sequence != 0 && first != 0 &&
        f.crash_sequence >= first && f.crash_sequence <= last) {
        ::_exit(journal::kCrashExitCode);
    }
}

void DurableExchange::make_io_error(CommandResult& out) const {
    const std::string rid = out.request_id;
    const CommandType type = out.type;
    out.reset();
    out.request_id = rid;
    out.type = type;
    out.accepted = false;
    out.error = ErrorCode::JournalIoError;
    out.message = "journal failure; outcome unknown, retry with the same request_id after restart";
    out.state_watermark = 0;
}

void DurableExchange::stage(const Command& cmd, CommandResult& out) {
    if (failed_) {
        out.reset();
        out.request_id = request_id_of(cmd);
        out.type = type_of(cmd);
        make_io_error(out);
        return;
    }
    const Decision decision = state_.prepare(cmd);
    if (!decision.accepted()) {
        state_.describe(cmd, decision, out);
        return;
    }
    try {
        state_.commit(cmd, decision, out);
        maybe_crash(journal::CrashPoint::AfterApply, decision.sequence, decision.sequence);
        journal::append_record(batch_, decision.sequence, cmd);
    } catch (const std::exception& e) {
        enter_fail_stop(std::string("internal error while applying command: ") + e.what());
        out.reset();
        out.request_id = request_id_of(cmd);
        out.type = type_of(cmd);
        make_io_error(out);
        out.error = ErrorCode::InternalError;
        return;
    }
    if (staged_count_ == 0) {
        batch_first_ = decision.sequence;
    }
    batch_last_ = decision.sequence;
    ++staged_count_;
}

ErrorCode DurableExchange::flush() {
    if (failed_) {
        return ErrorCode::JournalIoError;
    }
    if (staged_count_ == 0) {
        return ErrorCode::None;
    }
    if (const int err = file_->write_all(batch_); err != 0) {
        enter_fail_stop(std::string("write failed: ") + std::strerror(err));
        return ErrorCode::JournalIoError;
    }
    maybe_crash(journal::CrashPoint::AfterWrite, batch_first_, batch_last_);
    if (const int err = file_->sync(); err != 0) {
        enter_fail_stop(std::string("sync failed: ") + std::strerror(err));
        return ErrorCode::JournalIoError;
    }
    maybe_crash(journal::CrashPoint::AfterSync, batch_first_, batch_last_);
    durable_bytes_ += batch_.size();
    batch_.clear();
    staged_count_ = 0;
    return ErrorCode::None;
}

void DurableExchange::process(const Command& cmd, CommandResult& out) {
    stage(cmd, out);
    if (flush() != ErrorCode::None && out.accepted) {
        make_io_error(out);
    }
}

}  // namespace enginex
