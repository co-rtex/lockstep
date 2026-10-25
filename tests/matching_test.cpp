// Matching behavior (spec §7, §11.1). Order ids equal the accepting command sequence, so in a
// fresh state the n-th accepted command creates ord-n.
#include <gtest/gtest.h>

#include "test_util.hpp"

namespace enginex {
namespace {

using test::accept;
using test::buy;
using test::Exec;
using test::execs;
using test::Resting;
using test::resting;
using test::sell;

TEST(Matching, EmptyBookRestsBuy) {
    ExchangeStateMachine s;
    const auto r = accept(s, buy("r1", 10000, 5));
    EXPECT_EQ(r.command_sequence, 1U);
    EXPECT_EQ(r.order_id, 1U);
    EXPECT_EQ(r.remaining, 5);
    EXPECT_EQ(r.order_status, OrderStatus::Open);
    EXPECT_TRUE(r.executions.empty());
    EXPECT_EQ(resting(s, "AAPL", Side::Buy), (std::vector<Resting>{{1, 5, 10000}}));
    EXPECT_TRUE(resting(s, "AAPL", Side::Sell).empty());
}

TEST(Matching, EmptyBookRestsSell) {
    ExchangeStateMachine s;
    const auto r = accept(s, sell("r1", 10000, 5));
    EXPECT_TRUE(r.executions.empty());
    EXPECT_EQ(resting(s, "AAPL", Side::Sell), (std::vector<Resting>{{1, 5, 10000}}));
}

TEST(Matching, NonCrossingOrdersBothRest) {
    ExchangeStateMachine s;
    accept(s, sell("r1", 10100, 5));
    const auto r = accept(s, buy("r2", 10000, 4));  // spec example 1
    EXPECT_TRUE(r.executions.empty());
    EXPECT_EQ(resting(s, "AAPL", Side::Buy), (std::vector<Resting>{{2, 4, 10000}}));
    EXPECT_EQ(resting(s, "AAPL", Side::Sell), (std::vector<Resting>{{1, 5, 10100}}));
}

TEST(Matching, ExactPriceCrossTrades) {
    ExchangeStateMachine s;
    accept(s, sell("r1", 10000, 5));
    const auto r = accept(s, buy("r2", 10000, 5));
    EXPECT_EQ(execs(r), (std::vector<Exec>{{2, 1, 10000, 5}}));
    EXPECT_EQ(r.order_status, OrderStatus::Filled);
    EXPECT_TRUE(resting(s, "AAPL", Side::Buy).empty());
    EXPECT_TRUE(resting(s, "AAPL", Side::Sell).empty());
}

TEST(Matching, BetterPriceCrossExecutesAtRestingAskPrice) {
    ExchangeStateMachine s;
    accept(s, sell("r1", 19900, 10));
    const auto r = accept(s, buy("r2", 20000, 10));
    EXPECT_EQ(execs(r), (std::vector<Exec>{{2, 1, 19900, 10}}));
}

TEST(Matching, BetterPriceCrossExecutesAtRestingBidPrice) {
    ExchangeStateMachine s;
    accept(s, buy("r1", 20000, 10));
    const auto r = accept(s, sell("r2", 19900, 10));
    EXPECT_EQ(execs(r), (std::vector<Exec>{{2, 1, 20000, 10}}));
}

TEST(Matching, PartialIncomingRestsRemainder) {
    ExchangeStateMachine s;
    accept(s, sell("r1", 10000, 4));
    const auto r = accept(s, buy("r2", 10100, 10));  // spec example 3
    EXPECT_EQ(execs(r), (std::vector<Exec>{{2, 1, 10000, 4}}));
    EXPECT_EQ(r.remaining, 6);
    EXPECT_EQ(r.order_status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(resting(s, "AAPL", Side::Buy), (std::vector<Resting>{{2, 6, 10100}}));
    EXPECT_TRUE(resting(s, "AAPL", Side::Sell).empty());
}

TEST(Matching, PartialRestingKeepsRemainder) {
    ExchangeStateMachine s;
    accept(s, sell("r1", 10000, 10));
    const auto r = accept(s, buy("r2", 10000, 3));
    EXPECT_EQ(execs(r), (std::vector<Exec>{{2, 1, 10000, 3}}));
    EXPECT_EQ(r.remaining, 0);
    EXPECT_EQ(resting(s, "AAPL", Side::Sell), (std::vector<Resting>{{1, 7, 10000}}));
    EXPECT_EQ(s.command(1).status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(s.command(2).status, OrderStatus::Filled);
}

TEST(Matching, MultiLevelSweepBestBidFirst) {
    // Spec example 5: price beats time even though the best bid is the newest order.
    ExchangeStateMachine s;
    accept(s, buy("b1", 10000, 2));
    accept(s, buy("b2", 10100, 3));
    accept(s, buy("b3", 10200, 4));
    const auto r = accept(s, sell("s1", 10000, 9));
    EXPECT_EQ(execs(r), (std::vector<Exec>{{4, 3, 10200, 4}, {4, 2, 10100, 3}, {4, 1, 10000, 2}}));
    EXPECT_TRUE(resting(s, "AAPL", Side::Buy).empty());
    EXPECT_TRUE(resting(s, "AAPL", Side::Sell).empty());
}

TEST(Matching, SweepStopsAtNonMarketableLevel) {
    // Spec example 7.
    ExchangeStateMachine s;
    accept(s, buy("b1", 10200, 3));
    accept(s, buy("b2", 10100, 4));
    accept(s, buy("b3", 10000, 5));
    const auto r = accept(s, sell("s1", 10100, 10));
    EXPECT_EQ(execs(r), (std::vector<Exec>{{4, 1, 10200, 3}, {4, 2, 10100, 4}}));
    EXPECT_EQ(r.remaining, 3);
    EXPECT_EQ(resting(s, "AAPL", Side::Buy), (std::vector<Resting>{{3, 5, 10000}}));
    EXPECT_EQ(resting(s, "AAPL", Side::Sell), (std::vector<Resting>{{4, 3, 10100}}));
}

TEST(Matching, FifoAcrossThreeSamePriceOrders) {
    // Spec example 4.
    ExchangeStateMachine s;
    accept(s, sell("s1", 10000, 4));
    accept(s, sell("s2", 10000, 6));
    accept(s, sell("s3", 10000, 2));
    const auto r = accept(s, buy("b1", 10000, 7));
    EXPECT_EQ(execs(r), (std::vector<Exec>{{4, 1, 10000, 4}, {4, 2, 10000, 3}}));
    EXPECT_EQ(resting(s, "AAPL", Side::Sell), (std::vector<Resting>{{2, 3, 10000}, {3, 2, 10000}}));
}

TEST(Matching, PartiallyFilledRestingRetainsPriority) {
    ExchangeStateMachine s;
    accept(s, sell("s1", 10000, 4));
    accept(s, sell("s2", 10000, 6));
    accept(s, sell("s3", 10000, 2));
    accept(s, buy("b1", 10000, 7));   // s2 left with 3
    accept(s, sell("s4", 10000, 5));  // newer order behind s2 and s3
    const auto r = accept(s, buy("b2", 10000, 4));
    EXPECT_EQ(execs(r), (std::vector<Exec>{{6, 2, 10000, 3}, {6, 3, 10000, 1}}));
    EXPECT_EQ(resting(s, "AAPL", Side::Sell), (std::vector<Resting>{{3, 1, 10000}, {5, 5, 10000}}));
}

TEST(Matching, BetterPriceBeatsOlderOrder) {
    // Spec example 6: the newer, cheaper ask trades first.
    ExchangeStateMachine s;
    accept(s, sell("s1", 10000, 8));
    accept(s, sell("s2", 9900, 3));
    const auto r = accept(s, buy("b1", 10100, 7));
    EXPECT_EQ(execs(r), (std::vector<Exec>{{3, 2, 9900, 3}, {3, 1, 10000, 4}}));
    EXPECT_EQ(resting(s, "AAPL", Side::Sell), (std::vector<Resting>{{1, 4, 10000}}));
}

TEST(Matching, RemainderRestsBehindEarlierOrdersAtSamePrice) {
    ExchangeStateMachine s;
    accept(s, buy("b1", 9800, 1));
    accept(s, sell("s1", 9900, 2));
    accept(s, buy("b2", 9900, 5));  // trades 2 with s1, rests 3 at 9900
    EXPECT_EQ(resting(s, "AAPL", Side::Buy), (std::vector<Resting>{{3, 3, 9900}, {1, 1, 9800}}));
    accept(s, buy("b3", 9900, 1));
    EXPECT_EQ(resting(s, "AAPL", Side::Buy),
              (std::vector<Resting>{{3, 3, 9900}, {4, 1, 9900}, {1, 1, 9800}}));
}

TEST(Matching, SymbolsAreIndependent) {
    ExchangeStateMachine s;
    accept(s, sell("a1", 10000, 5, "AAPL"));
    const auto r = accept(s, buy("m1", 20000, 5, "MSFT"));
    EXPECT_TRUE(r.executions.empty());
    EXPECT_EQ(resting(s, "AAPL", Side::Sell), (std::vector<Resting>{{1, 5, 10000}}));
    EXPECT_EQ(resting(s, "MSFT", Side::Buy), (std::vector<Resting>{{2, 5, 20000}}));
    const auto r2 = accept(s, buy("a2", 10000, 5, "AAPL"));
    EXPECT_EQ(execs(r2), (std::vector<Exec>{{3, 1, 10000, 5}}));
    EXPECT_EQ(r2.executions.at(0).symbol, *s.find_symbol("AAPL"));
    EXPECT_EQ(resting(s, "MSFT", Side::Buy), (std::vector<Resting>{{2, 5, 20000}}));
}

TEST(Matching, TopNAggregationAndSorting) {
    ExchangeStateMachine s;
    accept(s, buy("b1", 10000, 1));
    accept(s, buy("b2", 10200, 2));
    accept(s, buy("b3", 10100, 3));
    accept(s, buy("b4", 10200, 4));
    accept(s, sell("s1", 10500, 5));
    accept(s, sell("s2", 10300, 6));
    accept(s, sell("s3", 10300, 7));
    const auto sym = *s.find_symbol("AAPL");

    const BookSnapshot full = s.engine().snapshot(sym, 0);
    EXPECT_EQ(full.bids, (std::vector<BookLevel>{{10200, 6, 2}, {10100, 3, 1}, {10000, 1, 1}}));
    EXPECT_EQ(full.asks, (std::vector<BookLevel>{{10300, 13, 2}, {10500, 5, 1}}));

    const BookSnapshot top2 = s.engine().snapshot(sym, 2);
    EXPECT_EQ(top2.bids, (std::vector<BookLevel>{{10200, 6, 2}, {10100, 3, 1}}));
    EXPECT_EQ(top2.asks, (std::vector<BookLevel>{{10300, 13, 2}, {10500, 5, 1}}));

    const BookSnapshot top1 = s.engine().snapshot(sym, 1);
    EXPECT_EQ(top1.bids, (std::vector<BookLevel>{{10200, 6, 2}}));
    EXPECT_EQ(top1.asks, (std::vector<BookLevel>{{10300, 13, 2}}));
}

TEST(Matching, ExecutionSequencesAreGlobalAndMonotonic) {
    ExchangeStateMachine s;
    accept(s, sell("s1", 10000, 1));
    accept(s, sell("s2", 10000, 1, "MSFT"));
    const auto r1 = accept(s, buy("b1", 10000, 1));
    const auto r2 = accept(s, buy("b2", 10000, 1, "MSFT"));
    ASSERT_EQ(r1.executions.size(), 1U);
    ASSERT_EQ(r2.executions.size(), 1U);
    EXPECT_EQ(r1.executions[0].execution_sequence, 1U);
    EXPECT_EQ(r1.executions[0].command_sequence, 3U);
    EXPECT_EQ(r2.executions[0].execution_sequence, 2U);
    EXPECT_EQ(r2.executions[0].command_sequence, 4U);
    EXPECT_EQ(s.last_execution_sequence(), 2U);
}

TEST(Matching, LargeQuantitiesAggregateWithoutOverflow) {
    StateConfig cfg;
    ExchangeStateMachine s(cfg);
    for (int i = 0; i < 100; ++i) {
        accept(s, sell("s" + std::to_string(i), kMaxPrice, kMaxQuantity));
    }
    const auto snap = s.engine().snapshot(*s.find_symbol("AAPL"), 0);
    ASSERT_EQ(snap.asks.size(), 1U);
    EXPECT_EQ(snap.asks[0].aggregate_quantity, 100 * kMaxQuantity);
    EXPECT_EQ(snap.asks[0].resting_order_count, 100U);
    const auto r = accept(s, buy("b", kMaxPrice, kMaxQuantity));
    EXPECT_EQ(r.executions.size(), 1U);
}

}  // namespace
}  // namespace enginex
