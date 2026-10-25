// Cancellation (spec §6.1, §11.1): queues, totals and the order index stay consistent.
#include <gtest/gtest.h>

#include "test_util.hpp"

namespace enginex {
namespace {

using test::accept;
using test::buy;
using test::cancel;
using test::Exec;
using test::execs;
using test::reject;
using test::Resting;
using test::resting;
using test::sell;

class CancelTest : public ::testing::Test {
protected:
    void SetUp() override {
        accept(s, sell("s1", 10000, 1));  // ord-1
        accept(s, sell("s2", 10000, 2));  // ord-2
        accept(s, sell("s3", 10000, 3));  // ord-3
    }
    ExchangeStateMachine s;
};

TEST_F(CancelTest, CancelFirst) {
    const auto r = accept(s, cancel("c1", 1));
    EXPECT_EQ(r.type, CommandType::Cancel);
    EXPECT_EQ(r.command_sequence, 4U);
    EXPECT_EQ(r.order_id, 1U);
    EXPECT_EQ(r.cancelled_quantity, 1);
    EXPECT_EQ(r.order_status, OrderStatus::Cancelled);
    EXPECT_EQ(resting(s, "AAPL", Side::Sell), (std::vector<Resting>{{2, 2, 10000}, {3, 3, 10000}}));
    EXPECT_EQ(s.engine().snapshot(0, 0).asks, (std::vector<BookLevel>{{10000, 5, 2}}));
}

TEST_F(CancelTest, CancelMiddle) {
    accept(s, cancel("c1", 2));
    EXPECT_EQ(resting(s, "AAPL", Side::Sell), (std::vector<Resting>{{1, 1, 10000}, {3, 3, 10000}}));
    EXPECT_EQ(s.engine().snapshot(0, 0).asks, (std::vector<BookLevel>{{10000, 4, 2}}));
    // Priority of remaining orders is unchanged.
    const auto r = accept(s, buy("b1", 10000, 2));
    EXPECT_EQ(execs(r), (std::vector<Exec>{{5, 1, 10000, 1}, {5, 3, 10000, 1}}));
}

TEST_F(CancelTest, CancelLast) {
    accept(s, cancel("c1", 3));
    EXPECT_EQ(resting(s, "AAPL", Side::Sell), (std::vector<Resting>{{1, 1, 10000}, {2, 2, 10000}}));
    // A new order at the same price goes behind the remaining ones.
    accept(s, sell("s4", 10000, 4));
    EXPECT_EQ(resting(s, "AAPL", Side::Sell),
              (std::vector<Resting>{{1, 1, 10000}, {2, 2, 10000}, {5, 4, 10000}}));
}

TEST_F(CancelTest, CancelAllRemovesLevel) {
    accept(s, cancel("c1", 2));
    accept(s, cancel("c2", 1));
    accept(s, cancel("c3", 3));
    EXPECT_TRUE(resting(s, "AAPL", Side::Sell).empty());
    EXPECT_EQ(s.engine().book(0).level_count(Side::Sell), 0U);
    EXPECT_EQ(s.engine().open_order_count(), 0U);
}

TEST(Cancel, OnlyOrderAtLevelRemovesLevelButKeepsOthers) {
    ExchangeStateMachine s;
    accept(s, buy("b1", 10000, 1));
    accept(s, buy("b2", 10100, 1));
    accept(s, buy("b3", 9900, 1));
    accept(s, cancel("c1", 2));
    EXPECT_EQ(s.engine().snapshot(0, 0).bids,
              (std::vector<BookLevel>{{10000, 1, 1}, {9900, 1, 1}}));
    EXPECT_EQ(s.engine().book(0).best_price(Side::Buy), 10000);
}

TEST(Cancel, PartiallyFilledOrderCancelsRemainder) {
    ExchangeStateMachine s;
    accept(s, sell("s1", 10000, 10));
    accept(s, buy("b1", 10000, 4));
    const auto r = accept(s, cancel("c1", 1));
    EXPECT_EQ(r.cancelled_quantity, 6);
    EXPECT_EQ(r.quantity, 10);
    EXPECT_EQ(s.command(1).status, OrderStatus::Cancelled);
}

TEST(Cancel, UnknownOrderRejected) {
    ExchangeStateMachine s;
    accept(s, buy("b1", 10000, 1));
    reject(s, cancel("c1", 99), ErrorCode::UnknownOrder);
    reject(s, cancel("c2", "ord-0"), ErrorCode::UnknownOrder);
    reject(s, cancel("c3", "garbage"), ErrorCode::UnknownOrder);
    reject(s, cancel("c4", ""), ErrorCode::UnknownOrder);
    EXPECT_EQ(s.last_command_sequence(), 1U);
}

TEST(Cancel, CancelCommandSequenceIsNotAnOrder) {
    ExchangeStateMachine s;
    accept(s, buy("b1", 10000, 1));
    accept(s, buy("b2", 10000, 1));
    accept(s, cancel("c1", 1));  // sequence 3 is a cancel, not an order
    reject(s, cancel("c2", 3), ErrorCode::UnknownOrder);
}

TEST(Cancel, FilledOrderRejected) {
    ExchangeStateMachine s;
    accept(s, sell("s1", 10000, 5));
    accept(s, buy("b1", 10000, 5));
    reject(s, cancel("c1", 1), ErrorCode::OrderNotOpen);
    reject(s, cancel("c2", 2), ErrorCode::OrderNotOpen);  // incoming fully filled, never rested
}

TEST(Cancel, AlreadyCancelledRejectedButRetryReturnsOriginal) {
    ExchangeStateMachine s;
    accept(s, sell("s1", 10000, 5));
    const auto first = accept(s, cancel("c1", 1));
    // A fresh request id for the same target fails without changing state.
    reject(s, cancel("c2", 1), ErrorCode::OrderNotOpen);
    // A retry of the accepted request id returns the original result, not a second cancel.
    const auto retry = s.process(cancel("c1", 1));
    EXPECT_TRUE(retry.accepted);
    EXPECT_TRUE(retry.duplicate);
    EXPECT_EQ(retry.command_sequence, first.command_sequence);
    EXPECT_EQ(retry.cancelled_quantity, 5);
    EXPECT_EQ(s.last_command_sequence(), 2U);
}

TEST(Cancel, CancelledOrderNeverTrades) {
    ExchangeStateMachine s;
    accept(s, sell("s1", 10000, 5));
    accept(s, sell("s2", 10100, 5));
    accept(s, cancel("c1", 1));
    const auto r = accept(s, buy("b1", 10100, 5));
    EXPECT_EQ(execs(r), (std::vector<Exec>{{4, 2, 10100, 5}}));
}

TEST(Cancel, RepeatedInsertCancelLeavesNoDanglingState) {
    ExchangeStateMachine s;
    for (int round = 0; round < 50; ++round) {
        const auto a = accept(s, buy("a" + std::to_string(round), 10000 + round % 3, 2));
        const auto b = accept(s, buy("b" + std::to_string(round), 10000 + round % 3, 2));
        accept(s, cancel("ca" + std::to_string(round), a.order_id));
        if (round % 2 == 0) {
            accept(s, cancel("cb" + std::to_string(round), b.order_id));
        }
    }
    EXPECT_EQ(s.engine().open_order_count(), 25U);
    const auto r = accept(s, sell("sweep", 9000, 1000));
    EXPECT_EQ(r.executions.size(), 25U);
    EXPECT_EQ(r.remaining, 1000 - 50);
    EXPECT_EQ(s.engine().open_order_count(), 1U);
}

}  // namespace
}  // namespace enginex
