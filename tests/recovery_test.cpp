// Durable exchange: restart/replay, idempotent retries across restarts, tail repair, corruption
// refusal, injected I/O failures with fail-stop, and process-crash boundaries (spec §9, §11.4).
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

#include "enginex/canonical.hpp"
#include "enginex/durable.hpp"
#include "enginex/rng.hpp"
#include "temp_dir.hpp"
#include "test_util.hpp"

namespace enginex {
namespace {

namespace fs = std::filesystem;
using journal::CrashPoint;
using journal::FaultPlan;
using journal::JournalError;
using test::TempDir;

// A deterministic command list with fills, partial fills, cancels and a rejection.
std::vector<Command> script() {
    return {
        test::buy("a1", 10100, 10),          // seq 1
        test::sell("a2", 10000, 4),          // seq 2, fills 4 vs ord-1
        test::sell("a3", 10100, 2),          // seq 3, fills 2 vs ord-1
        test::sell("m1", 42000, 5, "MSFT"),  // seq 4
        test::buy("a4", 9900, 3),            // seq 5
        test::cancel("c1", 5),               // seq 6
        test::buy("bad", 0, 1),              // rejected
        test::buy("m2", 42000, 2, "MSFT"),   // seq 7, fills 2 vs ord-4
        test::sell("a5", 9000, 20),          // seq 8, sweeps ord-1 (4), rests 16
    };
}

std::string reference_hash(const std::vector<Command>& cmds) {
    ExchangeStateMachine s;
    for (const auto& c : cmds) {
        (void)s.process(c);
    }
    return state_hash(s);
}

DurableOptions options_for(const std::string& path, FaultPlan faults = {}) {
    DurableOptions o;
    o.journal_path = path;
    o.faults = faults;
    return o;
}

std::uint64_t file_size(const std::string& path) {
    return fs::file_size(path);
}

ErrorCode open_error(const DurableOptions& o) {
    try {
        (void)DurableExchange::open(o);
    } catch (const JournalError& e) {
        return e.code();
    }
    return ErrorCode::None;
}

void run_all(DurableExchange& ex, const std::vector<Command>& cmds) {
    CommandResult out;
    for (const auto& c : cmds) {
        ex.process(c, out);
        ASSERT_NE(out.error, ErrorCode::JournalIoError) << out.message;
    }
}

TEST(Recovery, CreatesJournalWithSyncedHeader) {
    TempDir dir;
    const std::string path = dir.file("j.exj");
    RecoveryReport rep;
    auto ex = DurableExchange::open(options_for(path), &rep);
    EXPECT_TRUE(rep.created);
    EXPECT_EQ(rep.records_replayed, 0U);
    EXPECT_EQ(file_size(path), journal::kHeaderSize);
    EXPECT_EQ(journal::read_file(path), journal::encode_header());
    EXPECT_EQ(ex->state().last_command_sequence(), 0U);
}

TEST(Recovery, RestartReconstructsCanonicalStateAndContinuesSequences) {
    TempDir dir;
    const std::string path = dir.file("j.exj");
    const auto cmds = script();
    std::string hash_before;
    {
        auto ex = DurableExchange::open(options_for(path));
        run_all(*ex, cmds);
        hash_before = state_hash(ex->state());
        EXPECT_EQ(ex->state().last_command_sequence(), 8U);
    }
    EXPECT_EQ(hash_before, reference_hash(cmds));
    RecoveryReport rep;
    auto ex = DurableExchange::open(options_for(path), &rep);
    EXPECT_FALSE(rep.created);
    EXPECT_EQ(rep.records_replayed, 8U);
    EXPECT_EQ(rep.tail, journal::TailState::Clean);
    EXPECT_EQ(state_hash(ex->state()), hash_before);

    CommandResult out;
    ex->process(test::buy("next", 9000, 16), out);  // fills the resting 16 from a5
    ASSERT_TRUE(out.accepted);
    EXPECT_EQ(out.command_sequence, 9U);
    EXPECT_EQ(out.order_id, 9U);
    ASSERT_EQ(out.executions.size(), 1U);
    EXPECT_EQ(out.executions[0].execution_sequence, ex->state().last_execution_sequence());
    EXPECT_EQ(out.executions[0].resting_order_id, 8U);
}

TEST(Recovery, RepeatedRestartsStayIdentical) {
    TempDir dir;
    const std::string path = dir.file("j.exj");
    const auto cmds = script();
    std::vector<Command> applied;
    for (std::size_t i = 0; i < cmds.size(); ++i) {
        auto ex = DurableExchange::open(options_for(path));
        EXPECT_EQ(state_hash(ex->state()), reference_hash(applied)) << "restart " << i;
        CommandResult out;
        ex->process(cmds[i], out);
        applied.push_back(cmds[i]);
    }
    for (int i = 0; i < 3; ++i) {
        auto ex = DurableExchange::open(options_for(path));
        EXPECT_EQ(state_hash(ex->state()), reference_hash(cmds));
    }
}

TEST(Recovery, RejectedAndDuplicateCommandsAreNotJournaled) {
    TempDir dir;
    const std::string path = dir.file("j.exj");
    auto ex = DurableExchange::open(options_for(path));
    CommandResult out;
    ex->process(test::buy("b1", 10000, 5), out);
    const auto size = file_size(path);
    const std::string hash = state_hash(ex->state());
    ex->process(test::buy("b2", 0, 5), out);
    EXPECT_EQ(out.error, ErrorCode::InvalidPrice);
    ex->process(test::cancel("c1", 99), out);
    EXPECT_EQ(out.error, ErrorCode::UnknownOrder);
    ex->process(test::buy("b1", 10000, 5), out);
    EXPECT_TRUE(out.duplicate);
    ex->process(test::buy("b1", 10000, 6), out);
    EXPECT_EQ(out.error, ErrorCode::DuplicateRequestIdConflict);
    EXPECT_EQ(file_size(path), size);
    EXPECT_EQ(state_hash(ex->state()), hash);
}

TEST(Idempotency, RetriesAfterRestartReturnOriginalResults) {
    TempDir dir;
    const std::string path = dir.file("j.exj");
    CommandResult order_first;
    CommandResult cancel_first;
    {
        auto ex = DurableExchange::open(options_for(path));
        CommandResult out;
        ex->process(test::sell("s1", 10000, 3), out);
        ex->process(test::buy("b1", 10100, 5), order_first);  // fills 3, rests 2
        ex->process(test::cancel("c1", 2), cancel_first);
        ASSERT_TRUE(order_first.accepted && cancel_first.accepted);
    }
    auto ex = DurableExchange::open(options_for(path));
    const auto size = file_size(path);
    const std::string hash = state_hash(ex->state());

    CommandResult retry;
    ex->process(test::buy("b1", 10100, 5), retry);
    EXPECT_TRUE(retry.accepted);
    EXPECT_TRUE(retry.duplicate);
    EXPECT_EQ(retry.command_sequence, order_first.command_sequence);
    EXPECT_EQ(retry.order_id, order_first.order_id);
    EXPECT_EQ(retry.remaining, order_first.remaining);
    EXPECT_EQ(retry.executions, order_first.executions);

    ex->process(test::cancel("c1", 2), retry);
    EXPECT_TRUE(retry.duplicate);
    EXPECT_EQ(retry.command_sequence, cancel_first.command_sequence);
    EXPECT_EQ(retry.cancelled_quantity, cancel_first.cancelled_quantity);

    ex->process(test::buy("b1", 10100, 6), retry);  // same id, changed body
    EXPECT_EQ(retry.error, ErrorCode::DuplicateRequestIdConflict);
    ex->process(test::cancel("c1", 1), retry);
    EXPECT_EQ(retry.error, ErrorCode::DuplicateRequestIdConflict);

    EXPECT_EQ(file_size(path), size);
    EXPECT_EQ(state_hash(ex->state()), hash);
}

class TailRepair : public ::testing::TestWithParam<std::size_t> {};

TEST_P(TailRepair, TruncatedLastRecordIsPreservedAndTruncated) {
    TempDir dir;
    const std::string path = dir.file("j.exj");
    const auto cmds = script();
    std::uint64_t size_before_last = 0;
    {
        auto ex = DurableExchange::open(options_for(path));
        for (std::size_t i = 0; i + 1 < cmds.size(); ++i) {
            CommandResult out;
            ex->process(cmds[i], out);
        }
        size_before_last = file_size(path);
        CommandResult out;
        ex->process(cmds.back(), out);
    }
    const std::uint64_t full = file_size(path);
    const std::size_t cut = GetParam();  // bytes removed from the end of the last record
    ASSERT_LT(cut, full - size_before_last);
    const std::string original = journal::read_file(path);
    fs::resize_file(path, full - cut);

    // Refuse policy leaves the file untouched.
    DurableOptions refuse = options_for(path);
    refuse.tail_policy = TailPolicy::Refuse;
    EXPECT_EQ(open_error(refuse), ErrorCode::JournalCorrupt);
    EXPECT_EQ(file_size(path), full - cut);

    RecoveryReport rep;
    auto ex = DurableExchange::open(options_for(path), &rep);
    EXPECT_EQ(rep.tail, journal::TailState::Incomplete);
    EXPECT_TRUE(rep.tail_repaired);
    EXPECT_EQ(rep.records_replayed, 7U);  // the 8th accepted command was cut
    EXPECT_EQ(file_size(path), size_before_last);
    ASSERT_FALSE(rep.preserved_tail_path.empty());
    EXPECT_EQ(journal::read_file(rep.preserved_tail_path),
              original.substr(size_before_last, full - cut - size_before_last));

    std::vector<Command> expected(cmds.begin(), cmds.end() - 1);
    EXPECT_EQ(state_hash(ex->state()), reference_hash(expected));
    // The lost command was never acknowledged under sync mode; a retry executes it now.
    CommandResult out;
    ex->process(cmds.back(), out);
    EXPECT_TRUE(out.accepted);
    EXPECT_FALSE(out.duplicate);
    EXPECT_EQ(out.command_sequence, 8U);
    ex.reset();
    RecoveryReport again;
    auto ex2 = DurableExchange::open(options_for(path), &again);
    EXPECT_EQ(again.tail, journal::TailState::Clean);
    EXPECT_EQ(state_hash(ex2->state()), reference_hash(cmds));
}

// 1..7 bytes cut leaves a partial payload; cutting into the frame header is covered by
// cutting (payload + 1..7) bytes, computed from the record size in the test below.
INSTANTIATE_TEST_SUITE_P(CutBytes, TailRepair, ::testing::Values(1, 2, 7, 13, 20));

TEST(Recovery, TruncatedFrameHeaderIsRepaired) {
    TempDir dir;
    const std::string path = dir.file("j.exj");
    {
        auto ex = DurableExchange::open(options_for(path));
        run_all(*ex, script());
    }
    // Append 5 bytes of a frame header (a write that died after 5 bytes).
    {
        std::ofstream f(path, std::ios::app | std::ios::binary);
        f.write("\x20\x00\x00\x00\x11", 5);
    }
    RecoveryReport rep;
    auto ex = DurableExchange::open(options_for(path), &rep);
    EXPECT_TRUE(rep.tail_repaired);
    EXPECT_NE(rep.tail_reason.find("partial record header"), std::string::npos) << rep.tail_reason;
    EXPECT_EQ(rep.records_replayed, 8U);
    EXPECT_EQ(state_hash(ex->state()), reference_hash(script()));
}

TEST(Recovery, IncompleteFileHeaderIsRewritten) {
    TempDir dir;
    const std::string path = dir.file("j.exj");
    {
        std::ofstream f(path, std::ios::binary);
        f.write("EXJ", 3);
    }
    RecoveryReport rep;
    auto ex = DurableExchange::open(options_for(path), &rep);
    EXPECT_TRUE(rep.tail_repaired);
    EXPECT_EQ(journal::read_file(path), journal::encode_header());
    CommandResult out;
    ex->process(test::buy("b", 100, 1), out);
    EXPECT_TRUE(out.accepted);
}

TEST(Recovery, ZeroFilledTailIsRepaired) {
    TempDir dir;
    const std::string path = dir.file("j.exj");
    {
        auto ex = DurableExchange::open(options_for(path));
        run_all(*ex, script());
    }
    const auto size = file_size(path);
    fs::resize_file(path, size + 4096);  // extends with zeros
    RecoveryReport rep;
    auto ex = DurableExchange::open(options_for(path), &rep);
    EXPECT_TRUE(rep.tail_repaired);
    EXPECT_EQ(file_size(path), size);
    EXPECT_EQ(state_hash(ex->state()), reference_hash(script()));
}

TEST(Recovery, CorruptedRecordRefusesStartupAndLeavesFileUntouched) {
    TempDir dir;
    const std::string path = dir.file("j.exj");
    {
        auto ex = DurableExchange::open(options_for(path));
        run_all(*ex, script());
    }
    std::string bytes = journal::read_file(path);
    for (const std::size_t pos : {journal::kHeaderSize + 12, bytes.size() / 2, bytes.size() - 1}) {
        std::string damaged = bytes;
        damaged[pos] = static_cast<char>(damaged[pos] ^ 0x40);
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f.write(damaged.data(), static_cast<std::streamsize>(damaged.size()));
        }
        EXPECT_EQ(open_error(options_for(path)), ErrorCode::JournalCorrupt) << pos;
        EXPECT_EQ(journal::read_file(path), damaged) << "file modified after refusal";
    }
    std::string bad_magic = bytes;
    bad_magic[1] = 'Y';
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(bad_magic.data(), static_cast<std::streamsize>(bad_magic.size()));
    }
    EXPECT_EQ(open_error(options_for(path)), ErrorCode::JournalCorrupt);
}

