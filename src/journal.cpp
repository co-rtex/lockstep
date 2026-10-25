#include "enginex/journal.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <filesystem>

#include "enginex/crc32c.hpp"

namespace enginex::journal {

namespace {

// ---- little-endian helpers ----------------------------------------------------------------

void put_u8(std::string& out, std::uint8_t v) {
    out.push_back(static_cast<char>(v));
}

void put_u16(std::string& out, std::uint16_t v) {
    for (int i = 0; i < 2; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
    }
}

void put_u32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
    }
}

void put_u64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFU));
    }
}

void put_bytes8(std::string& out, std::string_view bytes) {
    put_u8(out, static_cast<std::uint8_t>(bytes.size()));
    out.append(bytes);
}

std::uint64_t get_le(std::string_view bytes, std::size_t offset, int width) {
    std::uint64_t v = 0;
    for (int i = 0; i < width; ++i) {
        v |= static_cast<std::uint64_t>(
                 static_cast<unsigned char>(bytes[offset + static_cast<std::size_t>(i)]))
             << (8 * i);
    }
    return v;
}

// Bounds-checked sequential reader over a payload.
class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}

    bool u8(std::uint8_t& v) {
        if (!have(1)) {
            return false;
        }
        v = static_cast<std::uint8_t>(bytes_[pos_++]);
        return true;
    }
    bool u64(std::uint64_t& v) {
        if (!have(8)) {
            return false;
        }
        v = get_le(bytes_, pos_, 8);
        pos_ += 8;
        return true;
    }
    bool i64(std::int64_t& v) {
        std::uint64_t u = 0;
        if (!u64(u)) {
            return false;
        }
        std::memcpy(&v, &u, sizeof v);  // two's complement reinterpretation, no UB
        return true;
    }
    bool bytes8(std::string& out, std::size_t max_len) {
        std::uint8_t len = 0;
        if (!u8(len) || len == 0 || len > max_len || !have(len)) {
            return false;
        }
        out.assign(bytes_.substr(pos_, len));
        pos_ += len;
        return true;
    }
    [[nodiscard]] bool done() const noexcept { return pos_ == bytes_.size(); }

private:
    bool have(std::size_t n) const noexcept { return bytes_.size() - pos_ >= n; }

    std::string_view bytes_;
    std::size_t pos_ = 0;
};

bool all_zero(std::string_view bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](char c) { return c == 0; });
}

[[noreturn]] void corrupt(const std::string& message, std::uint64_t offset) {
    throw JournalError(ErrorCode::JournalCorrupt, message + " at offset " + std::to_string(offset),
                       offset);
}

[[noreturn]] void io_error(const std::string& what, const std::string& path, int err) {
    throw JournalError(ErrorCode::JournalIoError, what + " " + path + ": " + std::strerror(err));
}

int sync_fd_full(int fd) {
#if defined(__APPLE__)
    int rc = 0;
    do {
        rc = ::fcntl(fd, F_FULLFSYNC);
    } while (rc != 0 && errno == EINTR);
    if (rc == 0) {
        return 0;
    }
    if (errno != ENOTSUP && errno != EINVAL && errno != ENOTTY) {
        return errno;
    }
    // Filesystem without F_FULLFSYNC support: fall back to fsync.
#endif
#if defined(__linux__)
    int rc2 = 0;
    do {
        rc2 = ::fdatasync(fd);
    } while (rc2 != 0 && errno == EINTR);
    return rc2 == 0 ? 0 : errno;
#else
    int rc2 = 0;
    do {
        rc2 = ::fsync(fd);
    } while (rc2 != 0 && errno == EINTR);
    return rc2 == 0 ? 0 : errno;
#endif
}

int sync_fd_fsync(int fd) {
    int rc = 0;
    do {
        rc = ::fsync(fd);
    } while (rc != 0 && errno == EINTR);
    return rc == 0 ? 0 : errno;
}

int raw_write_all(int fd, const char* data, std::size_t size) {
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno;
        }
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return 0;
}

std::optional<int> parse_errno(std::string_view text) {
    if (text == "EIO") {
        return EIO;
    }
    if (text == "ENOSPC") {
        return ENOSPC;
    }
    if (text == "EDQUOT") {
        return EDQUOT;
    }
    if (text == "EROFS") {
        return EROFS;
    }
    int v = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (ec == std::errc{} && ptr == text.data() + text.size() && v > 0) {
        return v;
    }
    return std::nullopt;
}

