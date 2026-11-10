// ITCH order-book reconstruction: specification semantics, anomaly handling, a differential
// test against the naive reference model, determinism and replay error policy.
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <streambuf>

#include "enginex/itch/book.hpp"
#include "enginex/itch/replay.hpp"
#include "enginex/itch/synthetic.hpp"
#include "enginex/rng.hpp"
#include "itch_reference_model.hpp"

namespace enginex::itch {

void PrintTo(const LevelView& l, std::ostream* os) {
    *os << "{" << l.price << " x" << l.shares << " n" << l.orders << "}";
}
void PrintTo(const OrderView& o, std::ostream* os) {
    *os << "{ref " << o.ref << " " << static_cast<char>(o.side) << " " << o.price << " x"
        << o.shares << "}";
}

namespace {

constexpr std::uint16_t kL = 1;  // stock locate used by the scenario tests

Header hdr(char type, std::uint16_t locate = kL) {
    static std::uint64_t ts = 1000;
    return Header{type, locate, 0, ts += 10};
}
AddOrder add(OrderRef ref, Side side, Price4 price, std::uint32_t shares,
             std::uint16_t locate = kL, const char* stock = "AAPL") {
    AddOrder a;
    a.h = hdr('A', locate);
    a.order_ref = ref;
    a.side = side;
    a.price = price;
    a.shares = shares;
    a.stock = make_symbol(stock);
    return a;
}
OrderExecuted exec(OrderRef ref, std::uint32_t n) { return {hdr('E'), ref, n, 1}; }
OrderExecutedWithPrice exec_px(OrderRef ref, std::uint32_t n, bool printable, Price4 px) {
    return {hdr('C'), ref, n, 2, printable, px};
}
OrderCancel cancel(OrderRef ref, std::uint32_t n) { return {hdr('X'), ref, n}; }
OrderDelete del(OrderRef ref, std::uint16_t locate = kL) { return {hdr('D', locate), ref}; }
OrderReplace replace(OrderRef from, OrderRef to, std::uint32_t n, Price4 px) {
    return {hdr('U'), from, to, n, px};
}

std::vector<OrderRef> refs(const BookBuilder& b, Side s) {
    std::vector<OrderRef> out;
    for (const auto& o : b.orders(kL, s)) out.push_back(o.ref);
    return out;
}

ApplyOutcome apply(BookBuilder& b, const Message& m) {
    const ApplyOutcome out = b.apply(m);
    const auto bad = b.check_invariants();
    EXPECT_FALSE(bad) << *bad;
    return out;
}

TEST(ItchBook, AddsBuildSortedLevelsWithFifo) {
    BookBuilder b;
    apply(b, add(1, Side::Buy, 100, 10));
    apply(b, add(2, Side::Buy, 101, 20));
    apply(b, add(3, Side::Buy, 100, 30));
    apply(b, add(4, Side::Sell, 103, 5));
    apply(b, add(5, Side::Sell, 102, 6));
    EXPECT_EQ(b.depth(kL, Side::Buy, 0),
              (std::vector<LevelView>{{101, 20, 1}, {100, 40, 2}}));
    EXPECT_EQ(b.depth(kL, Side::Sell, 0), (std::vector<LevelView>{{102, 6, 1}, {103, 5, 1}}));
    EXPECT_EQ(b.depth(kL, Side::Buy, 1), (std::vector<LevelView>{{101, 20, 1}}));
    EXPECT_EQ(refs(b, Side::Buy), (std::vector<OrderRef>{2, 1, 3}));
    const TopOfBook t = b.top(kL);
    ASSERT_TRUE(t.bid && t.ask);
    EXPECT_EQ(t.bid->price, 101U);
    EXPECT_EQ(t.ask->price, 102U);
    EXPECT_FALSE(t.crossed());
    EXPECT_EQ(b.stats().live_orders, 5U);
    EXPECT_EQ(b.symbol(kL), "AAPL");
}

TEST(ItchBook, ExecutionsAndPartialCancelKeepPriority) {
    BookBuilder b;
    apply(b, add(1, Side::Sell, 200, 100));
    apply(b, add(2, Side::Sell, 200, 100));
    EXPECT_EQ(apply(b, exec(1, 30)).status, ApplyStatus::Applied);
    EXPECT_EQ(apply(b, cancel(1, 20)).status, ApplyStatus::Applied);
    EXPECT_EQ(refs(b, Side::Sell), (std::vector<OrderRef>{1, 2}));  // 1 still first
    EXPECT_EQ(b.find_order(1)->shares, 50U);
    EXPECT_EQ(b.depth(kL, Side::Sell, 0), (std::vector<LevelView>{{200, 150, 2}}));
    // Effects are cumulative; reaching zero removes the order.
    apply(b, exec(1, 50));
    EXPECT_FALSE(b.find_order(1));
    EXPECT_EQ(refs(b, Side::Sell), (std::vector<OrderRef>{2}));
    apply(b, cancel(2, 100));  // a cancel of all remaining shares also removes it
    EXPECT_EQ(b.level_count(kL, Side::Sell), 0U);
    EXPECT_EQ(b.stats().executed_shares, 80U);
    EXPECT_EQ(b.stats().canceled_shares, 120U);
}

TEST(ItchBook, ExecutionWithPriceDoesNotMoveTheOrderAndRespectsPrintable) {
    BookBuilder b;
    apply(b, add(1, Side::Buy, 500, 100));
    apply(b, exec_px(1, 10, /*printable=*/false, 499));
    apply(b, exec_px(1, 15, /*printable=*/true, 498));
    EXPECT_EQ(b.find_order(1)->price, 500U);  // display price unchanged
    EXPECT_EQ(b.find_order(1)->shares, 75U);
    EXPECT_EQ(b.stats().executed_shares, 25U);
    EXPECT_EQ(b.stats().printable_executed_shares, 15U);
}

TEST(ItchBook, ReplaceLosesPriorityEvenAtTheSamePriceAndSize) {
    BookBuilder b;
    AddOrder a = add(1, Side::Buy, 100, 10);
    a.attribution = Mpid{'G', 'S', 'C', 'O'};
    apply(b, a);
    apply(b, add(2, Side::Buy, 100, 10));
    EXPECT_EQ(apply(b, replace(1, 3, 10, 100)).status, ApplyStatus::Applied);
    EXPECT_EQ(refs(b, Side::Buy), (std::vector<OrderRef>{2, 3}));
    EXPECT_FALSE(b.find_order(1));
    // Replace to a new price: same side, new level, back of its queue.
    apply(b, add(4, Side::Buy, 105, 1));
    apply(b, replace(2, 5, 7, 105));
    EXPECT_EQ(refs(b, Side::Buy), (std::vector<OrderRef>{4, 5, 3}));
    EXPECT_EQ(b.find_order(5)->side, Side::Buy);
    // Attribution survives a replace (the U message carries none).
    ASSERT_TRUE(b.attribution(3));
    EXPECT_EQ(std::string(b.attribution(3)->data(), 4), "GSCO");
    EXPECT_FALSE(b.attribution(5));
    EXPECT_EQ(b.depth(kL, Side::Buy, 0), (std::vector<LevelView>{{105, 8, 2}, {100, 10, 1}}));
    EXPECT_EQ(b.stats().replaces, 2U);
}

TEST(ItchBook, DeleteRemovesOrderAndEmptyLevel) {
    BookBuilder b;
    apply(b, add(1, Side::Sell, 100, 10));
    apply(b, add(2, Side::Sell, 101, 10));
    apply(b, del(1));
    EXPECT_EQ(b.depth(kL, Side::Sell, 0), (std::vector<LevelView>{{101, 10, 1}}));
    EXPECT_EQ(b.stats().live_orders, 1U);
    EXPECT_EQ(b.stats().peak_live_orders, 2U);
}

TEST(ItchBook, TradesCrossesAndBreaksNeverChangeTheBook) {
    BookBuilder b;
    apply(b, add(1, Side::Buy, 100, 10));
    apply(b, add(2, Side::Sell, 101, 10));
    const std::string before = b.state_hash();
    const Message msgs[] = {
        Trade{hdr('P'), 0, 'B', 500, make_symbol("AAPL"), 100, 9},
        CrossTrade{hdr('Q'), 10000, make_symbol("AAPL"), 100, 10, 'O'},
        BrokenTrade{hdr('B'), 9},
        Noii{hdr('I'), 1, 2, 'B', make_symbol("AAPL"), 0, 0, 100, 'O', ' '},
        StockTradingAction{hdr('H'), make_symbol("AAPL"), 'H', ' ', {' ', ' ', ' ', ' '}},
        SystemEvent{hdr('S', 0), 'Q'},
    };
    for (const Message& m : msgs) {
        EXPECT_EQ(apply(b, m).status, ApplyStatus::NoBookEffect);
    }
    EXPECT_EQ(b.state_hash(), before);
    EXPECT_EQ(b.stats().non_displayed_trade_shares, 500U);
    EXPECT_EQ(b.stats().cross_shares, 10000U);
    EXPECT_EQ(b.stats().broken_trades, 1U);
    EXPECT_EQ(b.stats().last_system_event, 'Q');
    EXPECT_EQ(b.trading_state(kL), 'H');
}

TEST(ItchBook, AnomaliesAreRejectedWithoutChangingTheBook) {
    BookBuilder b;
    apply(b, add(1, Side::Buy, 100, 10));
    apply(b, add(2, Side::Sell, 105, 10));
    const std::string before = b.state_hash();
    struct Case {
        Message m;
        Anomaly expected;
    };
    const Case cases[] = {
        {exec(99, 1), Anomaly::UnknownOrderRef},
        {exec_px(99, 1, true, 1), Anomaly::UnknownOrderRef},
        {cancel(99, 1), Anomaly::UnknownOrderRef},
        {del(99), Anomaly::UnknownOrderRef},
        {replace(99, 100, 1, 1), Anomaly::UnknownOrderRef},
        {exec(1, 11), Anomaly::OverExecution},
        {cancel(1, 11), Anomaly::OverExecution},
        {exec_px(1, 11, true, 100), Anomaly::OverExecution},
        {exec(1, 0), Anomaly::ZeroShares},
        {cancel(1, 0), Anomaly::ZeroShares},
        {add(3, Side::Buy, 100, 0), Anomaly::ZeroShares},
        {replace(1, 3, 0, 100), Anomaly::ZeroShares},
        {add(1, Side::Buy, 100, 5), Anomaly::DuplicateOrderRef},
        {replace(1, 2, 5, 100), Anomaly::DuplicateOrderRef},  // new ref already live
        {del(1, 7), Anomaly::LocateMismatch},
        {OrderExecuted{hdr('E', 7), 1, 1, 1}, Anomaly::LocateMismatch},
        {OrderExecutedWithPrice{hdr('C', 7), 1, 1, 1, true, 1}, Anomaly::LocateMismatch},
        {OrderCancel{hdr('X', 7), 1, 1}, Anomaly::LocateMismatch},
        {OrderReplace{hdr('U', 7), 1, 50, 1, 1}, Anomaly::LocateMismatch},
    };
    for (const Case& c : cases) {
        const ApplyOutcome out = apply(b, c.m);
        EXPECT_EQ(out.status, ApplyStatus::Rejected) << to_string(c.expected);
        EXPECT_EQ(out.anomaly, c.expected);
        EXPECT_EQ(b.state_hash(), before) << to_string(c.expected);
    }
    EXPECT_EQ(b.stats().rejected, std::size(cases));
    EXPECT_EQ(b.stats().anomaly_count(), std::size(cases));
}

TEST(ItchBook, ReplaceMayReuseItsOwnReference) {
    BookBuilder b;
    apply(b, add(1, Side::Buy, 100, 10));
    apply(b, add(2, Side::Buy, 100, 10));
    EXPECT_EQ(apply(b, replace(1, 1, 4, 100)).status, ApplyStatus::Applied);
    EXPECT_EQ(refs(b, Side::Buy), (std::vector<OrderRef>{2, 1}));  // still loses priority
}

TEST(ItchBook, DirectorySymbolMismatchAndConflictAreInformational) {
    BookBuilder b;
    StockDirectory d;
    d.h = hdr('R');
    d.stock = make_symbol("AAPL");
    EXPECT_EQ(apply(b, d).anomaly, Anomaly::None);
    EXPECT_EQ(b.find_locate("AAPL"), kL);
    const ApplyOutcome mismatch = apply(b, add(1, Side::Buy, 1, 1, kL, "MSFT"));
    EXPECT_EQ(mismatch.status, ApplyStatus::Applied);  // locate is authoritative
    EXPECT_EQ(mismatch.anomaly, Anomaly::SymbolMismatch);
    d.stock = make_symbol("MSFT");
    EXPECT_EQ(apply(b, d).anomaly, Anomaly::DirectoryConflict);
    EXPECT_EQ(b.symbol(kL), "MSFT");
}

TEST(ItchBook, DirectoryOnlyLocatesAreListedAndSymbolsHashExactly) {
    BookBuilder a;
    StockDirectory d;
    d.h = hdr('R', 9);
    d.stock = make_symbol("ZVZZT");
    apply(a, d);
    EXPECT_EQ(a.locates(), (std::vector<std::uint16_t>{9}));
    EXPECT_TRUE(a.depth(9, Side::Buy, 0).empty());
    EXPECT_NE(a.canonical_state(true).find("locate 9 ZVZZT"), std::string::npos);
    // Symbols differing only in an embedded NUL vs space must not hash the same.
    BookBuilder b;
    BookBuilder c;
    StockDirectory e = d;
    e.stock[5] = '\0';
    apply(b, d);
    apply(c, e);
    EXPECT_NE(b.state_hash(), c.state_hash());
}

TEST(ItchBook, CrossedAndLockedBooksAreReportedNotRejected) {
    BookBuilder b;
    apply(b, add(1, Side::Buy, 100, 1));
    apply(b, add(2, Side::Sell, 100, 1));
    EXPECT_TRUE(b.top(kL).locked());
    EXPECT_EQ(apply(b, add(3, Side::Buy, 101, 1)).status, ApplyStatus::Applied);
    EXPECT_TRUE(b.top(kL).crossed());
}

TEST(ItchBook, BooksAreIndependentPerLocate) {
    BookBuilder b;
    apply(b, add(1, Side::Buy, 100, 1, 1, "AAA"));
    apply(b, add(2, Side::Buy, 200, 1, 2, "BBB"));
    EXPECT_EQ(b.locates(), (std::vector<std::uint16_t>{1, 2}));
    EXPECT_EQ(b.top(1).bid->price, 100U);
    EXPECT_EQ(b.top(2).bid->price, 200U);
    EXPECT_EQ(b.symbol(2), "BBB");
}

// ---- differential test against the reference model ------------------------------------------

void compare_books(const BookBuilder& b, const test::ReferenceBook& ref, std::uint16_t symbols,
                   std::size_t at) {
    for (std::uint16_t l = 1; l <= symbols; ++l) {
        for (const Side s : {Side::Buy, Side::Sell}) {
            ASSERT_EQ(b.orders(l, s), ref.orders(l, s)) << "locate " << l << " msg " << at;
            ASSERT_EQ(b.depth(l, s, 0), ref.levels(l, s)) << "locate " << l << " msg " << at;
        }
    }
    ASSERT_EQ(b.stats().live_orders, ref.live()) << at;
}

class ItchDifferential : public ::testing::TestWithParam<std::uint32_t> {};

TEST_P(ItchDifferential, MatchesReferenceModelMessageByMessage) {
    SyntheticOptions o;
    o.seed = 1000 + GetParam();
    o.messages = 20'000;
    o.symbols = static_cast<std::uint16_t>(1 + GetParam() % 5);
    o.anomalies_per_million = GetParam() % 2 == 0 ? 0 : 20'000;  // half the seeds inject 2%
    const std::vector<Message> stream = generate(o);
    BookBuilder b;
    test::ReferenceBook ref;
    std::size_t anomalies = 0;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        const ApplyOutcome out = b.apply(stream[i]);
        const Anomaly expected = ref.apply(stream[i]);
        ASSERT_EQ(out.anomaly, expected) << "message " << i;
        anomalies += expected != Anomaly::None;
        if (i % 997 == 0) {
            compare_books(b, ref, o.symbols, i);
            const auto bad = b.check_invariants();
            ASSERT_FALSE(bad) << *bad;
        }
    }
    compare_books(b, ref, o.symbols, stream.size());
    EXPECT_EQ(b.stats().executed_shares, ref.executed_shares());
    EXPECT_EQ(b.stats().canceled_shares, ref.canceled_shares());
    if (o.anomalies_per_million == 0) {
        EXPECT_EQ(anomalies, 0U) << "clean synthetic stream must replay without anomalies";
        EXPECT_EQ(b.stats().anomaly_count(), 0U);
    } else {
        EXPECT_GT(anomalies, 0U);
        EXPECT_EQ(b.stats().anomaly_count(), anomalies);
    }
}

INSTANTIATE_TEST_SUITE_P(Seeds, ItchDifferential, ::testing::Range(0U, 12U));

// ---- determinism and replay policy ----------------------------------------------------------

TEST(ItchBook, CapacityHintDoesNotChangeResultsAndUnknownLocatesAreEmpty) {
    SyntheticOptions o;
    o.seed = 21;
    o.messages = 20'000;
    o.symbols = 3;
    const auto stream = generate(o);
    BookBuilder plain;
    BookBuilder hinted(100'000);
    for (const Message& m : stream) {
        ASSERT_EQ(plain.apply(m).anomaly, hinted.apply(m).anomaly);
    }
    EXPECT_EQ(plain.state_hash(), hinted.state_hash());
    EXPECT_FALSE(hinted.check_invariants());
    // Queries on a locate that never appeared.
    EXPECT_FALSE(plain.symbol(999));
    EXPECT_FALSE(plain.trading_state(999));
    EXPECT_FALSE(plain.top(999).bid);
    EXPECT_TRUE(plain.depth(999, Side::Buy, 0).empty());
    EXPECT_TRUE(plain.orders(999, Side::Sell).empty());
    EXPECT_EQ(plain.level_count(999, Side::Buy), 0U);
    EXPECT_FALSE(plain.find_locate("NOPE"));
    EXPECT_FALSE(plain.attribution(123456789));
}

std::vector<std::uint8_t> framed(const std::vector<Message>& msgs);

// A stream whose buffer throws after the first chunk: istream sets badbit, replay reports it.
class FailingBuf : public std::streambuf {
public:
    explicit FailingBuf(std::string first) : first_(std::move(first)) {
        setg(first_.data(), first_.data(), first_.data() + first_.size());
    }

protected:
    int_type underflow() override { throw std::runtime_error("disk error"); }

private:
    std::string first_;
};

TEST(ItchReplay, ReadErrorStopsWithAReason) {
    const auto file = framed({add(1, Side::Buy, 100, 10), add(2, Side::Buy, 100, 10)});
    // 48 bytes = 3 whole 16-byte reads (message 1 complete + 10 bytes of message 2); the 4th
    // read throws. (Bytes of a read() call that fails part-way are lost by the stream.)
    FailingBuf buf(std::string(file.begin(), file.begin() + 48));
    std::istream in(&buf);
    BookBuilder b;
    ReplayOptions o;
    o.chunk_bytes = 16;
    const ReplayResult r = replay(in, b, o);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(*r.stopped, "read error");
    EXPECT_EQ(r.frames, 1U);
    EXPECT_EQ(b.stats().live_orders, 1U);
    EXPECT_EQ(r.frame_error, FrameError::None);  // not misreported as a truncated file
}

TEST(ItchReplay, DeterministicAcrossRunsAndChunkSizes) {
    SyntheticOptions o;
    o.seed = 77;
    o.messages = 30'000;
    o.symbols = 4;
    const std::vector<std::uint8_t> file = generate_file(o);
    EXPECT_EQ(file, generate_file(o));  // generator is a pure function of its options
    BookBuilder first;
    const ReplayResult r1 = replay(file, first);
    ASSERT_TRUE(r1.ok());
    EXPECT_EQ(r1.decode_error_count(), 0U);
    EXPECT_EQ(first.stats().anomaly_count(), 0U);
    const std::string hash = first.state_hash();
    for (const std::size_t chunk : {1UL, 2UL, 3UL, 7UL, 64UL, 4096UL}) {
        BookBuilder b;
        std::istringstream in(std::string(file.begin(), file.end()));
        ReplayOptions opts;
        opts.chunk_bytes = chunk;
        const ReplayResult r = replay(in, b, opts);
        ASSERT_TRUE(r.ok()) << chunk;
        EXPECT_EQ(r.frames, r1.frames);
        EXPECT_EQ(b.state_hash(), hash) << "chunk " << chunk;
    }
}

TEST(ItchSynthetic, CleanStreamsNeverLockOrCrossAndExecuteAtTheTop) {
    SyntheticOptions o;
    o.seed = 3;
    o.messages = 50'000;
    o.symbols = 3;
    BookBuilder b;
    std::uint64_t executions = 0;
    std::uint64_t at_top = 0;
    std::uint64_t checked = 0;
    ReplayOptions opts;
    // Before applying an execution, check that it hits the best order of its side.
    BookBuilder shadow;
    opts.on_message = [&](const Message& m, const ApplyOutcome& out) {
        ASSERT_NE(out.status, ApplyStatus::Rejected);
        const Header& h = header_of(m);
        if (const auto* e = std::get_if<OrderExecuted>(&m)) {
            const auto o_view = shadow.find_order(e->order_ref);
            ASSERT_TRUE(o_view);
            const auto best = shadow.orders(h.stock_locate, o_view->side);
            ++executions;
            at_top += !best.empty() && best.front().ref == e->order_ref;
        }
        (void)shadow.apply(m);
        if (h.stock_locate != 0 && ++checked % 7 == 0) {
            const TopOfBook t = b.top(h.stock_locate);
            ASSERT_FALSE(t.locked() || t.crossed()) << "message " << checked;
        }
    };
    const ReplayResult r = replay(generate_file(o), b, opts);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(b.stats().anomaly_count(), 0U);
    EXPECT_GT(executions, 1000U);
    EXPECT_EQ(at_top, executions);  // every E hits the best-priority order of its side
}

std::vector<std::uint8_t> framed(const std::vector<Message>& msgs) {
    std::vector<std::uint8_t> out;
    for (const Message& m : msgs) append_framed(out, m);
    return out;
}

TEST(ItchReplay, LenientCountsAndSkipsStrictStopsAtFirstProblem) {
    std::vector<std::uint8_t> file = framed({add(1, Side::Buy, 100, 10)});
    // A frame with an unknown type, then a known type with a bad length.
    file.insert(file.end(), {0x00, 0x03, 'z', 1, 2});
    file.insert(file.end(), {0x00, 0x02, 'A', 0});
    const auto tail = framed({exec(5, 1), add(2, Side::Buy, 100, 10)});
    file.insert(file.end(), tail.begin(), tail.end());

    BookBuilder lenient;
    const ReplayResult r = replay(file, lenient);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(r.frames, 5U);
    EXPECT_EQ(r.decoded, 3U);
    EXPECT_EQ(r.decode_errors[static_cast<std::size_t>(DecodeError::UnknownType)], 1U);
    EXPECT_EQ(r.decode_errors[static_cast<std::size_t>(DecodeError::LengthMismatch)], 1U);
    EXPECT_EQ(r.unknown_types['z'], 1U);
    EXPECT_EQ(lenient.stats().anomalies[static_cast<std::size_t>(Anomaly::UnknownOrderRef)], 1U);
    EXPECT_EQ(lenient.stats().live_orders, 2U);

    BookBuilder strict;
    ReplayOptions so;
    so.strict = true;
    const ReplayResult s = replay(file, strict, so);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.stopped_at_message, 2U);
    EXPECT_NE(s.stopped->find("unknown_type"), std::string::npos) << *s.stopped;
    EXPECT_EQ(strict.stats().live_orders, 1U);  // nothing after the stop was applied
}

TEST(ItchReplay, StrictStopsOnBookAnomaly) {
    const auto file = framed({add(1, Side::Buy, 100, 10), cancel(1, 11), del(1)});
    BookBuilder b;
    ReplayOptions so;
    so.strict = true;
    const ReplayResult s = replay(file, b, so);
    ASSERT_FALSE(s.ok());
    EXPECT_EQ(s.stopped_at_message, 2U);
    EXPECT_NE(s.stopped->find("over_execution"), std::string::npos);
    EXPECT_EQ(b.stats().live_orders, 1U);
}

// Regression (independent review): after a strict stop the result must describe the stop
// point only — later frames are not counted and later framing errors are not reported.
TEST(ItchReplay, StrictStopFreezesTheResultAtTheStopPoint) {
    auto file = framed({add(1, Side::Buy, 100, 10), cancel(1, 11), del(1), add(2, Side::Buy, 1, 1)});
    file.insert(file.end(), {0x00, 0x00});  // a framing error after the stop
    for (const bool use_stream : {false, true}) {
        BookBuilder b;
        ReplayOptions so;
        so.strict = true;
        so.chunk_bytes = 5;
        ReplayResult r;
        if (use_stream) {
            std::istringstream in(std::string(file.begin(), file.end()));
            r = replay(in, b, so);
        } else {
            r = replay(file, b, so);
        }
        ASSERT_FALSE(r.ok());
        EXPECT_EQ(r.stopped_at_message, 2U);
        EXPECT_EQ(r.frames, 2U) << use_stream;
        EXPECT_EQ(r.decoded, 2U);
        EXPECT_EQ(r.bytes_processed, 38U + 25U) << use_stream;  // A (2+36) + X (2+23)
        EXPECT_EQ(r.frame_error, FrameError::None) << use_stream;
        EXPECT_NE(r.stopped->find("over_execution"), std::string::npos);
    }
}

TEST(ItchReplay, LenientResultCountsEveryFrame) {
    const auto file = framed({add(1, Side::Buy, 100, 10), cancel(1, 11), del(1)});
    BookBuilder b;
    const ReplayResult r = replay(file, b);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.frames, 3U);
    EXPECT_EQ(r.bytes_processed, file.size());
    EXPECT_EQ(r.bytes_read, file.size());
}