// Property: whatever happens to the file (random truncation, random byte flips anywhere,
// or both), recovery either reproduces exactly the state after some prefix of the journaled
// commands, or refuses with JOURNAL_CORRUPT. There is no third outcome, i.e. no silent
// divergence. Truncation alone must always recover (a torn tail is never "corruption").
TEST(Recovery, RandomDamageNeverYieldsAStateOutsideThePrefixes) {
    TempDir dir;
    const std::string path = dir.file("p.exj");
    Rng rng(9001);
    std::vector<Command> cmds;
    for (int i = 0; i < 80; ++i) {
        const auto r = rng.below(10);
        const std::string rid = "p" + std::to_string(i);
        if (r < 4) {
            cmds.push_back(test::buy(rid, 10000 + static_cast<PriceCents>(rng.below(20)) * 10,
                                     1 + static_cast<Quantity>(rng.below(9))));
        } else if (r < 8) {
            cmds.push_back(test::sell(rid, 10000 + static_cast<PriceCents>(rng.below(20)) * 10,
                                      1 + static_cast<Quantity>(rng.below(9))));
        } else {
            cmds.push_back(test::cancel(rid, static_cast<OrderId>(1 + rng.below(static_cast<std::uint64_t>(i) + 1))));
        }
    }
    DurableOptions o = options_for(path);
    o.sync = journal::SyncMode::None;  // recovery semantics are under test, not durability
    {
        auto ex = DurableExchange::open(o);
        CommandResult out;
        for (const auto& c : cmds) ex->process(c, out);
    }
    // Hash after each journaled (accepted) prefix, from an independent in-memory run.
    std::set<std::string> prefixes;
    {
        ExchangeStateMachine ref;
        prefixes.insert(state_hash(ref));
        CommandResult out;
        for (const auto& c : cmds) {
            ref.process(c, out);
            if (out.accepted && !out.duplicate) prefixes.insert(state_hash(ref));
        }
    }
    const std::string clean = journal::read_file(path);
    int recovered = 0;
    int refused = 0;
    for (int trial = 0; trial < 400; ++trial) {
        std::string bytes = clean;
        const auto mode = rng.below(3);  // 0 truncate, 1 flip, 2 both
        if (mode != 1) bytes.resize(rng.below(bytes.size() + 1));
        if (mode != 0 && !bytes.empty()) {
            for (auto k = 1 + rng.below(3); k > 0; --k) {
                bytes[rng.below(bytes.size())] ^= static_cast<char>(1U << rng.below(8));
            }
        }
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }
        try {
            auto ex = DurableExchange::open(o);
            ASSERT_TRUE(prefixes.contains(state_hash(ex->state())))
                << "trial " << trial << ": recovered a state that is no journaled prefix";
            ++recovered;
        } catch (const JournalError& e) {
            ASSERT_NE(mode, 0U) << "trial " << trial << ": pure truncation refused: " << e.what();
            ASSERT_EQ(e.code(), ErrorCode::JournalCorrupt) << e.what();
            ++refused;
        }
    }
    EXPECT_GT(recovered, 100);
    EXPECT_GT(refused, 50);
    std::printf("journal damage property: %d recovered to a prefix, %d refused, %zu prefixes\n",
                recovered, refused, prefixes.size());
}

