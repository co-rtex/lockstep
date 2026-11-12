#pragma once

// EngineX journal format v1 (spec §9.2) and its low-level reader/writer.
//
//   file   := header record*
//   header := "EXJ1" | u16le format_version (=1) | u16le flags (=0)
//   record := u32le payload_length | u32le crc32c(payload) | payload
//
// payload v1 (all integers little-endian, written byte by byte, never as raw structs):
//   u8  payload_version (=1)
//   u8  command_type      1 = new_limit, 2 = cancel
//   u64 command_sequence
//   u8  request_id length (1..64), bytes
//   new_limit: u8 symbol length (1..32), bytes | u8 side (0 BUY, 1 SELL) | i64 price |
//              i64 quantity | u64 order_id (== command_sequence)
//   cancel:    u64 target order id
//
// Only accepted mutating commands are journaled. Execution ids, events and outcomes are not
// stored: replay re-derives them deterministically from the ordered commands.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "enginex/command.hpp"
#include "enginex/types.hpp"

namespace enginex::journal {

inline constexpr std::string_view kMagic = "EXJ1";
inline constexpr std::uint16_t kFormatVersion = 1;
inline constexpr std::size_t kHeaderSize = 8;
inline constexpr std::size_t kFrameHeaderSize = 8;
inline constexpr std::size_t kMaxPayloadSize = 4096;
inline constexpr std::uint8_t kPayloadVersion = 1;
inline constexpr std::size_t kMaxSymbolBytes = 32;

struct Record {
    CommandSeq sequence = 0;
    Command command;
};

// A failure that prevents safe use of the journal. `code` is JOURNAL_CORRUPT (integrity) or
// JOURNAL_IO_ERROR (the OS reported an error or the file is locked).
class JournalError : public std::runtime_error {
public:
    JournalError(ErrorCode code, const std::string& message, std::uint64_t offset = 0)
        : std::runtime_error(message), code_(code), offset_(offset) {}
    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] std::uint64_t offset() const noexcept { return offset_; }

private:
    ErrorCode code_;
    std::uint64_t offset_;
};

// ---- encoding ----------------------------------------------------------------------------

[[nodiscard]] std::string encode_header();
// Payload bytes for one accepted command. Throws std::invalid_argument if a field cannot be
// represented (e.g. request id > 64 bytes); validated commands never trigger this.
[[nodiscard]] std::string encode_payload(CommandSeq sequence, const Command& command);
// Appends a complete framed record (length, CRC32C, payload) to `out`.
void append_record(std::string& out, CommandSeq sequence, const Command& command);
// Decodes a payload; returns nullopt and sets `error` when it is malformed.
[[nodiscard]] std::optional<Record> decode_payload(std::string_view payload, std::string& error);

// ---- scanning ----------------------------------------------------------------------------

enum class TailState : std::uint8_t {
    Clean,       // file ends exactly at a record boundary
    Incomplete,  // partial header/frame/payload or an all-zero tail after the last record
};

struct ScanResult {
    std::uint64_t records = 0;
    std::uint64_t file_bytes = 0;
    std::uint64_t valid_bytes = 0;  // offset just past the last complete valid record
    bool header_complete = false;
    CommandSeq last_sequence = 0;
    TailState tail = TailState::Clean;
    std::string tail_reason;
};

using RecordCallback = std::function<void(const Record& record, std::uint64_t offset)>;

// Validates `bytes` as a journal and calls `on_record` for every valid record in order.
// Recoverable tails are reported in the result; integrity violations throw JournalError with
// JOURNAL_CORRUPT. Exceptions thrown by `on_record` propagate unchanged.
[[nodiscard]] ScanResult scan_bytes(std::string_view bytes, const RecordCallback& on_record);

// Reads a whole journal file and scans it (read-only, no lock).
[[nodiscard]] ScanResult scan_file(const std::string& path, const RecordCallback& on_record);

[[nodiscard]] std::string read_file(const std::string& path);

// ---- fault injection (tests and crash demos only) ------------------------------------------

enum class CrashPoint : std::uint8_t {
    None,
    AfterApply,  // command applied in memory, record not yet written
    AfterWrite,  // record written to the OS, not yet synchronized
    AfterSync,   // record durable, response not yet released
};

// Deterministic faults injected into the journal write path. Crashes call _exit() so no
// destructors, stdio flushes or further writes happen, like a killed process.
struct FaultPlan {
    CrashPoint crash_point = CrashPoint::None;
    CommandSeq crash_sequence = 0;      // crash when the batch containing this sequence gets there
    std::uint64_t fail_write_call = 0;  // 1-based write() call that fails (0 = never)
    std::size_t partial_write_bytes = 0;  // bytes really written by the failing call
    int write_errno = EIO;
    std::uint64_t fail_sync_call = 0;  // 1-based sync call that fails (0 = never)
    int sync_errno = EIO;

    // Parses e.g. "crash=after_sync@5", "fail_write=2,partial=7,errno=ENOSPC", "fail_sync=1".
    // Throws std::invalid_argument on malformed specs.
    [[nodiscard]] static FaultPlan parse(std::string_view spec);
    [[nodiscard]] bool any() const noexcept {
        return crash_point != CrashPoint::None || fail_write_call != 0 || fail_sync_call != 0;
    }
};

inline constexpr int kCrashExitCode = 86;

// ---- writer ------------------------------------------------------------------------------

enum class SyncMode : std::uint8_t {
    Full,   // Linux fdatasync; macOS fcntl(F_FULLFSYNC) with fsync fallback
    Fsync,  // POSIX fsync only
    None,   // no synchronization; acknowledged once write() returns
};

[[nodiscard]] std::string_view to_string(SyncMode mode) noexcept;
[[nodiscard]] std::optional<SyncMode> parse_sync_mode(std::string_view text) noexcept;

// Owns the journal file descriptor and its exclusive lock. Appends go to the end of the file.
class JournalFile {
public:
    // Opens (creating if `create`) with an exclusive non-blocking flock. Throws JournalError.
    // A newly created file gets a synced header and a synced parent directory entry.
    static std::unique_ptr<JournalFile> open(const std::string& path, bool create, SyncMode mode,
                                             FaultPlan faults = {});
    ~JournalFile();
    JournalFile(const JournalFile&) = delete;
    JournalFile& operator=(const JournalFile&) = delete;

    // Writes all bytes (looping over short writes and EINTR). Returns 0 or an errno value.
    [[nodiscard]] int write_all(std::string_view bytes);
    // Synchronizes according to the sync mode. Returns 0 or an errno value.
    [[nodiscard]] int sync();
    // Truncates the file to `size` bytes and synchronizes. Returns 0 or an errno value.
    [[nodiscard]] int truncate_and_sync(std::uint64_t size);

    [[nodiscard]] std::string read_all() const;
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] SyncMode sync_mode() const noexcept { return mode_; }
    [[nodiscard]] const FaultPlan& faults() const noexcept { return faults_; }
    [[nodiscard]] bool created() const noexcept { return created_; }

private:
    JournalFile(std::string path, int fd, SyncMode mode, FaultPlan faults, bool created);

    std::string path_;
    int fd_ = -1;
    SyncMode mode_;
    FaultPlan faults_;
    bool created_ = false;
    std::uint64_t write_calls_ = 0;
    std::uint64_t sync_calls_ = 0;
};

// fsyncs the directory containing `path` (makes a create/rename durable on Linux).
[[nodiscard]] int sync_parent_directory(const std::string& path);

}  // namespace enginex::journal
