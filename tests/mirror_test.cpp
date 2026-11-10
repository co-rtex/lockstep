// FeedMirror: state-machine unit tests on crafted messages, then end-to-end sessions against a
// real server under load, with induced slow-consumer cut-offs and failed subscribes.
#include <gtest/gtest.h>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <atomic>
#include <mutex>
#include <thread>

#include "enginex/client.hpp"
#include "enginex/mirror.hpp"
#include "enginex/protocol.hpp"
#include "enginex/socket.hpp"
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

TEST(FeedMirror, MalformedUpdateIsNotAppliedPartially) {
    FeedMirror m;
    m.on_message(snapshot(1, true, Json::array({level(100, 5, 1)}), Json::array()));
    Json u = update(2, "BUY", 101, 7, 1);
    u["levels"].push_back(Json{{"side", "BUY"}, {"price", 102}});  // missing quantity
    EXPECT_EQ(m.on_message(u), V::Malformed);
    EXPECT_EQ(m.state(), S::Stale);
    EXPECT_EQ(m.canonical(), "AAPL B 100 5 1\n");  // the valid first level was not applied
    EXPECT_EQ(m.stats().malformed, 1U);
    EXPECT_EQ(m.on_message(Json{{"type", "snapshot_response"}, {"subscribed", true}}),
              V::Malformed);  // baseline without books
}

TEST(FeedMirror, SymbolSetMustMatchTheBaseline) {
    FeedMirror a;  // update for a symbol the baseline does not have
    a.on_message(snapshot(1, true, Json::array(), Json::array()));
    Json u = update(2, "BUY", 100, 1, 1);
    u["symbol"] = "MSFT";
    EXPECT_EQ(a.on_message(u), V::Mismatch);
    EXPECT_EQ(a.state(), S::Stale);
    EXPECT_EQ(a.stats().mismatches, 1U);

    FeedMirror b;  // mirror holds AAPL, verification snapshot lacks it
    b.on_message(snapshot(1, true, Json::array({level(100, 5, 1)}), Json::array()));
    const Json empty_snap{{"type", "snapshot_response"}, {"event_watermark", 1},
                          {"subscribed", false},         {"depth", 0},
                          {"books", Json::array()}};
    EXPECT_EQ(b.on_message(empty_snap), V::Mismatch);
    EXPECT_EQ(b.stats().mismatches, 1U);
}

TEST(FeedMirror, SubscribeErrorOnALiveFeedGoesStale) {
    FeedMirror m;
    m.on_message(snapshot(1, true, Json::array(), Json::array()));
    EXPECT_EQ(m.on_message(Json{{"type", "error"}, {"command_type", "subscribe"},
                                {"code", "INVALID_REQUEST"}, {"message", "already subscribed"}}),
              V::SubscribeFailed);
    EXPECT_EQ(m.state(), S::Stale);
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

    FeedMirror e;  // a second subscribe snapshot on a live feed is a protocol violation
    e.on_message(snapshot(1, true, Json::array(), Json::array()));
    EXPECT_EQ(e.on_message(snapshot(1, true, Json::array(), Json::array())), V::Mismatch);
    EXPECT_EQ(e.state(), S::Stale);
    EXPECT_EQ(e.stats().mismatches, 1U);  // counted, so a later ok() cannot hide it
    EXPECT_EQ(to_string(e.state()), "stale");
    EXPECT_EQ(to_string(V::Gap), "gap");
    EXPECT_EQ(e.on_message(Json{{"type", "ack"}}), V::Ignored);

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
    EXPECT_GE(r.connections, 1U);  // a slow-consumer cut under heavy load would be legitimate
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
    // Long enough that two closed-loop traders produce far more event bytes than the outbound
    // queue (8 frames) plus kernel socket buffers can hold, even with Linux autotuning.
    o.duration = 3000ms;
    o.verify_every = 100ms;
    o.stall_after_first_baseline = 1500ms;
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

// TCP proxy that silently drops the `drop_nth` book_update frame on its first connection
// (frame loss that TCP itself would never produce, e.g. a buggy relay). Later connections are
// forwarded intact.
class LossyProxy {
public:
    LossyProxy(std::uint16_t upstream, int drop_nth)
        : upstream_(upstream), drop_nth_(drop_nth), listener_(listen_tcp("127.0.0.1", 0)) {
        port_ = local_port(listener_.fd());
        acceptor_ = std::thread([this] { accept_loop(); });
    }
    ~LossyProxy() {
        stop_ = true;
        ::shutdown(listener_.fd(), SHUT_RDWR);
        acceptor_.join();
        std::lock_guard lock(mu_);
        for (auto& t : pumps_) t.join();
    }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] int dropped() const { return dropped_.load(); }

private:
    void accept_loop() {
        int conn_no = 0;
        while (!stop_) {
            // accept() does not honor SO_RCVTIMEO on macOS: wait with poll() so `stop_` is seen.
            pollfd p{listener_.fd(), POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0) {
                continue;
            }
            const int fd = ::accept(listener_.fd(), nullptr, nullptr);
            if (fd < 0) {
                continue;
            }
            auto down = std::make_shared<Socket>(fd);
            std::shared_ptr<Socket> up;
            try {
                up = std::make_shared<Socket>(connect_tcp("127.0.0.1", upstream_, 2s));
            } catch (const std::exception&) {
                continue;  // upstream gone: drop this client (closing `down`)
            }
            configure_stream(down->fd(), 100ms, 2000ms);
            configure_stream(up->fd(), 100ms, 2000ms);
            const bool lossy = conn_no++ == 0;
            std::lock_guard lock(mu_);
            pumps_.emplace_back([this, down, up] { copy_raw(*down, *up); });
            pumps_.emplace_back([this, down, up, lossy] { copy_frames(*up, *down, lossy); });
        }
    }
    void copy_raw(Socket& from, Socket& to) {
        char buf[4096];
        while (!stop_) {
            const ssize_t n = ::recv(from.fd(), buf, sizeof buf, 0);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                break;
            }
            if (n > 0 && send_all(to.fd(), std::string_view(buf, static_cast<std::size_t>(n))) != 0) {
                break;
            }
        }
        ::shutdown(to.fd(), SHUT_RDWR);
        ::shutdown(from.fd(), SHUT_RDWR);
    }
    void copy_frames(Socket& from, Socket& to, bool lossy) {
        FrameDecoder dec(kMaxServerFrame);
        char buf[4096];
        int updates = 0;
        std::string payload;
        while (!stop_) {
            const ssize_t n = ::recv(from.fd(), buf, sizeof buf, 0);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                break;
            }
            if (n <= 0) {
                continue;
            }
            dec.feed(buf, static_cast<std::size_t>(n));
            bool ok = true;
            while (ok && dec.next(payload) == FrameDecoder::Status::Frame) {
                if (lossy && payload.find("\"type\":\"book_update\"") != std::string::npos &&
                    ++updates == drop_nth_) {
                    dropped_.fetch_add(1);
                    continue;  // the loss
                }
                ok = send_all(to.fd(), encode_frame(payload, kMaxServerFrame)) == 0;
            }
            if (!ok) {
                break;
            }
        }
        ::shutdown(to.fd(), SHUT_RDWR);
        ::shutdown(from.fd(), SHUT_RDWR);
    }

    std::uint16_t upstream_;
    int drop_nth_;
    Socket listener_;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<int> dropped_{0};
    std::thread acceptor_;
    std::mutex mu_;
    std::vector<std::thread> pumps_;
};

