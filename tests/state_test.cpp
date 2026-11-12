// Validation, deterministic sequencing and request-id idempotency (spec §3.3, §6.1, §6.2).
#include <gtest/gtest.h>

#include <limits>

#include "test_util.hpp"

namespace enginex {
namespace {

using test::accept;
using test::buy;
using test::cancel;
using test::order;
using test::reject;
using test::sell;

TEST(Validation, RejectsInvalidSymbol) {
    ExchangeStateMachine s;
    reject(s, buy("r1", 10000, 1, ""), ErrorCode::InvalidSymbol);
    reject(s, buy("r2", 10000, 1, "aapl"), ErrorCode::InvalidSymbol);
    reject(s, buy("r3", 10000, 1, "GOOG"), ErrorCode::InvalidSymbol);
    reject(s, buy("r4", 10000, 1, "AAPL "), ErrorCode::InvalidSymbol);
}

TEST(Validation, RejectsInvalidSide) {
    ExchangeStateMachine s;
    reject(s, order("r1", "AAPL", static_cast<Side>(7), 10000, 1), ErrorCode::InvalidSide);
}

TEST(Validation, RejectsInvalidPrice) {
    ExchangeStateMachine s;
    for (PriceCents p :
         {PriceCents{0}, PriceCents{-1}, kMaxPrice + 1, std::numeric_limits<PriceCents>::max(),
          std::numeric_limits<PriceCents>::min()}) {
        reject(s, buy("r", p, 1), ErrorCode::InvalidPrice);
    }
    accept(s, buy("lo", kMinPrice, 1));
    accept(s, sell("hi", kMaxPrice, 1));
}

TEST(Validation, RejectsInvalidQuantity) {
    ExchangeStateMachine s;
    for (Quantity q :
         {Quantity{0}, Quantity{-5}, kMaxQuantity + 1, std::numeric_limits<Quantity>::max(),
          std::numeric_limits<Quantity>::min()}) {
        reject(s, buy("r", 10000, q), ErrorCode::InvalidQuantity);
    }
    accept(s, buy("max", 10000, kMaxQuantity));
}

TEST(Validation, RejectsInvalidRequestId) {
    ExchangeStateMachine s;
    reject(s, buy("", 10000, 1), ErrorCode::InvalidRequest);
    reject(s, buy(std::string(65, 'x'), 10000, 1), ErrorCode::InvalidRequest);
    reject(s, buy("bad\xFF", 10000, 1), ErrorCode::InvalidRequest);
    reject(s, cancel("", 1), ErrorCode::InvalidRequest);
    accept(s, buy(std::string(64, 'x'), 10000, 1));
    accept(s, buy("caf\xC3\xA9", 10000, 1));
}

TEST(Validation, RejectionOrderPrefersRequestThenSymbolThenFields) {
    ExchangeStateMachine s;
    reject(s, order("", "BAD", static_cast<Side>(9), 0, 0), ErrorCode::InvalidRequest);
    reject(s, order("r", "BAD", static_cast<Side>(9), 0, 0), ErrorCode::InvalidSymbol);
    reject(s, order("r", "AAPL", static_cast<Side>(9), 0, 0), ErrorCode::InvalidSide);
    reject(s, order("r", "AAPL", Side::Buy, 0, 0), ErrorCode::InvalidPrice);
    reject(s, order("r", "AAPL", Side::Buy, 1, 0), ErrorCode::InvalidQuantity);
}

TEST(Sequencing, RejectedCommandsDoNotConsumeSequence) {
    ExchangeStateMachine s;
    EXPECT_EQ(accept(s, buy("r1", 10000, 1)).command_sequence, 1U);
    reject(s, buy("r2", 0, 1), ErrorCode::InvalidPrice);
    reject(s, cancel("r3", 42), ErrorCode::UnknownOrder);
    const auto r = accept(s, buy("r4", 10000, 1));
    EXPECT_EQ(r.command_sequence, 2U);
    EXPECT_EQ(r.order_id, 2U);
    EXPECT_EQ(r.state_watermark, 2U);
}

TEST(Sequencing, CancelsConsumeSequenceNumbers) {
    ExchangeStateMachine s;
    accept(s, buy("r1", 10000, 1));
    EXPECT_EQ(accept(s, cancel("c1", 1)).command_sequence, 2U);
    EXPECT_EQ(accept(s, buy("r2", 10000, 1)).order_id, 3U);
}

TEST(Sequencing, RejectedRequestIdCanBeReusedLater) {
    // Only accepted requests are remembered (DECISIONS.md D4).
    ExchangeStateMachine s;
    reject(s, cancel("c1", 1), ErrorCode::UnknownOrder);
    accept(s, buy("b1", 10000, 1));
    const auto r = accept(s, cancel("c1", 1));
    EXPECT_FALSE(r.duplicate);
    EXPECT_EQ(r.command_sequence, 2U);
}

TEST(Idempotency, RetrySameNewOrderReturnsOriginalResult) {
    ExchangeStateMachine s;
    accept(s, sell("s1", 10000, 3));
    const auto first = accept(s, buy("b1", 10100, 5));
    accept(s, sell("s2", 10100, 2));  // fills more of b1 after the original response

    const std::string before = canonical_state(s);
    const auto retry = s.process(buy("b1", 10100, 5));
    EXPECT_EQ(canonical_state(s), before);
    EXPECT_TRUE(retry.accepted);
    EXPECT_TRUE(retry.duplicate);
    EXPECT_EQ(retry.command_sequence, first.command_sequence);
    EXPECT_EQ(retry.order_id, first.order_id);
    EXPECT_EQ(retry.remaining, first.remaining);  // the original response, not current state
    EXPECT_EQ(retry.order_status, first.order_status);
    EXPECT_EQ(retry.executions, first.executions);
    EXPECT_EQ(retry.state_watermark, 3U);
    EXPECT_EQ(s.last_command_sequence(), 3U);
    EXPECT_EQ(s.last_execution_sequence(), 2U);
}

TEST(Idempotency, ReuseWithDifferentBodyIsConflict) {
    ExchangeStateMachine s;
    accept(s, buy("b1", 10000, 5));
    reject(s, buy("b1", 10000, 6), ErrorCode::DuplicateRequestIdConflict);
    reject(s, sell("b1", 10000, 5), ErrorCode::DuplicateRequestIdConflict);
    reject(s, buy("b1", 10000, 5, "MSFT"), ErrorCode::DuplicateRequestIdConflict);
    reject(s, cancel("b1", 1), ErrorCode::DuplicateRequestIdConflict);
    // Conflict detection precedes field validation.
    reject(s, buy("b1", 0, 0), ErrorCode::DuplicateRequestIdConflict);
}

TEST(Idempotency, RetryCancelReturnsOriginal) {
    ExchangeStateMachine s;
    accept(s, buy("b1", 10000, 5));
    const auto first = accept(s, cancel("c1", 1));
    const auto retry = s.process(cancel("c1", 1));
    EXPECT_TRUE(retry.duplicate);
    EXPECT_EQ(retry.command_sequence, first.command_sequence);
    EXPECT_EQ(retry.cancelled_quantity, first.cancelled_quantity);
    reject(s, cancel("c1", "ord-2"), ErrorCode::DuplicateRequestIdConflict);
}

TEST(Capacity, OpenOrderLimitRejectsAtomically) {
    StateConfig cfg;
    cfg.max_open_orders = 2;
    ExchangeStateMachine s(cfg);
    accept(s, buy("b1", 10000, 1));
    accept(s, buy("b2", 10000, 1));
    reject(s, sell("s1", 10000, 1), ErrorCode::LimitExceeded);
    accept(s, cancel("c1", 1));
    accept(s, sell("s2", 10000, 1));  // trades with b2
    EXPECT_EQ(s.engine().open_order_count(), 0U);
}

TEST(Config, CustomSymbolCatalog) {
    StateConfig cfg;
    cfg.symbols = {"AAA", "BBB", "CCC"};
    ExchangeStateMachine s(cfg);
    accept(s, buy("r1", 100, 1, "CCC"));
    reject(s, buy("r2", 100, 1, "AAPL"), ErrorCode::InvalidSymbol);
    EXPECT_THROW(ExchangeStateMachine(StateConfig{{"A", "A"}, 10}), std::invalid_argument);
    EXPECT_THROW(ExchangeStateMachine(StateConfig{{}, 10}), std::invalid_argument);
}

TEST(Commit, RejectsStaleDecision) {
    ExchangeStateMachine s;
    const Command c1 = buy("b1", 10000, 1);
    const Command c2 = buy("b2", 10000, 1);
    const Decision d1 = s.prepare(c1);
    const Decision d2 = s.prepare(c2);  // also sequence 1
    CommandResult out;
    s.commit(c1, d1, out);
    EXPECT_THROW(s.commit(c2, d2, out), InvariantViolation);
}

TEST(Commit, PrepareDoesNotMutate) {
    ExchangeStateMachine s;
    accept(s, sell("s1", 10000, 5));
    const std::string before = canonical_state(s);
    const Decision d = s.prepare(buy("b1", 10000, 5));
    EXPECT_TRUE(d.accepted());
    EXPECT_EQ(d.sequence, 2U);
    EXPECT_EQ(canonical_state(s), before);
}

}  // namespace
}  // namespace enginex
