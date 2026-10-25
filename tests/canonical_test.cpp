// Canonical state serialization and hashing (spec §8.2, §7.2).
#include <gtest/gtest.h>

#include "enginex/sha256.hpp"
#include "test_util.hpp"

namespace enginex {
namespace {

using test::accept;
using test::buy;
using test::cancel;
using test::sell;

void run_spec_example(ExchangeStateMachine& s) {
    accept(s, buy("req-001", 10100, 10));
    accept(s, sell("req-002", 10000, 4));
    accept(s, sell("req-003", 10100, 2));
}

TEST(Canonical, SpecExampleExactBytes) {
    ExchangeStateMachine s;
    run_spec_example(s);
    const std::string expected =
        R"({"format":"enginex.canonical.v1","symbols":["AAPL","MSFT"],)"
        R"("last_command_sequence":3,"last_execution_sequence":2,)"
        R"("books":[{"symbol":"AAPL","bids":[{"price":10100,"aggregate":4,"orders":[)"
        R"({"order_id":"ord-1","original":10,"remaining":4}]}],"asks":[]},)"
        R"({"symbol":"MSFT","bids":[],"asks":[]}],)"
        R"("commands":[)"
        R"({"command_sequence":1,"request_id":"req-001","type":"new_limit","symbol":"AAPL",)"
        R"("order_id":"ord-1","side":"BUY","price":10100,"quantity":10,)"
        R"("remaining_after_command":10,"status":"PARTIALLY_FILLED",)"
        R"("first_execution_sequence":0,"execution_count":0},)"
        R"({"command_sequence":2,"request_id":"req-002","type":"new_limit","symbol":"AAPL",)"
        R"("order_id":"ord-2","side":"SELL","price":10000,"quantity":4,)"
        R"("remaining_after_command":0,"status":"FILLED",)"
        R"("first_execution_sequence":1,"execution_count":1},)"
        R"({"command_sequence":3,"request_id":"req-003","type":"new_limit","symbol":"AAPL",)"
        R"("order_id":"ord-3","side":"SELL","price":10100,"quantity":2,)"
        R"("remaining_after_command":0,"status":"FILLED",)"
        R"("first_execution_sequence":2,"execution_count":1}],)"
        R"("executions":[)"
        R"({"execution_id":"exe-1","execution_sequence":1,"command_sequence":2,"symbol":"AAPL",)"
        R"("incoming_order_id":"ord-2","resting_order_id":"ord-1","incoming_side":"SELL",)"
        R"("price":10100,"quantity":4},)"
        R"({"execution_id":"exe-2","execution_sequence":2,"command_sequence":3,"symbol":"AAPL",)"
        R"("incoming_order_id":"ord-3","resting_order_id":"ord-1","incoming_side":"SELL",)"
        R"("price":10100,"quantity":2}]})";
    EXPECT_EQ(canonical_state(s), expected);
    EXPECT_EQ(state_hash(s), Sha256::hex_digest(expected));
}

TEST(Canonical, RepeatedRunsProduceIdenticalBytes) {
    std::string first;
    for (int run = 0; run < 5; ++run) {
        ExchangeStateMachine s;
        run_spec_example(s);
        accept(s, buy("b-x", 9000, 7, "MSFT"));
        accept(s, buy("b-y", 9100, 3, "MSFT"));
        accept(s, cancel("c-x", 4));
        const std::string bytes = canonical_state(s);
        if (run == 0) {
            first = bytes;
        }
        EXPECT_EQ(bytes, first);
    }
}

TEST(Canonical, StreamingHashMatchesStringHashForLargeState) {
    ExchangeStateMachine s;
    for (int i = 0; i < 3000; ++i) {
        accept(s, buy("b" + std::to_string(i), 1000 + i % 50, 1 + i % 7));
    }
    EXPECT_EQ(state_hash(s), Sha256::hex_digest(canonical_state(s)));
}

TEST(Canonical, DifferentFifoOrderChangesHash) {
    ExchangeStateMachine a;
    accept(a, buy("x", 10000, 1));
    accept(a, buy("y", 10000, 2));
    ExchangeStateMachine b;
    accept(b, buy("x", 10000, 2));
    accept(b, buy("y", 10000, 1));
    EXPECT_NE(state_hash(a), state_hash(b));
}

TEST(Canonical, EscapesStrings) {
    ExchangeStateMachine s;
    accept(s, buy("q\"\\\n\x01", 10000, 1));
    const std::string bytes = canonical_state(s);
    EXPECT_NE(bytes.find(R"("request_id":"q\"\\\n\u0001")"), std::string::npos) << bytes;
}

TEST(Canonical, RejectedAndDuplicateCommandsDoNotChangeHash) {
    ExchangeStateMachine s;
    run_spec_example(s);
    const std::string h = state_hash(s);
    (void)s.process(buy("req-001", 10100, 10));  // duplicate
    (void)s.process(buy("req-bad", 0, 10));      // invalid
    (void)s.process(cancel("req-cx", 2));        // filled order
    EXPECT_EQ(state_hash(s), h);
}

}  // namespace
}  // namespace enginex