TEST(ItchReplay, TruncatedAndZeroLengthStreamsAreFatal) {
    auto file = framed({add(1, Side::Buy, 100, 10), add(2, Side::Buy, 100, 10)});
    file.pop_back();
    BookBuilder b;
    const ReplayResult r = replay(file, b);
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.frame_error, FrameError::TruncatedTail);
    EXPECT_EQ(r.frame_error_offset, 38U);  // second message's prefix
    EXPECT_EQ(b.stats().live_orders, 1U);

    auto z = framed({add(1, Side::Buy, 100, 10)});
    z.insert(z.end(), {0, 0});
    const auto more = framed({add(2, Side::Buy, 100, 10)});
    z.insert(z.end(), more.begin(), more.end());
    BookBuilder b2;
    const ReplayResult r2 = replay(z, b2);
    ASSERT_FALSE(r2.ok());
    EXPECT_EQ(r2.frame_error, FrameError::ZeroLength);
    EXPECT_EQ(r2.stopped_at_message, 2U);
    EXPECT_EQ(b2.stats().live_orders, 1U);
}

// Random corruption of a valid file: the replay must never crash, never violate book
// invariants and always classify what happened.
TEST(ItchReplay, CorruptedFilesNeverBreakInvariants) {
    SyntheticOptions o;
    o.seed = 5;
    o.messages = 3'000;
    o.symbols = 3;
    const std::vector<std::uint8_t> clean = generate_file(o);
    Rng rng(123);
    for (int trial = 0; trial < 300; ++trial) {
        std::vector<std::uint8_t> f = clean;
        const int flips = 1 + static_cast<int>(rng.below(20));
        for (int i = 0; i < flips; ++i) {
            f[rng.below(f.size())] = static_cast<std::uint8_t>(rng.next());
        }
        if (rng.chance(30)) {
            f.resize(rng.below(f.size()));
        }
        BookBuilder b;
        const ReplayResult r = replay(f, b);
        const auto bad = b.check_invariants();
        ASSERT_FALSE(bad) << "trial " << trial << ": " << *bad;
        EXPECT_EQ(r.decoded + r.decode_error_count(), r.frames) << trial;
        // Replaying the same corrupted bytes is deterministic.
        BookBuilder b2;
        (void)replay(f, b2);
        ASSERT_EQ(b.state_hash(), b2.state_hash());
    }
}

