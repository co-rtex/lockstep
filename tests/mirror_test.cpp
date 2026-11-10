// FeedMirror: state-machine unit tests on crafted messages, then end-to-end sessions against a
// real server under load, with induced slow-consumer cut-offs and failed subscribes.
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "enginex/client.hpp"
#include "enginex/mirror.hpp"
#include "enginex/rng.hpp"
#include "enginex/server.hpp"

namespace enginex::net {
namespace {

using codec::Json;
using namespace std::chrono_literals;
using V = FeedMirror::Verdict;
using S = FeedMirror::State;

Json level(std::int64_t price, std::int64_t qty, std::int64_t n) {
    return Json{{"price", price}, {"quantity", qty}, {"order_count", n}};
}
Json snapshot(std::uint64_t watermark, bool subscribed, Json bids, Json asks,
              std::size_t depth = 0) {
    return Json{{"type", "snapshot_response"},
                {"event_watermark", watermark},
                {"subscribed", subscribed},
                {"depth", depth},
                {"books", Json::array({Json{{"symbol", "AAPL"}, {"bids", bids}, {"asks", asks}}})}};
}
Json update(std::uint64_t seq, const std::string& side, std::int64_t price, std::int64_t qty,
            std::int64_t n) {
    return Json{{"type", "book_update"},
                {"event_sequence", seq},
                {"symbol", "AAPL"},
                {"levels", Json::array({Json{{"side", side},
                                             {"price", price},
                                             {"quantity", qty},
                                             {"order_count", n}}})}};
}

TEST(FeedMirror, UpdatesBeforeAnyBaselineAreRejected) {
    FeedMirror m;
    EXPECT_EQ(m.on_message(update(1, "BUY", 100, 5, 1)), V::Rejected);
    EXPECT_EQ(m.state(), S::AwaitingBaseline);
    EXPECT_EQ(m.canonical(), "");
    EXPECT_EQ(m.stats().rejected, 1U);
}

TEST(FeedMirror, BaselineThenContiguousUpdatesThenInBandVerification) {
    FeedMirror m;
    EXPECT_EQ(m.on_message(snapshot(10, true, Json::array({level(100, 5, 1)}), Json::array())),
              V::Baseline);
    EXPECT_EQ(m.state(), S::Live);
    EXPECT_EQ(m.on_message(update(11, "SELL", 101, 3, 1)), V::Applied);
    EXPECT_EQ(m.on_message(update(12, "BUY", 100, 0, 0)), V::Applied);  // level removed
    EXPECT_EQ(m.canonical(), "AAPL S 101 3 1\n");
    // In-band check at the matching watermark.
    EXPECT_EQ(m.on_message(snapshot(12, false, Json::array(), Json::array({level(101, 3, 1)}))),
              V::Verified);
    // Depth-limited verification compares only the top levels.
    EXPECT_EQ(m.on_message(update(13, "SELL", 102, 1, 1)), V::Applied);
    EXPECT_EQ(m.on_message(snapshot(13, false, Json::array(), Json::array({level(101, 3, 1)}), 1)),
              V::Verified);
    EXPECT_EQ(m.stats().verified, 2U);
}

TEST(FeedMirror, GapGoesStaleAndNothingIsAppliedAfterwards) {
    FeedMirror m;
    m.on_message(snapshot(5, true, Json::array(), Json::array()));
    EXPECT_EQ(m.on_message(update(7, "BUY", 100, 5, 1)), V::Gap);  // 6 is missing
    EXPECT_EQ(m.state(), S::Stale);
    EXPECT_EQ(m.canonical(), "");  // the gapped update was not applied
    EXPECT_EQ(m.on_message(update(8, "BUY", 100, 5, 1)), V::Rejected);
    EXPECT_EQ(m.canonical(), "");
    EXPECT_EQ(m.stats().gaps, 1U);
    EXPECT_NE(m.last_problem().find("event 7 after 5"), std::string::npos);
    // Recovery only through a fresh baseline.
    m.reset_for_resubscribe();
    EXPECT_EQ(m.on_message(snapshot(8, true, Json::array({level(100, 5, 1)}), Json::array())),
              V::Baseline);
    EXPECT_EQ(m.on_message(update(9, "BUY", 100, 6, 2)), V::Applied);
}

TEST(FeedMirror, DuplicateOrOldEventIsAGapToo) {
    FeedMirror m;
    m.on_message(snapshot(5, true, Json::array(), Json::array()));
    EXPECT_EQ(m.on_message(update(6, "BUY", 100, 5, 1)), V::Applied);
    EXPECT_EQ(m.on_message(update(6, "BUY", 100, 5, 1)), V::Gap);
}

TEST(FeedMirror, FailedSubscribeNeverGoesLive) {
    FeedMirror m;
    const Json err{{"type", "error"},
                   {"command_type", "subscribe"},
                   {"code", "INTERNAL_ERROR"},
                   {"message", "snapshot exceeds the frame limit; request less depth"}};
    EXPECT_EQ(m.on_message(err), V::SubscribeFailed);
    EXPECT_NE(m.state(), S::Live);
    EXPECT_EQ(m.on_message(update(1, "BUY", 100, 5, 1)), V::Rejected);
    EXPECT_EQ(m.canonical(), "");
}

TEST(FeedMirror, ResyncDisconnectAndMismatchAllGoStale) {
    FeedMirror a;
    a.on_message(snapshot(1, true, Json::array(), Json::array()));
    EXPECT_EQ(a.on_message(Json{{"type", "resync_required"}, {"reason", "slow_consumer"}}),
              V::ResyncRequired);
    EXPECT_EQ(a.state(), S::Stale);

    FeedMirror b;
    b.on_message(snapshot(1, true, Json::array(), Json::array()));
    b.on_disconnect();
    EXPECT_EQ(b.state(), S::Stale);

    FeedMirror c;
    c.on_message(snapshot(1, true, Json::array({level(100, 5, 1)}), Json::array()));
    EXPECT_EQ(c.on_message(snapshot(1, false, Json::array({level(100, 4, 1)}), Json::array())),
              V::Mismatch);
    EXPECT_EQ(c.state(), S::Stale);

    FeedMirror d;  // in-band snapshot at a different watermark is a protocol violation
    d.on_message(snapshot(1, true, Json::array(), Json::array()));
    EXPECT_EQ(d.on_message(snapshot(3, false, Json::array(), Json::array())), V::Mismatch);
}

// ---- end to end ------------------------------------------------------------------------------

struct Running {
    explicit Running(ServerOptions o) {
        o.host = "127.0.0.1";
        o.port = 0;
        server = std::make_unique<Server>(o);
        server->start();
    }
    ~Running() { server->stop(); }
    std::unique_ptr<Server> server;
};

// Places random orders and cancels (of its own open orders) until `stop`.
void trade(std::uint16_t port, std::atomic<bool>& stop, std::uint64_t seed) {
    Client c = Client::connect("127.0.0.1", port);
    Rng rng(seed);
    std::vector<std::string> open;
    std::uint64_t n = 0;
    while (!stop.load()) {
        const std::string rid = "t" + std::to_string(seed) + "-" + std::to_string(++n);
        Json req;
        if (!open.empty() && rng.chance(30)) {
            const std::size_t i = rng.below(open.size());
            req = Json{{"type", "cancel"}, {"request_id", rid}, {"order_id", open[i]}};
            open.erase(open.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            req = Json{{"type", "new_limit"},
                       {"request_id", rid},
                       {"symbol", rng.chance(50) ? "AAPL" : "MSFT"},
                       {"side", rng.chance(50) ? "BUY" : "SELL"},
                       {"price", rng.between(9990, 10010)},
                       {"quantity", rng.between(1, 50)}};
        }
        const auto r = c.request(req, 2s);
        if (r && (*r)["type"] == "ack" && (*r).contains("order_id") &&
            (*r).value("remaining", 0) > 0) {
            open.push_back((*r)["order_id"].get<std::string>());
        }
    }
}

TEST(FeedMirrorSession, FollowsLiveTrafficAndVerifiesInBand) {
    Running rs(ServerOptions{});
    std::atomic<bool> stop{false};
    std::thread t1(trade, rs.server->port(), std::ref(stop), 1);
    std::thread t2(trade, rs.server->port(), std::ref(stop), 2);
    FeedMirror m;
    MirrorOptions o;
    o.port = rs.server->port();
    o.duration = 1500ms;
    o.verify_every = 100ms;
    const MirrorReport r = run_mirror(o, m);
    stop = true;
    t1.join();
    t2.join();
    EXPECT_TRUE(r.ok()) << r.last_problem;
    EXPECT_EQ(r.connections, 1U);
    EXPECT_GE(r.stats.verified, 5U);
    EXPECT_GT(r.stats.applied, 50U);
    EXPECT_EQ(r.stats.gaps, 0U);
    EXPECT_EQ(r.stats.mismatches, 0U);
}

TEST(FeedMirrorSession, StalledMirrorIsCutOffThenResyncsToTheExactBook) {
    ServerOptions so;
    so.outbound_capacity = 8;  // a stalled reader overflows quickly
    so.socket_send_buffer = 4096;
    Running rs(so);
    std::atomic<bool> stop{false};
    std::thread t1(trade, rs.server->port(), std::ref(stop), 3);
    std::thread t2(trade, rs.server->port(), std::ref(stop), 4);
    FeedMirror m;
    MirrorOptions o;
    o.port = rs.server->port();
    o.duration = 2000ms;
    o.verify_every = 100ms;
    o.stall_after_first_baseline = 600ms;
    const MirrorReport r = run_mirror(o, m);
    stop = true;
    t1.join();
    t2.join();
    // The server cut the stalled feed (resync_required, or the close itself), the mirror
    // refused to continue on it, reconnected, and ended verified against the live server.
    EXPECT_GE(r.connections, 2U) << r.last_problem;
    EXPECT_GE(r.stats.resyncs + r.stats.disconnects, 1U);
    EXPECT_EQ(r.stats.gaps, 0U);
    EXPECT_EQ(r.stats.mismatches, 0U);
    EXPECT_TRUE(r.ok()) << r.last_problem;
    EXPECT_GE(rs.server->stats().slow_consumer_disconnects, 1U);
}

TEST(FeedMirrorSession, OversizedBaselineNeverGoesLive) {
    ServerOptions so;
    so.max_frame_bytes = 1024;
    Running rs(so);
    {
        Client c = Client::connect("127.0.0.1", rs.server->port());
        for (int i = 0; i < 40; ++i) {  // 40 ask levels: the full snapshot exceeds 1 KiB
            ASSERT_TRUE(c.request(Json{{"type", "new_limit"},
                                       {"request_id", "s" + std::to_string(i)},
                                       {"symbol", "AAPL"},
                                       {"side", "SELL"},
                                       {"price", 10000 + i},
                                       {"quantity", 1}},
                                  2s));
        }
    }
    FeedMirror m;
    MirrorOptions o;
    o.port = rs.server->port();
    o.duration = 300ms;
    o.max_resubscribes = 2;
    const MirrorReport r = run_mirror(o, m);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.stats.baselines, 0U);
    EXPECT_GE(r.stats.subscribe_failures, 1U);
    EXPECT_EQ(r.stats.applied, 0U);
    EXPECT_NE(r.state, "live");
    EXPECT_NE(r.last_problem.find("frame limit"), std::string::npos) << r.last_problem;
}

}  // namespace
}  // namespace enginex::net