TEST(FeedMirrorSession, DroppedEventIsDetectedAsGapAndRecoveredBySnapshot) {
    Running rs(ServerOptions{});
    LossyProxy proxy(rs.server->port(), /*drop_nth=*/20);
    std::atomic<bool> stop{false};
    std::thread t1(trade, rs.server->port(), std::ref(stop), 5);
    FeedMirror m;
    MirrorOptions o;
    o.port = proxy.port();  // the mirror reads through the lossy relay
    o.duration = 1500ms;
    // No periodic verification: a verify served right after the dropped event would detect the
    // loss as a watermark mismatch instead of a gap (also correct, but this test pins the gap).
    o.verify_every = 60000ms;
    const MirrorReport r = run_mirror(o, m);
    stop = true;
    t1.join();
    ASSERT_EQ(proxy.dropped(), 1);
    EXPECT_EQ(r.stats.gaps, 1U) << r.last_problem;  // detected, not applied
    EXPECT_EQ(r.connections, 2U);                    // resubscribed through a clean connection
    EXPECT_EQ(r.stats.mismatches, 0U);
    EXPECT_TRUE(r.ok()) << r.last_problem;
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
    // Server side: nothing was registered. An accepted command afterwards reaches no one.
    Client c = Client::connect("127.0.0.1", rs.server->port());
    ASSERT_TRUE(c.request(Json{{"type", "new_limit"}, {"request_id", "late"}, {"symbol", "AAPL"},
                               {"side", "BUY"}, {"price", 100}, {"quantity", 1}},
                          2s));
    std::this_thread::sleep_for(300ms);  // subscriber count is refreshed by the sequencer loop
    EXPECT_EQ(rs.server->stats().subscribers, 0U);
}

TEST(FeedMirrorSession, UnreachableServerIsReportedNotThrown) {
    std::uint16_t port = 0;
    {
        const Socket probe = listen_tcp("127.0.0.1", 0);  // find a free port, then free it
        port = local_port(probe.fd());
    }
    FeedMirror m;
    MirrorOptions o;
    o.port = port;
    o.duration = 200ms;
    o.max_resubscribes = 3;
    MirrorReport r;
    ASSERT_NO_THROW(r = run_mirror(o, m));
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.connections, 0U);
    EXPECT_EQ(r.stats.connect_failures, 4U);  // first attempt + 3 retries, with back-off
    EXPECT_FALSE(r.last_problem.empty());
}

}  // namespace
}  // namespace enginex::net
