// Cross-implementation golden fixtures: bytes packed by tools/itch_fixture.py (Python struct,
// straight from the specification's offsets) with hand-written expected books.
#include <gtest/gtest.h>

#include <fstream>
#include <iterator>

#include "enginex/itch/replay.hpp"
#include "enginex/json_codec.hpp"

namespace enginex::itch {
namespace {

using codec::Json;

std::vector<std::uint8_t> read_bytes(const std::string& name) {
    std::ifstream f(std::string(ENGINEX_TESTDATA_DIR) + "/itch/" + name, std::ios::binary);
    EXPECT_TRUE(f) << name;
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

Json read_json(const std::string& name) {
    std::ifstream f(std::string(ENGINEX_TESTDATA_DIR) + "/itch/" + name);
    EXPECT_TRUE(f) << name;
    return Json::parse(f);
}

Json levels_json(const std::vector<LevelView>& levels) {
    Json j = Json::array();
    for (const auto& l : levels) j.push_back(Json::array({l.price, l.shares, l.orders}));
    return j;
}

Json orders_json(const std::vector<OrderView>& orders) {
    Json j = Json::array();
    for (const auto& o : orders) j.push_back(Json::array({o.ref, o.shares}));
    return j;
}

TEST(ItchFixture, LifecycleMatchesHandWrittenBook) {
    const auto file = read_bytes("lifecycle.itch");
    const Json want = read_json("lifecycle.expected.json");
    BookBuilder b;
    ReplayOptions strict;
    strict.strict = true;
    const ReplayResult r = replay(file, b, strict);
    ASSERT_TRUE(r.ok()) << *r.stopped;
    EXPECT_EQ(r.frames, want["frames"]);
    EXPECT_EQ(r.decode_error_count(), 0U);
    EXPECT_EQ(b.stats().anomaly_count(), want["anomalies"]);
    for (const auto& [type, n] : want["messages_by_type"].items()) {
        EXPECT_EQ(b.stats().messages_by_type[static_cast<unsigned char>(type[0])], n) << type;
    }
    for (const auto& [symbol, book] : want["books"].items()) {
        const auto locate = b.find_locate(symbol);
        ASSERT_TRUE(locate) << symbol;
        EXPECT_EQ(levels_json(b.depth(*locate, Side::Buy, 0)), book["bids"]) << symbol;
        EXPECT_EQ(levels_json(b.depth(*locate, Side::Sell, 0)), book["asks"]) << symbol;
        EXPECT_EQ(orders_json(b.orders(*locate, Side::Buy)), book["bid_orders"]) << symbol;
        EXPECT_EQ(orders_json(b.orders(*locate, Side::Sell)), book["ask_orders"]) << symbol;
    }
    const BookStats& s = b.stats();
    EXPECT_EQ(s.executed_shares, want["executed_shares"]);
    EXPECT_EQ(s.printable_executed_shares, want["printable_executed_shares"]);
    EXPECT_EQ(s.canceled_shares, want["canceled_shares"]);
    EXPECT_EQ(s.non_displayed_trade_shares, want["non_displayed_trade_shares"]);
    EXPECT_EQ(s.cross_shares, want["cross_shares"]);
    EXPECT_EQ(s.broken_trades, want["broken_trades"]);
    EXPECT_EQ(s.live_orders, want["live_orders"]);
    EXPECT_EQ(std::string(1, s.last_system_event), want["last_system_event"].get<std::string>());
    EXPECT_EQ(b.trading_state(*b.find_locate("AAPL")), 'T');
    ASSERT_TRUE(b.find_order(2));
    EXPECT_EQ(b.find_order(2)->price, 1'500'000U);
}

TEST(ItchFixture, MalformedFileIsClassifiedExactly) {
    const auto file = read_bytes("malformed.itch");
    const Json want = read_json("malformed.expected.json");
    BookBuilder b;
    const ReplayResult r = replay(file, b);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.frames, want["frames"]);
    EXPECT_EQ(r.decoded, want["decoded"]);
    EXPECT_EQ(r.decode_errors[static_cast<std::size_t>(DecodeError::UnknownType)],
              want["unknown_type"]);
    EXPECT_EQ(r.decode_errors[static_cast<std::size_t>(DecodeError::LengthMismatch)],
              want["length_mismatch"]);
    EXPECT_EQ(std::string(to_string(r.frame_error)), want["frame_error"].get<std::string>());
    EXPECT_EQ(r.frame_error_offset, want["frame_error_offset"]);
    EXPECT_EQ(b.stats().live_orders, want["live_orders"]);
}

}  // namespace
}  // namespace enginex::itch
