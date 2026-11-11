// JSON boundary parsing and serialization (spec §6.2, §8.1).
#include <gtest/gtest.h>

#include <fstream>
#include <limits>
#include <sstream>

#include "enginex/json_codec.hpp"
#include "enginex/simulate.hpp"

namespace enginex {
namespace {

using codec::Json;
using codec::ParseError;

ErrorCode parse_error_code_of(std::string_view text) {
    auto parsed = codec::parse_input(text);
    if (const auto* err = std::get_if<ParseError>(&parsed)) {
        return err->code;
    }
    return ErrorCode::None;
}

TEST(JsonCodec, ParsesNewLimit) {
    auto parsed = codec::parse_input(
        R"({"type":"new_limit","request_id":"r1","symbol":"AAPL","side":"SELL","price":10000,"quantity":4})");
    const auto& input = std::get<codec::Input>(parsed);
    const auto& order = std::get<NewLimitOrder>(std::get<Command>(input));
    EXPECT_EQ(order, (NewLimitOrder{"r1", "AAPL", Side::Sell, 10000, 4}));
}

TEST(JsonCodec, ParsesCancelAndQueries) {
    auto c = codec::parse_input(R"({"type":"cancel","request_id":"c1","order_id":"ord-7"})");
    EXPECT_EQ(std::get<CancelOrder>(std::get<Command>(std::get<codec::Input>(c))),
              (CancelOrder{"c1", "ord-7"}));
    auto s = codec::parse_input(R"({"type":"snapshot","symbol":"MSFT"})");
    const auto& snap = std::get<codec::SnapshotQuery>(std::get<codec::Input>(s));
    EXPECT_EQ(snap.symbol, "MSFT");
    EXPECT_EQ(snap.depth, codec::kDefaultSnapshotDepth);
    auto e = codec::parse_input(R"({"type":"executions","symbol":"AAPL"})");
    EXPECT_EQ(std::get<codec::ExecutionsQuery>(std::get<codec::Input>(e)).symbol, "AAPL");
}

TEST(JsonCodec, FieldErrorsMapToSpecificCodes) {
    const std::string base = R"("type":"new_limit","request_id":"r")";
    auto with = [&](const std::string& fields) { return "{" + base + "," + fields + "}"; };
    EXPECT_EQ(parse_error_code_of(with(R"("side":"BUY","price":1,"quantity":1)")),
              ErrorCode::InvalidSymbol);
    EXPECT_EQ(parse_error_code_of(with(R"("symbol":7,"side":"BUY","price":1,"quantity":1)")),
              ErrorCode::InvalidSymbol);
    EXPECT_EQ(parse_error_code_of(with(R"("symbol":"AAPL","price":1,"quantity":1)")),
              ErrorCode::InvalidSide);
    EXPECT_EQ(parse_error_code_of(with(R"("symbol":"AAPL","side":"buy","price":1,"quantity":1)")),
              ErrorCode::InvalidSide);
    EXPECT_EQ(parse_error_code_of(with(R"("symbol":"AAPL","side":"BUY","quantity":1)")),
              ErrorCode::InvalidPrice);
    EXPECT_EQ(
        parse_error_code_of(with(R"("symbol":"AAPL","side":"BUY","price":100.0,"quantity":1)")),
        ErrorCode::InvalidPrice);
    EXPECT_EQ(
        parse_error_code_of(with(R"("symbol":"AAPL","side":"BUY","price":"100","quantity":1)")),
        ErrorCode::InvalidPrice);
    EXPECT_EQ(
        parse_error_code_of(with(R"("symbol":"AAPL","side":"BUY","price":true,"quantity":1)")),
        ErrorCode::InvalidPrice);
    EXPECT_EQ(parse_error_code_of(with(
                  R"("symbol":"AAPL","side":"BUY","price":99999999999999999999,"quantity":1)")),
              ErrorCode::InvalidPrice);
    EXPECT_EQ(parse_error_code_of(with(R"("symbol":"AAPL","side":"BUY","price":1,"quantity":1.5)")),
              ErrorCode::InvalidQuantity);
    EXPECT_EQ(parse_error_code_of(with(R"("symbol":"AAPL","side":"BUY","price":1)")),
              ErrorCode::InvalidQuantity);
}

TEST(JsonCodec, StructuralErrorsAreInvalidRequest) {
    for (
        const char* text :
        {"", "{", "[]", "42", "null", R"({"type":"new_limit"})", R"({"request_id":"r"})",
         R"({"type":"bogus","request_id":"r"})", R"({"type":7})",
         R"({"type":"new_limit","request_id":5,"symbol":"AAPL","side":"BUY","price":1,"quantity":1})",
         R"({"type":"new_limit","request_id":"r","symbol":"AAPL","side":"BUY","price":1,"quantity":1,"extra":1})",
         R"({"type":"cancel","request_id":"r"})",
         R"({"type":"cancel","request_id":"r","order_id":5})",
         R"({"type":"snapshot","symbol":"AAPL","depth":-1})",
         R"({"type":"snapshot","symbol":"AAPL","depth":1001})"}) {
        EXPECT_EQ(parse_error_code_of(text), ErrorCode::InvalidRequest) << text;
    }
}

TEST(JsonCodec, NegativeAndHugeIntegersReachStateValidation) {
    // Exact integers are passed through; range checks belong to the state machine.
    auto parsed = codec::parse_input(
        R"({"type":"new_limit","request_id":"r","symbol":"AAPL","side":"BUY","price":-5,"quantity":9223372036854775807})");
    const auto& order = std::get<NewLimitOrder>(std::get<Command>(std::get<codec::Input>(parsed)));
    EXPECT_EQ(order.price, -5);
    EXPECT_EQ(order.quantity, std::numeric_limits<Quantity>::max());
    ExchangeStateMachine s;
    EXPECT_EQ(s.process(Command{order}).error, ErrorCode::InvalidPrice);
}

// A repeated key makes a command ambiguous: a reader that keeps the first value and one that
// keeps the last would disagree, e.g. about the idempotency key. The parser used to keep the
// last value silently and accept the command.
TEST(JsonCodec, RepeatedKeysAreRejected) {
    for (const char* text :
         {R"({"type":"new_limit","request_id":"r","request_id":"s","symbol":"AAPL","side":"BUY","price":5,"quantity":1})",
          R"({"type":"new_limit","request_id":"r","symbol":"AAPL","side":"BUY","price":5,"price":6,"quantity":1})",
          R"({"type":"cancel","request_id":"c","order_id":"ord-1","type":"cancel"})"}) {
        EXPECT_EQ(parse_error_code_of(text), ErrorCode::InvalidRequest) << text;
    }
    // The same key in different objects is not a repeat.
    EXPECT_TRUE(std::holds_alternative<codec::Input>(codec::parse_input(
        R"({"type":"new_limit","request_id":"r","symbol":"AAPL","side":"BUY","price":5,"quantity":1})")));
}

// ordered_json inserts keys by linear scan, so one object with k keys cost O(k^2) to parse
// (40 000 keys: 1.4 s). Objects are limited to kMaxKeysPerObject keys, at any depth.
TEST(JsonCodec, ObjectsWithTooManyKeysAreRejected) {
    auto object_with = [](std::size_t keys) {
        std::string s = "{";
        for (std::size_t i = 0; i < keys; ++i) {
            s += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":1";
        }
        return s + "}";
    };
    auto message_of = [](const std::string& text) {
        const auto parsed = codec::parse_input(text);
        return std::holds_alternative<ParseError>(parsed) ? std::get<ParseError>(parsed).message
                                                          : std::string("accepted");
    };
    const std::size_t limit = codec::kMaxKeysPerObject;
    EXPECT_EQ(message_of(object_with(limit + 1)), "too many fields in one object");
    EXPECT_EQ(message_of(object_with(200'000)), "too many fields in one object");
    EXPECT_NE(message_of(object_with(limit)), "too many fields in one object");  // other error
    EXPECT_EQ(message_of(R"({"type":"new_limit","x":)" + object_with(limit + 1) + "}"),
              "too many fields in one object");
    codec::KeyIssue issue = codec::KeyIssue::None;
    (void)codec::parse_json(object_with(limit), issue);
    EXPECT_EQ(issue, codec::KeyIssue::None);
}

// Keys are tracked per object: each nested object, sibling and array element starts afresh,
// and its keys count neither as repeats of nor towards the limit of the enclosing object.
TEST(JsonCodec, KeyChecksArePerObjectAtEveryDepth) {
    auto issue_of = [](const std::string& text) {
        codec::KeyIssue issue = codec::KeyIssue::None;
        (void)codec::parse_json(text, issue);
        return issue;
    };
    using codec::KeyIssue;
    EXPECT_EQ(issue_of(R"({"a":{"k":1},"b":{"k":1}})"), KeyIssue::None);
    EXPECT_EQ(issue_of(R"({"a":[{"k":1},{"k":1}]})"), KeyIssue::None);
    EXPECT_EQ(issue_of(R"({"k":{"k":{"k":1}}})"), KeyIssue::None);
    EXPECT_EQ(issue_of(R"({"a":{"k":1},"a":{"k":1}})"), KeyIssue::RepeatedKey);
    EXPECT_EQ(issue_of(R"({"a":{"k":1,"k":2}})"), KeyIssue::RepeatedKey);
    EXPECT_EQ(issue_of(R"({"a":[{"x":1},{"k":1,"k":2}]})"), KeyIssue::RepeatedKey);
    EXPECT_EQ(issue_of(R"({"a":{"k":1},"k":2,"k":3})"), KeyIssue::RepeatedKey);  // after a child

    // Two sibling children with the limit each: 2 x 64 keys below one parent is fine.
    auto keys = [](std::size_t n) {
        std::string s = "{";
        for (std::size_t i = 0; i < n; ++i) {
            s += (i ? ",\"k" : "\"k") + std::to_string(i) + "\":1";
        }
        return s + "}";
    };
    const std::size_t limit = codec::kMaxKeysPerObject;
    EXPECT_EQ(issue_of(R"({"a":)" + keys(limit) + R"(,"b":)" + keys(limit) + "}"), KeyIssue::None);
    EXPECT_EQ(issue_of("[" + keys(limit) + "," + keys(limit) + "]"), KeyIssue::None);
    EXPECT_EQ(issue_of("[" + keys(limit) + "," + keys(limit + 1) + "]"), KeyIssue::TooManyKeys);
}

TEST(JsonCodec, ErrorEchoesRequestIdAndType) {
    auto parsed = codec::parse_input(
        R"({"type":"new_limit","request_id":"abc","symbol":"AAPL","side":"BUY","price":1})");
    const auto& err = std::get<ParseError>(parsed);
    const Json j = codec::to_json(err);
    EXPECT_EQ(j["request_id"], "abc");
    EXPECT_EQ(j["type"], "new_limit");
    EXPECT_EQ(j["status"], "rejected");
    EXPECT_EQ(j["error"]["code"], "INVALID_QUANTITY");
}

TEST(Simulate, SpecExampleResponses) {
    std::istringstream in(
        R"({"type":"new_limit","request_id":"req-001","symbol":"AAPL","side":"BUY","price":10100,"quantity":10}
{"type":"new_limit","request_id":"req-002","symbol":"AAPL","side":"SELL","price":10000,"quantity":4}
{"type":"new_limit","request_id":"req-003","symbol":"AAPL","side":"SELL","price":10100,"quantity":2}

{"type":"snapshot","symbol":"AAPL","depth":10}
)");
    const Json doc = run_simulation(in);
    const Json& r = doc["responses"];
    ASSERT_EQ(r.size(), 4U);  // blank line produces no response
    EXPECT_EQ(r[1]["line"], 2);
    EXPECT_EQ(r[1]["command_sequence"], 2);
    EXPECT_EQ(r[1]["order_id"], "ord-2");
    EXPECT_EQ(r[1]["remaining"], 0);
    ASSERT_EQ(r[1]["executions"].size(), 1U);
    EXPECT_EQ(r[1]["executions"][0]["execution_id"], "exe-1");
    EXPECT_EQ(r[1]["executions"][0]["price"], 10100);
    EXPECT_EQ(r[1]["executions"][0]["quantity"], 4);
    EXPECT_EQ(r[1]["executions"][0]["resting_order_id"], "ord-1");
    EXPECT_EQ(r[2]["executions"][0]["execution_id"], "exe-2");
    EXPECT_EQ(r[2]["executions"][0]["price"], 10100);
    EXPECT_EQ(r[3]["line"], 5);
    EXPECT_EQ(r[3]["as_of_command_sequence"], 3);
    EXPECT_EQ(r[3]["bids"], Json::parse(R"([{"price":10100,"quantity":4,"order_count":1}])"));
    EXPECT_EQ(r[3]["asks"], Json::array());
    EXPECT_EQ(doc["final_state"]["last_command_sequence"], 3);
}

TEST(Simulate, DemoMatchesCommittedExpectedOutput) {
    std::ifstream in(std::string(ENGINEX_SOURCE_DIR) + "/examples/demo_orders.jsonl");
    ASSERT_TRUE(in);
    std::ifstream expected_file(std::string(ENGINEX_SOURCE_DIR) +
                                "/examples/expected_demo_results.json");
    ASSERT_TRUE(expected_file);
    const Json expected = Json::parse(expected_file);
    const Json actual = run_simulation(in);
    EXPECT_EQ(actual, expected) << actual.dump(2);
    // Key facts from spec §15, asserted explicitly as well.
    EXPECT_EQ(actual["final_state"]["books"][0]["bids"],
              Json::parse(R"([{"price":10100,"quantity":4,"order_count":1}])"));
}

TEST(Simulate, UnknownSymbolInQueries) {
    std::istringstream in(R"({"type":"snapshot","symbol":"GOOG"}
{"type":"executions","symbol":"GOOG"})");
    const Json doc = run_simulation(in);
    EXPECT_EQ(doc["responses"][0]["error"]["code"], "INVALID_SYMBOL");
    EXPECT_EQ(doc["responses"][1]["error"]["code"], "INVALID_SYMBOL");
}

}  // namespace
}  // namespace enginex