std::uint64_t parse_u64_or_throw(std::string_view text, std::string_view key) {
    std::uint64_t v = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    if (text.empty() || ec != std::errc{} || ptr != text.data() + text.size()) {
        throw std::invalid_argument("fault spec: bad number for " + std::string(key));
    }
    return v;
}

}  // namespace

// ---- encoding -----------------------------------------------------------------------------

std::string encode_header() {
    std::string out(kMagic);
    put_u16(out, kFormatVersion);
    put_u16(out, 0);
    return out;
}

std::string encode_payload(CommandSeq sequence, const Command& command) {
    std::string out;
    out.reserve(128);
    put_u8(out, kPayloadVersion);
    put_u8(out, static_cast<std::uint8_t>(type_of(command)));
    put_u64(out, sequence);
    const std::string& rid = request_id_of(command);
    if (rid.empty() || rid.size() > kMaxRequestIdBytes) {
        throw std::invalid_argument("request id cannot be journaled");
    }
    put_bytes8(out, rid);
    if (const auto* order = std::get_if<NewLimitOrder>(&command)) {
        if (order->symbol.empty() || order->symbol.size() > kMaxSymbolBytes) {
            throw std::invalid_argument("symbol cannot be journaled");
        }
        put_bytes8(out, order->symbol);
        put_u8(out, static_cast<std::uint8_t>(order->side));
        put_u64(out, static_cast<std::uint64_t>(order->price));
        put_u64(out, static_cast<std::uint64_t>(order->quantity));
        put_u64(out, sequence);  // generated order id
    } else {
        const auto target = parse_order_id(std::get<CancelOrder>(command).order_id);
        if (!target) {
            throw std::invalid_argument("cancel target cannot be journaled");
        }
        put_u64(out, *target);
    }
    if (out.size() > kMaxPayloadSize) {
        throw std::invalid_argument("payload exceeds maximum size");
    }
    return out;
}

void append_record(std::string& out, CommandSeq sequence, const Command& command) {
    const std::string payload = encode_payload(sequence, command);
    put_u32(out, static_cast<std::uint32_t>(payload.size()));
    put_u32(out, crc32c(payload));
    out.append(payload);
}

std::optional<Record> decode_payload(std::string_view payload, std::string& error) {
    Reader r(payload);
    std::uint8_t version = 0;
    std::uint8_t type = 0;
    Record rec;
    std::string request_id;
    if (!r.u8(version) || version != kPayloadVersion) {
        error = "unsupported payload version";
        return std::nullopt;
    }
    if (!r.u8(type) || !r.u64(rec.sequence) || rec.sequence == 0) {
        error = "malformed payload prefix";
        return std::nullopt;
    }
    if (!r.bytes8(request_id, kMaxRequestIdBytes)) {
        error = "malformed request id";
        return std::nullopt;
    }
    if (type == static_cast<std::uint8_t>(CommandType::NewLimit)) {
        NewLimitOrder order;
        order.request_id = std::move(request_id);
        std::uint8_t side = 0;
        std::uint64_t order_id = 0;
        if (!r.bytes8(order.symbol, kMaxSymbolBytes) || !r.u8(side) || side > 1 ||
            !r.i64(order.price) || !r.i64(order.quantity) || !r.u64(order_id)) {
            error = "malformed new_limit payload";
            return std::nullopt;
        }
        if (order_id != rec.sequence) {
            error = "order id does not match command sequence";
            return std::nullopt;
        }
        order.side = static_cast<Side>(side);
        rec.command = std::move(order);
    } else if (type == static_cast<std::uint8_t>(CommandType::Cancel)) {
        std::uint64_t target = 0;
        if (!r.u64(target) || target == 0) {
            error = "malformed cancel payload";
            return std::nullopt;
        }
        rec.command = CancelOrder{std::move(request_id), format_order_id(target)};
    } else {
        error = "unknown command type";
        return std::nullopt;
    }
    if (!r.done()) {
        error = "trailing bytes in payload";
        return std::nullopt;
    }
    return rec;
}

// ---- scanning -----------------------------------------------------------------------------