TEST(Recovery, ReplayDivergenceIsCorrupt) {
    TempDir dir;
    const std::string path = dir.file("j.exj");
    {
        auto ex = DurableExchange::open(options_for(path));
        run_all(*ex, script());  // contains MSFT orders
    }
    DurableOptions aapl_only = options_for(path);
    aapl_only.state.symbols = {"AAPL"};
    try {
        (void)DurableExchange::open(aapl_only);
        ADD_FAILURE() << "expected divergence";
    } catch (const JournalError& e) {
        EXPECT_EQ(e.code(), ErrorCode::JournalCorrupt);
        EXPECT_NE(std::string(e.what()).find("divergence"), std::string::npos) << e.what();
    }

    // A hand-written journal whose second record reuses a request id.
    const std::string dup = dir.file("dup.exj");
    std::string bytes = journal::encode_header();
    journal::append_record(bytes, 1, test::buy("x", 100, 1));
    journal::append_record(bytes, 2, test::buy("x", 100, 1));
    {
        std::ofstream f(dup, std::ios::binary);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    EXPECT_EQ(open_error(options_for(dup)), ErrorCode::JournalCorrupt);

    // A cancel of an order that was never open is equally impossible to replay.
    const std::string bad_cancel = dir.file("cancel.exj");
    bytes = journal::encode_header();
    journal::append_record(bytes, 1, test::cancel("c", 1));
    {
        std::ofstream f(bad_cancel, std::ios::binary);
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    EXPECT_EQ(open_error(options_for(bad_cancel)), ErrorCode::JournalCorrupt);
}

TEST(Recovery, SecondWriterIsLockedOut) {
    TempDir dir;
    const std::string path = dir.file("j.exj");
    auto first = DurableExchange::open(options_for(path));
    try {
        (void)DurableExchange::open(options_for(path));
        ADD_FAILURE() << "second writer was allowed";
    } catch (const JournalError& e) {
        EXPECT_EQ(e.code(), ErrorCode::JournalIoError);
        EXPECT_NE(std::string(e.what()).find("locked"), std::string::npos);
    }
    first.reset();
    EXPECT_NO_THROW((void)DurableExchange::open(options_for(path)));
}

TEST(Recovery, SymlinkedJournalPathIsRefused) {
    TempDir dir;
    const std::string target = dir.file("target.exj");
    {
        auto ex = DurableExchange::open(options_for(target));
    }
    const std::string link = dir.file("link.exj");
    fs::create_symlink(target, link);
    EXPECT_EQ(open_error(options_for(link)), ErrorCode::JournalIoError);
    const std::string dangling = dir.file("dangling.exj");
    fs::create_symlink(dir.file("nowhere.exj"), dangling);
    EXPECT_EQ(open_error(options_for(dangling)), ErrorCode::JournalIoError);
    EXPECT_FALSE(fs::exists(dir.file("nowhere.exj")));
}

TEST(Recovery, MissingJournalWithoutCreateIsIoError) {
    TempDir dir;
    DurableOptions o = options_for(dir.file("absent.exj"));
    o.create_if_missing = false;
    EXPECT_EQ(open_error(o), ErrorCode::JournalIoError);
}

// ---- injected I/O failures --------------------------------------------------------------

struct FailCase {
    const char* name;
    const char* spec;
    bool record_survives;  // whether the failed command's full record reached the file
};
void PrintTo(const FailCase& c, std::ostream* os) {
    *os << c.name;
}

class IoFailure : public ::testing::TestWithParam<FailCase> {};

TEST_P(IoFailure, FailStopsAndRecoversWithIdempotentRetry) {
    TempDir dir;
    const std::string path = dir.file("j.exj");
    const FailCase fc = GetParam();
    // Calls 1-2 succeed; the third accepted command hits the fault.
    auto ex = DurableExchange::open(options_for(path, FaultPlan::parse(fc.spec)));
    CommandResult out;
    ex->process(test::sell("s1", 10000, 5), out);
    ASSERT_TRUE(out.accepted);
    ex->process(test::buy("b1", 9000, 1), out);
    ASSERT_TRUE(out.accepted);
    const std::string durable_hash = state_hash(ex->state());

    ex->process(test::buy("b2", 10000, 3), out);  // would fill 3 vs s1
    EXPECT_FALSE(out.accepted);
    EXPECT_EQ(out.error, ErrorCode::JournalIoError);
    EXPECT_NE(out.message.find("outcome unknown"), std::string::npos) << out.message;
    EXPECT_TRUE(ex->failed());

    // Fail-stop: every later request is refused, including valid ones and retries.
    ex->process(test::buy("b3", 9000, 1), out);
    EXPECT_EQ(out.error, ErrorCode::JournalIoError);
    ex->process(test::sell("s1", 10000, 5), out);
    EXPECT_EQ(out.error, ErrorCode::JournalIoError);
    ex.reset();

    RecoveryReport rep;
    auto again = DurableExchange::open(options_for(path), &rep);
    EXPECT_EQ(rep.records_replayed, fc.record_survives ? 3U : 2U);
    CommandResult retry;
    again->process(test::buy("b2", 10000, 3), retry);
    ASSERT_TRUE(retry.accepted);
    EXPECT_EQ(retry.duplicate, fc.record_survives);  // never applied twice
    EXPECT_EQ(retry.command_sequence, 3U);
    ASSERT_EQ(retry.executions.size(), 1U);
    EXPECT_EQ(again->state().last_execution_sequence(), 1U);
    if (!fc.record_survives) {
        EXPECT_EQ(rep.records_replayed, 2U);
    }
    (void)durable_hash;
}

INSTANTIATE_TEST_SUITE_P(
    Faults, IoFailure,
    ::testing::Values(
        FailCase{"write_eio_nothing_written", "fail_write=3", false},
        FailCase{"write_enospc_partial", "fail_write=3,partial=11,errno=ENOSPC", false},
        FailCase{"write_error_after_full_record", "fail_write=3,partial=100000", true},
        FailCase{"sync_failure_record_written", "fail_sync=3", true}),
    [](const auto& param_info) { return std::string(param_info.param.name); });

TEST(IoFailure, EnospcMessageIsReported) {
    TempDir dir;
    auto ex = DurableExchange::open(
        options_for(dir.file("j.exj"), FaultPlan::parse("fail_write=1,errno=ENOSPC")));
    CommandResult out;
    ex->process(test::buy("b", 100, 1), out);
    EXPECT_EQ(out.error, ErrorCode::JournalIoError);
    EXPECT_NE(ex->failure_reason().find("No space left"), std::string::npos)
        << ex->failure_reason();
}

// ---- process crashes at each boundary ----------------------------------------------------

struct CrashCase {
    const char* name;
    const char* spec;  // empty = crash after the reply was produced
    bool record_survives;
};
void PrintTo(const CrashCase& c, std::ostream* os) {
    *os << c.name;
}

class CrashBoundary : public ::testing::TestWithParam<CrashCase> {};

TEST_P(CrashBoundary, RestartResolvesAmbiguityThroughRetry) {
    TempDir dir;
    const std::string path = dir.file("j.exj");
    const CrashCase cc = GetParam();
    const auto cmds = script();  // the 5th command (a4) gets sequence 5
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        // Child: no gtest assertions; exit codes only.
        try {
            FaultPlan plan = std::string(cc.spec).empty() ? FaultPlan{} : FaultPlan::parse(cc.spec);
            auto ex = DurableExchange::open(options_for(path, plan));
            CommandResult out;
            for (std::size_t i = 0; i < 5; ++i) {
                ex->process(cmds[i], out);
            }
            ::_exit(journal::kCrashExitCode);  // "crash after reply" for the empty spec
        } catch (...) {
            ::_exit(3);
        }
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), journal::kCrashExitCode);

    RecoveryReport rep;
    auto ex = DurableExchange::open(options_for(path), &rep);
    const std::uint64_t expected_records = cc.record_survives ? 5 : 4;
    EXPECT_EQ(rep.records_replayed, expected_records);
    EXPECT_EQ(rep.tail, journal::TailState::Clean);
    std::vector<Command> applied(cmds.begin(), cmds.begin() + static_cast<long>(expected_records));
    EXPECT_EQ(state_hash(ex->state()), reference_hash(applied));

    // The client never saw (or may have lost) the 5th reply and retries with the same id.
    CommandResult retry;
    ex->process(cmds[4], retry);
    ASSERT_TRUE(retry.accepted);
    EXPECT_EQ(retry.duplicate, cc.record_survives);
    EXPECT_EQ(retry.command_sequence, 5U);
    EXPECT_EQ(ex->state().last_command_sequence(), 5U);
    std::vector<Command> five(cmds.begin(), cmds.begin() + 5);
    EXPECT_EQ(state_hash(ex->state()), reference_hash(five));
}

