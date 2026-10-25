// Journal format v1: CRC32C, payload codec, framing and scan classification (spec §9.2, §11.4).
#include <gtest/gtest.h>

#include "enginex/crc32c.hpp"
#include "enginex/journal.hpp"

namespace enginex::journal {
namespace {

std::string hex(std::string_view bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        out.push_back(kHex[b >> 4U]);
        out.push_back(kHex[b & 0xFU]);
    }
    return out;
}

Command order_cmd(const std::string& rid, Side side, PriceCents price, Quantity qty) {
    return NewLimitOrder{rid, "AAPL", side, price, qty};
}

// A valid journal image with `n` alternating buy/sell records.
std::string journal_with(int n) {
    std::string bytes = encode_header();
    for (int i = 1; i <= n; ++i) {
        append_record(
            bytes, static_cast<CommandSeq>(i),
            order_cmd("r" + std::to_string(i), i % 2 ? Side::Buy : Side::Sell, 10000 + i, i));
    }
    return bytes;
}

std::uint32_t get_length(const std::string& bytes, std::size_t off) {
    std::uint32_t v = 0;
    for (int b = 0; b < 4; ++b) {
        v |= static_cast<std::uint32_t>(
                 static_cast<unsigned char>(bytes[off + static_cast<std::size_t>(b)]))
             << (8 * b);
    }
    return v;
}

std::size_t record_size(CommandSeq seq, const Command& c) {
    return kFrameHeaderSize + encode_payload(seq, c).size();
}

TEST(Crc32c, KnownAnswers) {
    // RFC 3720 (iSCSI) B.4 test vectors and the common "123456789" check value.
    EXPECT_EQ(crc32c("123456789"), 0xE3069283U);
    EXPECT_EQ(crc32c(""), 0U);
    EXPECT_EQ(crc32c(std::string(32, '\0')), 0x8A9136AAU);
    EXPECT_EQ(crc32c(std::string(32, '\xFF')), 0x62A8AB43U);
    std::string inc;
    for (int i = 0; i < 32; ++i) {
        inc.push_back(static_cast<char>(i));
    }
    EXPECT_EQ(crc32c(inc), 0x46DD794EU);
    // Incremental use equals one-shot.
    EXPECT_EQ(crc32c("6789", 4, crc32c("12345")), crc32c("123456789"));
}

TEST(JournalFormat, HeaderBytes) {
    EXPECT_EQ(hex(encode_header()), "45584a3101000000");
}

TEST(JournalFormat, PayloadBytesArePinnedLittleEndian) {
    const std::string p = encode_payload(258, NewLimitOrder{"ab", "AAPL", Side::Sell, 10100, 3});
    EXPECT_EQ(hex(p),
              "01"                // payload version
              "01"                // new_limit
              "0201000000000000"  // sequence 258
              "02"
              "6162"  // request id "ab"
              "04"
              "4141504c"            // symbol "AAPL"
              "01"                  // SELL
              "7427000000000000"    // price 10100
              "0300000000000000"    // quantity 3
              "0201000000000000");  // order id 258
    const std::string c = encode_payload(9, CancelOrder{"c", "ord-7"});
    EXPECT_EQ(hex(c),
              "0102"
              "0900000000000000"
              "0163"
              "0700000000000000");
}

TEST(JournalFormat, PayloadRoundTrip) {
    std::string error;
    const Command n =
        NewLimitOrder{std::string(64, 'q'), "MSFT", Side::Buy, kMaxPrice, kMaxQuantity};
    auto rec = decode_payload(encode_payload(77, n), error);
    ASSERT_TRUE(rec) << error;
    EXPECT_EQ(rec->sequence, 77U);
    EXPECT_EQ(rec->command, n);
    const Command c = CancelOrder{"cx", "ord-18446744073709551615"};
    rec = decode_payload(encode_payload(5, c), error);
    ASSERT_TRUE(rec) << error;
    EXPECT_EQ(rec->command, c);
    // Negative values survive exactly (replay validation rejects them later, not the codec).
    const Command neg = NewLimitOrder{"n", "AAPL", Side::Buy, -1, -9};
    rec = decode_payload(encode_payload(1, neg), error);
    ASSERT_TRUE(rec);
    EXPECT_EQ(rec->command, neg);
}

TEST(JournalFormat, EncodeRejectsUnrepresentableCommands) {
    EXPECT_THROW((void)encode_payload(1, NewLimitOrder{"", "AAPL", Side::Buy, 1, 1}),
                 std::invalid_argument);
    EXPECT_THROW(
        (void)encode_payload(1, NewLimitOrder{std::string(65, 'x'), "AAPL", Side::Buy, 1, 1}),
        std::invalid_argument);
    EXPECT_THROW((void)encode_payload(1, NewLimitOrder{"r", std::string(33, 'S'), Side::Buy, 1, 1}),
                 std::invalid_argument);
    EXPECT_THROW((void)encode_payload(1, CancelOrder{"r", "nope"}), std::invalid_argument);
}

TEST(JournalFormat, DecodeRejectsMalformedPayloads) {
    const std::string good = encode_payload(3, NewLimitOrder{"ab", "AAPL", Side::Buy, 5, 6});
    std::string error;
    auto bad = [&](std::string p) { return !decode_payload(p, error).has_value(); };
    EXPECT_TRUE(bad(""));
    EXPECT_TRUE(bad(good.substr(0, good.size() - 1)));  // truncated
    EXPECT_TRUE(bad(good + "x"));                       // trailing bytes
    std::string v = good;
    v[0] = 2;  // version
    EXPECT_TRUE(bad(v));
    std::string t = good;
    t[1] = 9;  // type
    EXPECT_TRUE(bad(t));
    std::string side = good;
    side[2 + 8 + 3 + 5] = 2;  // side byte
    EXPECT_TRUE(bad(side));
    std::string oid = good;
    oid[oid.size() - 1] = 1;  // order id != sequence
    EXPECT_TRUE(bad(oid));
    std::string zero_seq = encode_payload(1, CancelOrder{"c", "ord-1"});
    zero_seq[2] = 0;
    EXPECT_TRUE(bad(zero_seq));
    std::string empty_rid = good;
    empty_rid[10] = 0;  // request id length 0
    EXPECT_TRUE(bad(empty_rid));
    std::string zero_target = encode_payload(2, CancelOrder{"c", "ord-1"});
    zero_target[zero_target.size() - 8] = 0;
    EXPECT_TRUE(bad(zero_target));
}

TEST(JournalScan, CleanJournalVisitsRecordsInOrder) {
    const std::string bytes = journal_with(3);
    std::vector<CommandSeq> seqs;
    std::vector<std::uint64_t> offsets;
    const ScanResult r = scan_bytes(bytes, [&](const Record& rec, std::uint64_t off) {
        seqs.push_back(rec.sequence);
        offsets.push_back(off);
    });
    EXPECT_EQ(r.tail, TailState::Clean);
    EXPECT_EQ(r.records, 3U);
    EXPECT_EQ(r.valid_bytes, bytes.size());
    EXPECT_EQ(r.last_sequence, 3U);
    EXPECT_EQ(seqs, (std::vector<CommandSeq>{1, 2, 3}));
    EXPECT_EQ(offsets.front(), kHeaderSize);
}

TEST(JournalScan, HeaderOnlyIsCleanAndEmpty) {
    const ScanResult r = scan_bytes(encode_header(), nullptr);
    EXPECT_EQ(r.tail, TailState::Clean);
    EXPECT_EQ(r.records, 0U);
    EXPECT_TRUE(r.header_complete);
}

TEST(JournalScan, IncompleteHeaderIsRecoverableTail) {
    const std::string h = encode_header();
    for (std::size_t n = 0; n < kHeaderSize; ++n) {
        const ScanResult r = scan_bytes(h.substr(0, n), nullptr);
        EXPECT_EQ(r.tail, TailState::Incomplete) << n;
        EXPECT_FALSE(r.header_complete);
        EXPECT_EQ(r.valid_bytes, 0U);
    }
}

TEST(JournalScan, BadHeaderIsCorrupt) {
    auto code_of = [](const std::string& bytes) {
        try {
            (void)scan_bytes(bytes, nullptr);
        } catch (const JournalError& e) {
            return e.code();
        }
        return ErrorCode::None;
    };
    std::string magic = encode_header();
    magic[0] = 'X';
    EXPECT_EQ(code_of(magic), ErrorCode::JournalCorrupt);
    EXPECT_EQ(code_of("EXJ"), ErrorCode::None);  // valid prefix: incomplete, not corrupt
    EXPECT_EQ(code_of("EXQ"), ErrorCode::JournalCorrupt);
    std::string version = encode_header();
    version[4] = 2;
    EXPECT_EQ(code_of(version), ErrorCode::JournalCorrupt);
    std::string flags = encode_header();
    flags[6] = 1;
    EXPECT_EQ(code_of(flags), ErrorCode::JournalCorrupt);
}

TEST(JournalScan, TruncationAtEveryByteOfTheLastRecordIsAnIncompleteTail) {
    const std::string two = journal_with(2);
    const std::string three = journal_with(3);
    for (std::size_t cut = two.size() + 1; cut < three.size(); ++cut) {
        int seen = 0;
        const ScanResult r = scan_bytes(std::string_view(three).substr(0, cut),
                                        [&](const Record&, std::uint64_t) { ++seen; });
        EXPECT_EQ(r.tail, TailState::Incomplete) << "cut " << cut;
        EXPECT_EQ(r.records, 2U);
        EXPECT_EQ(seen, 2);
        EXPECT_EQ(r.valid_bytes, two.size());
        EXPECT_FALSE(r.tail_reason.empty());
    }
}

TEST(JournalScan, ZeroFilledTailIsIncompleteNotCorrupt) {
    for (std::size_t zeros : {1U, 7U, 8U, 100U, 5000U}) {
        const std::string bytes = journal_with(2) + std::string(zeros, '\0');
        const ScanResult r = scan_bytes(bytes, nullptr);
        EXPECT_EQ(r.tail, TailState::Incomplete) << zeros;
        EXPECT_EQ(r.records, 2U);
        EXPECT_NE(r.tail_reason.find("zero"), std::string::npos) << r.tail_reason;
    }
}

TEST(JournalScan, ChecksumMismatchIsHardErrorAnywhere) {
    const std::string bytes = journal_with(3);
    const Command first = order_cmd("r1", Side::Buy, 10001, 1);
    const std::size_t first_payload = kHeaderSize + kFrameHeaderSize;
    for (const std::size_t pos : {first_payload, first_payload + 5, bytes.size() - 1}) {
        std::string damaged = bytes;
        damaged[pos] = static_cast<char>(damaged[pos] ^ 0x01);
        try {
            (void)scan_bytes(damaged, nullptr);
            ADD_FAILURE() << "corruption at " << pos << " not detected";
        } catch (const JournalError& e) {
            EXPECT_EQ(e.code(), ErrorCode::JournalCorrupt);
            EXPECT_NE(std::string(e.what()).find("checksum"), std::string::npos) << e.what();
        }
    }
    // Corrupting the stored CRC itself is detected the same way.
    std::string crc = bytes;
    crc[kHeaderSize + 4] = static_cast<char>(crc[kHeaderSize + 4] ^ 0x80);
    EXPECT_THROW((void)scan_bytes(crc, nullptr), JournalError);
    (void)first;
}

TEST(JournalScan, InvalidLengthsAreCorrupt) {
    std::string big = journal_with(1);
    const std::size_t off = big.size();
    big += journal_with(2).substr(off);  // append record 2
    big[off + 0] = static_cast<char>(0x01);
    big[off + 1] = static_cast<char>(0x10);  // length 4097
    big[off + 2] = 0;
    big[off + 3] = 0;
    EXPECT_THROW((void)scan_bytes(big, nullptr), JournalError);

    std::string zero_len = journal_with(1);
    zero_len += std::string(4, '\0') + "abcd" + "garbage";
    EXPECT_THROW((void)scan_bytes(zero_len, nullptr), JournalError);
}

TEST(JournalScan, SequenceGapAndRegressionAreCorrupt) {
    std::string gap = encode_header();
    append_record(gap, 1, order_cmd("a", Side::Buy, 1, 1));
    append_record(gap, 3, order_cmd("b", Side::Buy, 1, 1));
    try {
        (void)scan_bytes(gap, nullptr);
        ADD_FAILURE();
    } catch (const JournalError& e) {
        EXPECT_NE(std::string(e.what()).find("gap"), std::string::npos);
    }
    std::string regress = encode_header();
    append_record(regress, 1, order_cmd("a", Side::Buy, 1, 1));
    append_record(regress, 2, order_cmd("b", Side::Buy, 1, 1));
    append_record(regress, 2, order_cmd("c", Side::Buy, 1, 1));
    try {
        (void)scan_bytes(regress, nullptr);
        ADD_FAILURE();
    } catch (const JournalError& e) {
        EXPECT_NE(std::string(e.what()).find("regression"), std::string::npos);
    }
    std::string starts_at_two = encode_header();
    append_record(starts_at_two, 2, order_cmd("a", Side::Buy, 1, 1));
    EXPECT_THROW((void)scan_bytes(starts_at_two, nullptr), JournalError);
}

TEST(JournalScan, CorruptedLengthNearEofIsCorruptNotATornTail) {
    // The CRC covers only the payload. Raising a length field so the frame "extends" past EOF
    // must not turn complete, acknowledged records into a repairable tail.
    const std::string bytes = journal_with(5);
    std::size_t off = kHeaderSize;
    std::vector<std::size_t> offsets;
    for (int i = 1; i <= 5; ++i) {
        offsets.push_back(off);
        off += kFrameHeaderSize + get_length(bytes, off);
    }
    for (const std::size_t rec : offsets) {
        for (const std::uint32_t bump : {1U, 40U, 300U, 4000U}) {
            std::string damaged = bytes;
            const std::uint32_t len = get_length(bytes, rec) + bump;
            if (len > kMaxPayloadSize) {
                continue;
            }
            for (int b = 0; b < 4; ++b) {
                damaged[rec + static_cast<std::size_t>(b)] =
                    static_cast<char>((len >> (8 * b)) & 0xFFU);
            }
            EXPECT_THROW((void)scan_bytes(damaged, nullptr), JournalError)
                << "record at " << rec << " length +" << bump;
        }
    }
}

TEST(JournalScan, ZeroPayloadAfterValidFrameHeaderIsIncomplete) {
    // File extended past a written frame header but the payload never reached the disk.
    std::string bytes = journal_with(2);
    const std::string third = journal_with(3).substr(bytes.size());
    bytes += third.substr(0, kFrameHeaderSize) + std::string(10, '\0');
    const ScanResult r = scan_bytes(bytes, nullptr);
    EXPECT_EQ(r.tail, TailState::Incomplete);
    EXPECT_EQ(r.records, 2U);
}

TEST(JournalScan, MiddleRecordTruncationThenMoreDataIsCorrupt) {
    // Removing bytes from the middle shifts later frames; this must not look like a tail.
    const std::string bytes = journal_with(4);
    const std::size_t r1 = kHeaderSize + record_size(1, order_cmd("r1", Side::Buy, 10001, 1));
    std::string damaged = bytes.substr(0, r1 + 5) + bytes.substr(r1 + 9);
    EXPECT_THROW((void)scan_bytes(damaged, nullptr), JournalError);
}

TEST(FaultPlan, Parses) {
    auto p = FaultPlan::parse("crash=after_sync@5");
    EXPECT_EQ(p.crash_point, CrashPoint::AfterSync);
    EXPECT_EQ(p.crash_sequence, 5U);
    p = FaultPlan::parse("fail_write=2,partial=7,errno=ENOSPC");
    EXPECT_EQ(p.fail_write_call, 2U);
    EXPECT_EQ(p.partial_write_bytes, 7U);
    EXPECT_EQ(p.write_errno, ENOSPC);
    p = FaultPlan::parse("fail_sync=1,sync_errno=EIO,crash=after_apply@1");
    EXPECT_EQ(p.fail_sync_call, 1U);
    EXPECT_EQ(p.crash_point, CrashPoint::AfterApply);
    for (const char* bad : {"", "crash=after_sync", "crash=bogus@1", "fail_write=x", "what=1",
                            "errno=EWHAT", "fail_write=1,"}) {
        EXPECT_THROW((void)FaultPlan::parse(bad), std::invalid_argument) << bad;
    }
}

TEST(SyncMode, Parses) {
    EXPECT_EQ(parse_sync_mode("sync"), SyncMode::Full);
    EXPECT_EQ(parse_sync_mode("fsync"), SyncMode::Fsync);
    EXPECT_EQ(parse_sync_mode("none"), SyncMode::None);
    EXPECT_FALSE(parse_sync_mode("SYNC").has_value());
    EXPECT_EQ(to_string(SyncMode::Full), "sync");
}

}  // namespace
}  // namespace enginex::journal