ScanResult scan_bytes(std::string_view bytes, const RecordCallback& on_record) {
    ScanResult r;
    r.file_bytes = bytes.size();
    const std::string header = encode_header();
    if (bytes.size() < kHeaderSize) {
        if (bytes != std::string_view(header).substr(0, bytes.size())) {
            corrupt("invalid journal header", 0);
        }
        r.tail = TailState::Incomplete;
        r.tail_reason = bytes.empty()
                            ? "empty file (no header)"
                            : "incomplete header (" + std::to_string(bytes.size()) + " of 8 bytes)";
        return r;
    }
    if (bytes.substr(0, 4) != kMagic) {
        corrupt("bad journal magic", 0);
    }
    if (get_le(bytes, 4, 2) != kFormatVersion) {
        corrupt("unsupported journal format version " + std::to_string(get_le(bytes, 4, 2)), 4);
    }
    if (get_le(bytes, 6, 2) != 0) {
        corrupt("unsupported journal flags", 6);
    }
    r.header_complete = true;
    std::size_t offset = kHeaderSize;
    r.valid_bytes = offset;
    CommandSeq expected = 1;
    std::string error;
    while (true) {
        const std::size_t remaining = bytes.size() - offset;
        if (remaining == 0) {
            r.tail = TailState::Clean;
            break;
        }
        const std::string_view rest = bytes.substr(offset);
        if (remaining < kFrameHeaderSize) {
            r.tail = TailState::Incomplete;
            r.tail_reason =
                all_zero(rest)
                    ? "zero-filled tail of " + std::to_string(remaining) + " bytes"
                    : "partial record header (" + std::to_string(remaining) + " of 8 bytes)";
            break;
        }
        const auto length = static_cast<std::size_t>(get_le(bytes, offset, 4));
        const auto crc = static_cast<std::uint32_t>(get_le(bytes, offset + 4, 4));
        if (length == 0 || length > kMaxPayloadSize) {
            if (all_zero(rest)) {
                r.tail = TailState::Incomplete;
                r.tail_reason = "zero-filled tail of " + std::to_string(remaining) + " bytes";
                break;
            }
            corrupt("invalid record length " + std::to_string(length), offset);
        }
        if (remaining - kFrameHeaderSize < length) {
            r.tail = TailState::Incomplete;
            r.tail_reason = "partial record payload (" +
                            std::to_string(remaining - kFrameHeaderSize) + " of " +
                            std::to_string(length) + " bytes)";
            break;
        }
        const std::string_view payload = bytes.substr(offset + kFrameHeaderSize, length);
        if (crc32c(payload) != crc) {
            corrupt("record checksum mismatch", offset);
        }
        auto rec = decode_payload(payload, error);
        if (!rec) {
            corrupt("undecodable record (" + error + ")", offset);
        }
        if (rec->sequence != expected) {
            corrupt("sequence " + std::string(rec->sequence < expected ? "regression" : "gap") +
                        ": expected " + std::to_string(expected) + ", found " +
                        std::to_string(rec->sequence),
                    offset);
        }
        if (on_record) {
            on_record(*rec, offset);
        }
        offset += kFrameHeaderSize + length;
        r.valid_bytes = offset;
        r.last_sequence = rec->sequence;
        ++r.records;
        ++expected;
    }
    return r;
}