INSTANTIATE_TEST_SUITE_P(
    Points, CrashBoundary,
    ::testing::Values(CrashCase{"before_durable_record", "crash=after_apply@5", false},
                      CrashCase{"after_write_before_sync", "crash=after_write@5", true},
                      CrashCase{"after_sync_before_reply", "crash=after_sync@5", true},
                      CrashCase{"after_reply", "", true}),
    [](const auto& param_info) { return std::string(param_info.param.name); });

// ---- batching (used by the R3 sequencer for group commit) --------------------------------

TEST(Batch, GroupCommitWritesOnceAndReleasesAfterSync) {
    TempDir dir;
    const std::string path = dir.file("j.exj");
    auto ex = DurableExchange::open(options_for(path));
    std::vector<CommandResult> outs(4);
    ex->stage(test::sell("s1", 10000, 5), outs[0]);
    ex->stage(test::buy("b1", 10000, 2), outs[1]);
    ex->stage(test::buy("b1", 10000, 2), outs[2]);  // duplicate of a not-yet-durable command
    ex->stage(test::cancel("c1", 1), outs[3]);
    EXPECT_EQ(ex->staged_records(), 3U);
    EXPECT_EQ(file_size(path), journal::kHeaderSize);  // nothing written before flush
    ASSERT_EQ(ex->flush(), ErrorCode::None);
    EXPECT_TRUE(outs[2].duplicate);
    EXPECT_EQ(outs[3].cancelled_quantity, 3);
    auto reopened_hash = [&] {
        ex.reset();
        auto r = DurableExchange::open(options_for(path));
        return state_hash(r->state());
    };
    ExchangeStateMachine ref;
    (void)ref.process(test::sell("s1", 10000, 5));
    (void)ref.process(test::buy("b1", 10000, 2));
    (void)ref.process(test::cancel("c1", 1));
    EXPECT_EQ(reopened_hash(), state_hash(ref));
}