// Opt-in: set ENGINEX_ITCH_REAL_FILE to a length-prefixed ITCH 5.0 file (for example a prefix of
// one of Nasdaq's publicly posted sample files). Never required; skipped otherwise. Replays it
// strictly through BookBuilder and through the naive reference model, then compares every
// order of every book.
TEST(ItchRealData, MatchesReferenceModelIfAvailable) {
    const char* path = std::getenv("ENGINEX_ITCH_REAL_FILE");
    if (path == nullptr || *path == '\0') {
        GTEST_SKIP() << "ENGINEX_ITCH_REAL_FILE not set";
    }
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in) << path;
    BookBuilder book(2'000'000);
    test::ReferenceBook ref;
    std::uint64_t mismatched_anomalies = 0;
    ReplayOptions o;
    o.on_message = [&](const Message& m, const ApplyOutcome& out) {
        mismatched_anomalies += ref.apply(m) != out.anomaly ? 1 : 0;
    };
    const ReplayResult r = replay(in, book, o);
    EXPECT_EQ(r.decode_error_count(), 0U);
    EXPECT_TRUE(r.ok() || r.frame_error == FrameError::TruncatedTail) << *r.stopped;
    EXPECT_EQ(mismatched_anomalies, 0U);
    EXPECT_EQ(book.stats().anomaly_count(), 0U);
    const auto groups = ref.grouped();
    std::size_t compared = 0;
    for (const auto& [key, orders] : groups) {
        ASSERT_EQ(book.orders(key.first, static_cast<Side>(key.second)), orders)
            << "locate " << key.first << " side " << key.second;
        compared += orders.size();
    }
    EXPECT_EQ(compared, book.stats().live_orders);
    EXPECT_FALSE(book.check_invariants());
    std::printf("real-data differential: %llu frames, %zu live orders in %zu books compared\n",
                static_cast<unsigned long long>(r.frames), compared, groups.size());
}

}  // namespace
}  // namespace enginex::itch