std::string read_file(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        io_error("cannot open", path, errno);
    }
    std::string out;
    char buf[1 << 16];
    while (true) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int err = errno;
            ::close(fd);
            io_error("cannot read", path, err);
        }
        if (n == 0) {
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

ScanResult scan_file(const std::string& path, const RecordCallback& on_record) {
    return scan_bytes(read_file(path), on_record);
}

// ---- fault plan ---------------------------------------------------------------------------

FaultPlan FaultPlan::parse(std::string_view spec) {
    FaultPlan plan;
    std::size_t start = 0;
    while (start <= spec.size()) {
        const std::size_t comma = spec.find(',', start);
        const std::string_view item = spec.substr(
            start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
        if (item.empty()) {
            throw std::invalid_argument("fault spec: empty item");
        }
        const std::size_t eq = item.find('=');
        if (eq == std::string_view::npos) {
            throw std::invalid_argument("fault spec: expected key=value");
        }
        const std::string_view key = item.substr(0, eq);
        const std::string_view value = item.substr(eq + 1);
        if (key == "crash") {
            const std::size_t at = value.find('@');
            if (at == std::string_view::npos) {
                throw std::invalid_argument("fault spec: crash=<point>@<sequence>");
            }
            const std::string_view point = value.substr(0, at);
            if (point == "after_apply") {
                plan.crash_point = CrashPoint::AfterApply;
            } else if (point == "after_write") {
                plan.crash_point = CrashPoint::AfterWrite;
            } else if (point == "after_sync") {
                plan.crash_point = CrashPoint::AfterSync;
            } else {
                throw std::invalid_argument("fault spec: unknown crash point");
            }
            plan.crash_sequence = parse_u64_or_throw(value.substr(at + 1), "crash");
        } else if (key == "fail_write") {
            plan.fail_write_call = parse_u64_or_throw(value, key);
        } else if (key == "partial") {
            plan.partial_write_bytes = static_cast<std::size_t>(parse_u64_or_throw(value, key));
        } else if (key == "errno" || key == "sync_errno") {
            const auto err = parse_errno(value);
            if (!err) {
                throw std::invalid_argument("fault spec: bad errno");
            }
            (key == "errno" ? plan.write_errno : plan.sync_errno) = *err;
        } else if (key == "fail_sync") {
            plan.fail_sync_call = parse_u64_or_throw(value, key);
        } else {
            throw std::invalid_argument("fault spec: unknown key " + std::string(key));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return plan;
}

// ---- writer -------------------------------------------------------------------------------

std::string_view to_string(SyncMode mode) noexcept {
    switch (mode) {
        case SyncMode::Full:
            return "sync";
        case SyncMode::Fsync:
            return "fsync";
        case SyncMode::None:
            return "none";
    }
    return "invalid";
}

std::optional<SyncMode> parse_sync_mode(std::string_view text) noexcept {
    if (text == "sync") {
        return SyncMode::Full;
    }
    if (text == "fsync") {
        return SyncMode::Fsync;
    }
    if (text == "none") {
        return SyncMode::None;
    }
    return std::nullopt;
}

int sync_parent_directory(const std::string& path) {
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    const int dfd = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC);
    if (dfd < 0) {
        return errno;
    }
    const int rc = sync_fd_fsync(dfd);
    ::close(dfd);
    return rc;
}

JournalFile::JournalFile(std::string path, int fd, SyncMode mode, FaultPlan faults, bool created)
    : path_(std::move(path)), fd_(fd), mode_(mode), faults_(faults), created_(created) {}

JournalFile::~JournalFile() {
    if (fd_ >= 0) {
        ::close(fd_);  // also releases the flock
    }
}

std::unique_ptr<JournalFile> JournalFile::open(const std::string& path, bool create, SyncMode mode,
                                               FaultPlan faults) {
    bool created = false;
    int fd = ::open(path.c_str(), O_RDWR | O_APPEND | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT && create) {
        fd = ::open(path.c_str(), O_RDWR | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        created = fd >= 0;
    }
    if (fd < 0) {
        io_error("cannot open journal", path, errno);
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int err = errno;
        ::close(fd);
        if (err == EWOULDBLOCK) {
            throw JournalError(ErrorCode::JournalIoError,
                               "journal " + path + " is locked by another process");
        }
        io_error("cannot lock journal", path, err);
    }
    std::unique_ptr<JournalFile> file(new JournalFile(path, fd, mode, faults, created));
    if (created) {
        const std::string header = encode_header();
        int err = raw_write_all(fd, header.data(), header.size());
        if (err == 0) {
            err = sync_fd_full(fd);
        }
        if (err == 0) {
            err = sync_parent_directory(path);
        }
        if (err != 0) {
            io_error("cannot initialize journal", path, err);
        }
    }
    return file;
}

int JournalFile::write_all(std::string_view bytes) {
    ++write_calls_;
    if (faults_.fail_write_call != 0 && write_calls_ == faults_.fail_write_call) {
        const std::size_t partial = std::min(faults_.partial_write_bytes, bytes.size());
        if (partial > 0) {
            (void)raw_write_all(fd_, bytes.data(), partial);
        }
        return faults_.write_errno;
    }
    return raw_write_all(fd_, bytes.data(), bytes.size());
}

int JournalFile::sync() {
    ++sync_calls_;
    if (faults_.fail_sync_call != 0 && sync_calls_ == faults_.fail_sync_call) {
        return faults_.sync_errno;
    }
    switch (mode_) {
        case SyncMode::Full:
            return sync_fd_full(fd_);
        case SyncMode::Fsync:
            return sync_fd_fsync(fd_);
        case SyncMode::None:
            return 0;
    }
    return EINVAL;
}

int JournalFile::truncate_and_sync(std::uint64_t size) {
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        return errno;
    }
    return sync_fd_full(fd_);
}

std::string JournalFile::read_all() const {
    struct stat st{};
    if (::fstat(fd_, &st) != 0) {
        io_error("cannot stat journal", path_, errno);
    }
    std::string out(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t n =
            ::pread(fd_, out.data() + done, out.size() - done, static_cast<off_t>(done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            io_error("cannot read journal", path_, errno);
        }
        if (n == 0) {
            out.resize(done);  // file shrank concurrently; should not happen under the lock
            break;
        }
        done += static_cast<std::size_t>(n);
    }
    return out;
}

}  // namespace enginex::journal