TEST(Batch, FailedGroupFlushFailStops) {
    TempDir dir;
    const std::string path = dir.file("j.exj");
    auto ex = DurableExchange::open(options_for(path, FaultPlan::parse("fail_sync=1")));
    CommandResult a;
    CommandResult b;
    ex->stage(test::sell("s1", 10000, 5), a);
    ex->stage(test::buy("b1", 10000, 2), b);
    EXPECT_EQ(ex->flush(), ErrorCode::JournalIoError);
    EXPECT_TRUE(ex->failed());
    ex->make_io_error(a);
    EXPECT_EQ(a.error, ErrorCode::JournalIoError);
    EXPECT_EQ(a.request_id, "s1");
}

TEST(SyncModes, AllModesPersistAcrossRestart) {
    for (const auto mode :
         {journal::SyncMode::Full, journal::SyncMode::Fsync, journal::SyncMode::None}) {
        TempDir dir;
        const std::string path = dir.file("j.exj");
        {
            DurableOptions o = options_for(path);
            o.sync = mode;
            auto ex = DurableExchange::open(o);
            run_all(*ex, script());
        }
        auto ex = DurableExchange::open(options_for(path));
        EXPECT_EQ(state_hash(ex->state()), reference_hash(script())) << journal::to_string(mode);
    }
}

}  // namespace
}  // namespace enginex
