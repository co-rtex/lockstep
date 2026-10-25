// R3 networking (spec §10, §11.5): framing under arbitrary segmentation, protocol parsing,
// bounded queues, and end-to-end server behavior over real TCP sockets: concurrent clients,
// snapshot/subscribe handoff under load, slow consumers, reconnects, retries, overload,
// shutdown and restart.
#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <set>
#include <thread>

#include "enginex/bounded_queue.hpp"
#include "enginex/canonical.hpp"
#include "enginex/client.hpp"
#include "enginex/protocol.hpp"
#include "enginex/server.hpp"
#include "temp_dir.hpp"

namespace enginex::net {
namespace {

using codec::Json;
using namespace std::chrono_literals;

constexpr auto kTimeout = 5s;

// ---- framing -------------------------------------------------------------------------------

TEST(Framing, EncodesBigEndianLength) {
    const std::string f = encode_frame("abc");
    ASSERT_EQ(f.size(), 7U);
    EXPECT_EQ(f.substr(0, 4), std::string("\0\0\0\x03", 4));
    EXPECT_EQ(f.substr(4), "abc");
    EXPECT_THROW((void)encode_frame(""), std::length_error);
    EXPECT_THROW((void)encode_frame(std::string(kMaxClientFrame + 1, 'x'), kMaxClientFrame),
                 std::length_error);
    EXPECT_EQ(encode_frame(std::string(256, 'x')).substr(0, 4), std::string("\0\0\x01\0", 4));
}

TEST(Framing, DecodesAnySegmentation) {
    std::string stream;
    std::vector<std::string> payloads;
    for (int i = 0; i < 50; ++i) {
        payloads.push_back(std::string(static_cast<std::size_t>(1 + i * 37 % 300),
                                       static_cast<char>('a' + i % 26)));
        stream += encode_frame(payloads.back());
    }
    for (const std::size_t chunk : {1U, 2U, 3U, 5U, 7U, 64U, 1000U, 100000U}) {
        FrameDecoder d(kMaxClientFrame);
        std::vector<std::string> got;
        std::string out;
        for (std::size_t off = 0; off < stream.size(); off += chunk) {
            const std::size_t n = std::min(chunk, stream.size() - off);
            d.feed(stream.data() + off, n);
            while (d.next(out) == FrameDecoder::Status::Frame) {
                got.push_back(out);
            }
        }
        EXPECT_EQ(got, payloads) << "chunk " << chunk;
        EXPECT_FALSE(d.has_partial());
    }
}

TEST(Framing, RejectsOversizedAndZeroLengthBeforeReadingPayload) {
    FrameDecoder d(kMaxClientFrame);
    const std::uint32_t big = kMaxClientFrame + 1;
    const char header[4] = {static_cast<char>(big >> 24), static_cast<char>(big >> 16),
                            static_cast<char>(big >> 8), static_cast<char>(big)};
    d.feed(header, 4);  // no payload bytes needed to detect the violation
    std::string out;
    EXPECT_EQ(d.next(out), FrameDecoder::Status::Error);
    EXPECT_NE(d.error().find("invalid frame length"), std::string::npos);
    EXPECT_EQ(d.next(out), FrameDecoder::Status::Error);  // sticky

    FrameDecoder z(kMaxClientFrame);
    z.feed(std::string(4, '\0').data(), 4);
    EXPECT_EQ(z.next(out), FrameDecoder::Status::Error);

    FrameDecoder exact(kMaxClientFrame);
    const std::string f = encode_frame(std::string(kMaxClientFrame, 'm'), kMaxClientFrame);
    exact.feed(f.data(), f.size());
    EXPECT_EQ(exact.next(out), FrameDecoder::Status::Frame);
    EXPECT_EQ(out.size(), kMaxClientFrame);
}

TEST(Protocol, ParsesClientMessages) {
    auto m = parse_client_message(
        R"({"protocol_version":1,"type":"new_limit","request_id":"r","symbol":"AAPL","side":"BUY","price":5,"quantity":2})");
    ASSERT_TRUE(std::holds_alternative<ClientMessage>(m));
    EXPECT_TRUE(std::holds_alternative<Command>(std::get<ClientMessage>(m)));
    m = parse_client_message(
        R"({"protocol_version":1,"type":"snapshot_request","symbol":"MSFT","depth":3,"request_id":"s"})");
    const auto& snap = std::get<SnapshotRequest>(std::get<ClientMessage>(m));
    EXPECT_EQ(snap.symbol, "MSFT");
    EXPECT_EQ(snap.depth, 3U);
    m = parse_client_message(R"({"protocol_version":1,"type":"subscribe"})");
    EXPECT_TRUE(std::holds_alternative<SubscribeRequest>(std::get<ClientMessage>(m)));
}

TEST(Protocol, RejectsBadMessages) {
    for (const char* text :
         {"not json", "[]", R"({"type":"subscribe"})",
          R"({"protocol_version":2,"type":"subscribe"})",
          R"({"protocol_version":"1","type":"subscribe"})", R"({"protocol_version":1})",
          R"({"protocol_version":1,"type":"teleport"})",
          R"({"protocol_version":1,"type":"subscribe","extra":true})",
          R"({"protocol_version":1,"type":"snapshot_request","depth":-1})"}) {
        auto m = parse_client_message(text);
        ASSERT_TRUE(std::holds_alternative<codec::ParseError>(m)) << text;
        EXPECT_EQ(std::get<codec::ParseError>(m).code, ErrorCode::InvalidRequest) << text;
    }
    auto bad_price = parse_client_message(
        R"({"protocol_version":1,"type":"new_limit","request_id":"r","symbol":"AAPL","side":"BUY","price":1.5,"quantity":2})");
    EXPECT_EQ(std::get<codec::ParseError>(bad_price).code, ErrorCode::InvalidPrice);
}

// ---- bounded queue -------------------------------------------------------------------------

TEST(BoundedQueue, CapacityCloseAndReservedSlot) {
    BoundedQueue<int> q(2);
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_FALSE(q.try_push(3));
    EXPECT_TRUE(q.push_reserved(99));
    EXPECT_EQ(q.size(), 3U);
    q.close();
    EXPECT_FALSE(q.try_push(4));
    EXPECT_FALSE(q.push_reserved(5));
    EXPECT_EQ(q.pop(), 1);
    EXPECT_EQ(q.pop(), 2);
    EXPECT_EQ(q.pop(), 99);
    EXPECT_EQ(q.pop(), std::nullopt);
    EXPECT_TRUE(q.closed_and_empty());
}

TEST(BoundedQueue, ManyProducersOneConsumerPreservesPerProducerOrder) {
    BoundedQueue<std::pair<int, int>> q(64);
    constexpr int kProducers = 8;
    constexpr int kItems = 5000;
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&q, p] {
            for (int i = 0; i < kItems; ++i) {
                while (!q.try_push({p, i})) {
                    std::this_thread::yield();
                }
            }
        });
    }
    std::vector<int> next(kProducers, 0);
    int total = 0;
    std::vector<std::pair<int, int>> batch;
    while (total < kProducers * kItems) {
        batch.clear();
        q.pop_batch(batch, 128, 100ms);
        for (const auto& [p, i] : batch) {
            ASSERT_EQ(i, next[static_cast<std::size_t>(p)]++);
            ++total;
        }
    }
    for (auto& t : producers) {
        t.join();
    }
    q.close();
    EXPECT_TRUE(q.closed_and_empty());
}

// ---- server fixtures -----------------------------------------------------------------------

struct TestServer {
    explicit TestServer(bool journal, ServerOptions extra = {}, std::string dir_path = {})
        : dir(), opts(std::move(extra)) {
        opts.host = "127.0.0.1";
        opts.port = 0;
        if (journal) {
            DurableOptions d;
            d.journal_path = dir_path.empty() ? dir.file("server.exj") : dir_path;
            opts.journal = d;
        }
        server = std::make_unique<Server>(opts);
        server->start();
    }
    ~TestServer() { server->stop(); }

    Client connect() const { return Client::connect("127.0.0.1", server->port(), kTimeout); }

    test::TempDir dir;
    ServerOptions opts;
    std::unique_ptr<Server> server;
};

Json order(const std::string& rid, const std::string& side, PriceCents price, Quantity qty,
           const std::string& symbol = "AAPL") {
    return Json{{"type", "new_limit"}, {"request_id", rid}, {"symbol", symbol},
                {"side", side},        {"price", price},    {"quantity", qty}};
}

Json cancel(const std::string& rid, const std::string& order_id) {
    return Json{{"type", "cancel"}, {"request_id", rid}, {"order_id", order_id}};
}

// Aggregated book rebuilt by a subscriber from a snapshot plus book_update levels.
struct BookMirror {
    std::map<std::string, std::map<std::pair<std::string, PriceCents>, std::pair<Quantity, int>>>
        books;

    void load(const Json& snapshot) {
        books.clear();
        for (const auto& b : snapshot["books"]) {
            auto& book = books[b["symbol"].get<std::string>()];
            for (const char* side : {"bids", "asks"}) {
                const std::string s = std::string(side) == "bids" ? "BUY" : "SELL";
                for (const auto& l : b[side]) {
                    book[{s, l["price"].get<PriceCents>()}] = {l["quantity"].get<Quantity>(),
                                                               l["order_count"].get<int>()};
                }
            }
        }
    }
    void apply(const Json& update) {
        auto& book = books[update["symbol"].get<std::string>()];
        for (const auto& l : update["levels"]) {
            const std::pair<std::string, PriceCents> key{l["side"].get<std::string>(),
                                                         l["price"].get<PriceCents>()};
            if (l["quantity"].get<Quantity>() == 0) {
                book.erase(key);
            } else {
                book[key] = {l["quantity"].get<Quantity>(), l["order_count"].get<int>()};
            }
        }
    }
};

// ---- end-to-end ----------------------------------------------------------------------------

TEST(Server, OrderEntryAckSnapshotAndErrors) {
    TestServer ts(/*journal=*/true);
    Client c = ts.connect();
    auto a1 = c.request(order("r1", "BUY", 10100, 10), kTimeout);
    ASSERT_TRUE(a1);
    EXPECT_EQ((*a1)["type"], "ack");
    EXPECT_EQ((*a1)["protocol_version"], 1);
    EXPECT_EQ((*a1)["command_sequence"], 1);
    EXPECT_EQ((*a1)["order_id"], "ord-1");
    auto a2 = c.request(order("r2", "SELL", 10000, 4), kTimeout);
    ASSERT_TRUE(a2);
    ASSERT_EQ((*a2)["executions"].size(), 1U);
    EXPECT_EQ((*a2)["executions"][0]["price"], 10100);

    auto bad = c.request(order("r3", "BUY", 0, 1), kTimeout);
    ASSERT_TRUE(bad);
    EXPECT_EQ((*bad)["type"], "error");
    EXPECT_EQ((*bad)["code"], "INVALID_PRICE");
    EXPECT_EQ((*bad)["command_type"], "new_limit");

    auto snap = c.request(
        Json{{"type", "snapshot_request"}, {"request_id", "s1"}, {"symbol", "AAPL"}}, kTimeout);
    ASSERT_TRUE(snap);
    EXPECT_EQ((*snap)["type"], "snapshot_response");
    EXPECT_EQ((*snap)["event_watermark"], 2);
    EXPECT_EQ((*snap)["books"][0]["bids"],
              Json::parse(R"([{"price":10100,"quantity":6,"order_count":1}])"));

    auto unknown = c.request(
        Json{{"type", "snapshot_request"}, {"request_id", "s2"}, {"symbol", "TSLA"}}, kTimeout);
    ASSERT_TRUE(unknown);
    EXPECT_EQ((*unknown)["code"], "INVALID_SYMBOL");

    // Malformed JSON gets an error, and the connection stays usable.
    ASSERT_TRUE(c.send_raw(encode_frame("{nope")));
    auto parse_err = c.receive(kTimeout);
    ASSERT_TRUE(parse_err);
    EXPECT_EQ((*parse_err)["code"], "INVALID_REQUEST");
    auto again = c.request(order("r4", "BUY", 9000, 1), kTimeout);
    ASSERT_TRUE(again);
    EXPECT_EQ((*again)["command_sequence"], 3);
    auto cx = c.request(cancel("c1", "ord-1"), kTimeout);
    ASSERT_TRUE(cx);
    EXPECT_EQ((*cx)["type"], "ack");
    EXPECT_EQ((*cx)["command_type"], "cancel");
    EXPECT_EQ((*cx)["cancelled_quantity"], 6);
    auto cx2 = c.request(cancel("c2", "ord-1"), kTimeout);
    ASSERT_TRUE(cx2);
    EXPECT_EQ((*cx2)["code"], "ORDER_NOT_OPEN");
}

TEST(Server, HandlesFragmentedAndCoalescedFrames) {
    TestServer ts(false);
    Client c = ts.connect();
    // One frame dribbled byte by byte.
    const std::string f1 = encode_frame(Json{{"protocol_version", 1},
                                             {"type", "new_limit"},
                                             {"request_id", "f1"},
                                             {"symbol", "AAPL"},
                                             {"side", "BUY"},
                                             {"price", 100},
                                             {"quantity", 1}}
                                            .dump());
    for (const char ch : f1) {
        ASSERT_TRUE(c.send_raw(std::string_view(&ch, 1)));
        std::this_thread::sleep_for(1ms);
    }
    auto r1 = c.receive(kTimeout);
    ASSERT_TRUE(r1);
    EXPECT_EQ((*r1)["request_id"], "f1");
    // Three frames in one write, split across a frame boundary in the middle of a header.
    std::string many;
    for (int i = 2; i <= 4; ++i) {
        many += encode_frame(Json{{"protocol_version", 1},
                                  {"type", "new_limit"},
                                  {"request_id", "f" + std::to_string(i)},
                                  {"symbol", "AAPL"},
                                  {"side", "BUY"},
                                  {"price", 100},
                                  {"quantity", 1}}
                                 .dump());
    }
    const std::size_t cut = f1.size() + 2;
    ASSERT_TRUE(c.send_raw(std::string_view(many).substr(0, cut)));
    std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(c.send_raw(std::string_view(many).substr(cut)));
    for (int i = 2; i <= 4; ++i) {
        auto r = c.receive(kTimeout);
        ASSERT_TRUE(r);
        EXPECT_EQ((*r)["request_id"], "f" + std::to_string(i));
        EXPECT_EQ((*r)["command_sequence"], i);
    }
}

TEST(Server, OversizedFrameIsRejectedAndConnectionClosed) {
    TestServer ts(false);
    Client c = ts.connect();
    const std::uint32_t big = 1U << 30;
    const char header[4] = {static_cast<char>(big >> 24), static_cast<char>(big >> 16),
                            static_cast<char>(big >> 8), static_cast<char>(big)};
    ASSERT_TRUE(c.send_raw(std::string_view(header, 4)));
    auto err = c.receive(kTimeout);
    ASSERT_TRUE(err);
    EXPECT_EQ((*err)["type"], "error");
    EXPECT_NE((*err)["message"].get<std::string>().find("invalid frame length"), std::string::npos);
    EXPECT_FALSE(c.receive(kTimeout).has_value());
    EXPECT_TRUE(c.closed());
    // The server keeps serving other clients.
    Client d = ts.connect();
    EXPECT_TRUE(d.request(order("ok", "BUY", 100, 1), kTimeout));
    EXPECT_EQ(ts.server->stats().protocol_errors, 1U);
}

TEST(Server, ConcurrentClientsGetUniqueContiguousSequences) {
    TestServer ts(true);
    constexpr int kClients = 8;
    constexpr int kOrders = 150;
    std::vector<std::vector<Json>> acks(kClients);
    std::vector<std::thread> threads;
    for (int c = 0; c < kClients; ++c) {
        threads.emplace_back([&, c] {
            Client client = ts.connect();
            for (int i = 0; i < kOrders; ++i) {
                const std::string side = (i + c) % 2 ? "BUY" : "SELL";
                auto r =
                    client.request(order("c" + std::to_string(c) + "-" + std::to_string(i), side,
                                         10000 + (i % 7) - 3, 1 + i % 5, i % 3 ? "AAPL" : "MSFT"),
                                   kTimeout);
                if (r) {
                    acks[static_cast<std::size_t>(c)].push_back(*r);
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    std::set<std::uint64_t> seqs;
    std::map<std::uint64_t, Json> by_seq;
    for (const auto& list : acks) {
        ASSERT_EQ(list.size(), static_cast<std::size_t>(kOrders));
        std::uint64_t last = 0;
        for (const auto& a : list) {
            ASSERT_EQ(a["type"], "ack") << a.dump();
            const auto seq = a["command_sequence"].get<std::uint64_t>();
            EXPECT_GT(seq, last) << "per-client acks out of order";
            last = seq;
            EXPECT_TRUE(seqs.insert(seq).second) << "duplicate sequence " << seq;
            by_seq[seq] = a;
        }
    }
    ASSERT_EQ(seqs.size(), static_cast<std::size_t>(kClients * kOrders));
    EXPECT_EQ(*seqs.begin(), 1U);
    EXPECT_EQ(*seqs.rbegin(), static_cast<std::uint64_t>(kClients * kOrders));

    // Replaying the commands in acknowledged sequence order reproduces the server's state.
    ts.server->stop();
    ExchangeStateMachine replay;
    for (const auto& [seq, a] : by_seq) {
        const Command cmd =
            NewLimitOrder{a["request_id"].get<std::string>(), a["symbol"].get<std::string>(),
                          *parse_side(a["side"].get<std::string>()), a["price"].get<PriceCents>(),
                          a["quantity"].get<Quantity>()};
        const CommandResult r = replay.process(cmd);
        ASSERT_EQ(r.command_sequence, seq);
        ASSERT_EQ(r.executions.size(), a["executions"].size()) << "seq " << seq;
    }
    EXPECT_EQ(state_hash(replay), state_hash(ts.server->state()));
    EXPECT_GE(ts.server->stats().batches, 1U);
}

TEST(Server, SubscribeHandoffUnderLoadHasNoGaps) {
    TestServer ts(true);
    std::atomic<bool> stop{false};
    std::atomic<int> sent{0};
    std::vector<std::thread> writers;
    for (int w = 0; w < 3; ++w) {
        writers.emplace_back([&, w] {
            Client c = ts.connect();
            for (int i = 0; !stop.load(); ++i) {
                const bool buy = (i + w) % 2 == 0;
                auto r = c.request(
                    order("w" + std::to_string(w) + "-" + std::to_string(i), buy ? "BUY" : "SELL",
                          10000 + (i % 9) - 4, 1 + i % 6, i % 4 ? "AAPL" : "MSFT"),
                    kTimeout);
                if (r) {
                    sent.fetch_add(1);
                }
            }
        });
    }
    while (sent.load() < 100) {
        std::this_thread::sleep_for(1ms);
    }
    // Subscribe while orders are flowing.
    Client sub = ts.connect();
    ASSERT_TRUE(sub.send(Json{{"type", "subscribe"}, {"request_id", "sub"}}));
    auto snap = sub.receive(kTimeout);
    ASSERT_TRUE(snap);
    ASSERT_EQ((*snap)["type"], "snapshot_response");
    ASSERT_TRUE((*snap)["subscribed"].get<bool>());
    const auto watermark = (*snap)["event_watermark"].get<std::uint64_t>();
    EXPECT_GT(watermark, 0U);
    BookMirror mirror;
    mirror.load(*snap);
    std::uint64_t expected = watermark + 1;
    while (sent.load() < 400) {
        auto ev = sub.receive(kTimeout);
        ASSERT_TRUE(ev);
        ASSERT_EQ((*ev)["type"], "book_update");
        ASSERT_EQ((*ev)["event_sequence"].get<std::uint64_t>(), expected) << "gap in feed";
        EXPECT_EQ((*ev)["command_sequence"], (*ev)["event_sequence"]);
        mirror.apply(*ev);
        ++expected;
    }
    stop = true;
    for (auto& t : writers) {
        t.join();
    }
    // Drain the remaining events, then compare the mirrored book with a fresh snapshot.
    Client checker = ts.connect();
    auto final_snap = checker.request(
        Json{{"type", "snapshot_request"}, {"request_id", "f"}, {"depth", 0}}, kTimeout);
    ASSERT_TRUE(final_snap);
    const auto final_wm = (*final_snap)["event_watermark"].get<std::uint64_t>();
    while (expected <= final_wm) {
        auto ev = sub.receive(kTimeout);
        ASSERT_TRUE(ev);
        ASSERT_EQ((*ev)["event_sequence"].get<std::uint64_t>(), expected);
        mirror.apply(*ev);
        ++expected;
    }
    BookMirror reference;
    reference.load(*final_snap);
    EXPECT_EQ(mirror.books, reference.books);
}

TEST(Server, SlowConsumerIsDisconnectedWithoutStallingTheSequencer) {
    ServerOptions o;
    o.outbound_capacity = 8;
    o.send_timeout = 300ms;
    o.socket_send_buffer = 16 * 1024;  // keep kernel buffering small on every platform
    TestServer ts(false, o);
    // A subscriber that never reads.
    Client slow = ts.connect();
    ASSERT_TRUE(slow.send(Json{{"type", "subscribe"}, {"request_id", "slow"}}));
    Client fast = ts.connect();
    ASSERT_TRUE(fast.send(Json{{"type", "subscribe"}, {"request_id", "fast"}}));
    auto fsnap = fast.receive(kTimeout);
    ASSERT_TRUE(fsnap);
    const auto wm = (*fsnap)["event_watermark"].get<std::uint64_t>();

    Client trader = ts.connect();
    const auto t0 = std::chrono::steady_clock::now();
    // Keep trading until the server reports the slow consumer was cut off (bounded).
    int orders = 0;
    for (; orders < 50000 && ts.server->stats().slow_consumer_disconnects == 0; ++orders) {
        auto r = trader.request(order("t" + std::to_string(orders), orders % 2 ? "BUY" : "SELL",
                                      9000 + orders % 2000, 1),
                                kTimeout);
        ASSERT_TRUE(r) << "sequencer stalled at order " << orders;
        // The healthy subscriber keeps up and sees every event in order.
        auto ev = fast.receive(kTimeout);
        ASSERT_TRUE(ev);
        ASSERT_EQ((*ev)["event_sequence"].get<std::uint64_t>(),
                  wm + 1 + static_cast<std::uint64_t>(orders));
    }
    EXPECT_GE(ts.server->stats().slow_consumer_disconnects, 1U);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 60s);
    // Trading continues normally after the disconnect.
    for (int i = 0; i < 50; ++i) {
        ASSERT_TRUE(trader.request(order("after" + std::to_string(i), "BUY", 100, 1), kTimeout));
    }

    // The slow client finds: its snapshot, contiguous events, an optional final
    // resync_required (it is lost if the socket was already unwritable), then EOF.
    std::uint64_t last = 0;
    while (auto m = slow.receive(3s)) {
        const std::string type = (*m)["type"];
        if (type == "book_update") {
            const auto seq = (*m)["event_sequence"].get<std::uint64_t>();
            if (last != 0) {
                EXPECT_EQ(seq, last + 1) << "silent gap before resync";
            }
            last = seq;
        } else if (type == "resync_required") {
            EXPECT_EQ((*m)["reason"], "slow_consumer");
            if (last != 0) {
                EXPECT_EQ((*m)["last_event_sequence"].get<std::uint64_t>(), last);
            }
        }
    }
    EXPECT_TRUE(slow.closed());
}

TEST(Server, ReconnectGetsFreshSnapshotAndContinues) {
    TestServer ts(true);
    Client trader = ts.connect();
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(trader.request(order("a" + std::to_string(i), "BUY", 9000 + i, 1), kTimeout));
    }
    std::uint64_t first_wm = 0;
    {
        Client sub = ts.connect();
        ASSERT_TRUE(sub.send(Json{{"type", "subscribe"}}));
        auto snap = sub.receive(kTimeout);
        ASSERT_TRUE(snap);
        first_wm = (*snap)["event_watermark"];
        EXPECT_EQ(first_wm, 5U);
        ASSERT_TRUE(trader.request(order("b", "SELL", 9500, 1), kTimeout));
        auto ev = sub.receive(kTimeout);
        ASSERT_TRUE(ev);
        EXPECT_EQ((*ev)["event_sequence"], 6);
    }  // disconnect
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(trader.request(order("c" + std::to_string(i), "BUY", 8000 + i, 1), kTimeout));
    }
    Client sub2 = ts.connect();
    ASSERT_TRUE(sub2.send(Json{{"type", "subscribe"}}));
    auto snap2 = sub2.receive(kTimeout);
    ASSERT_TRUE(snap2);
    EXPECT_EQ((*snap2)["event_watermark"], 9);
    ASSERT_TRUE(trader.request(order("d", "BUY", 8500, 1), kTimeout));
    auto ev2 = sub2.receive(kTimeout);
    ASSERT_TRUE(ev2);
    EXPECT_EQ((*ev2)["event_sequence"], 10);
    // A second subscribe on the same connection is refused.
    auto dup = sub2.request(Json{{"type", "subscribe"}, {"request_id", "again"}}, kTimeout);
    ASSERT_TRUE(dup);
    EXPECT_EQ((*dup)["type"], "error");
}

TEST(Server, RetryAfterDisconnectProducesOneMutation) {
    TestServer ts(true);
    {
        Client c = ts.connect();
        ASSERT_TRUE(c.send(order("x1", "SELL", 10000, 5)));
        // Disconnect without reading the ack (ambiguous outcome for the client).
    }
    Client c = ts.connect();
    auto r = c.request(order("x1", "SELL", 10000, 5), kTimeout);
    ASSERT_TRUE(r);
    EXPECT_EQ((*r)["type"], "ack");
    EXPECT_EQ((*r)["command_sequence"], 1);
    // Whether the first attempt was processed before the disconnect or not, the request id
    // maps to one command.
    auto r2 = c.request(order("x1", "SELL", 10000, 5), kTimeout);
    ASSERT_TRUE(r2);
    EXPECT_TRUE((*r2)["duplicate"].get<bool>());
    EXPECT_EQ((*r2)["command_sequence"], 1);
    auto conflict = c.request(order("x1", "SELL", 10000, 6), kTimeout);
    ASSERT_TRUE(conflict);
    EXPECT_EQ((*conflict)["code"], "DUPLICATE_REQUEST_ID_CONFLICT");
    ts.server->stop();
    EXPECT_EQ(ts.server->state().last_command_sequence(), 1U);
}

TEST(Server, ConnectionLimitIsEnforced) {
    ServerOptions o;
    o.max_clients = 3;
    TestServer ts(false, o);
    std::vector<Client> clients;
    for (int i = 0; i < 3; ++i) {
        clients.push_back(ts.connect());
        ASSERT_TRUE(
            clients.back().request(order("l" + std::to_string(i), "BUY", 100, 1), kTimeout));
    }
    Client extra = ts.connect();
    auto r = extra.receive(kTimeout);
    ASSERT_TRUE(r);
    EXPECT_EQ((*r)["code"], "SERVER_OVERLOADED");
    EXPECT_FALSE(extra.receive(kTimeout).has_value());
    EXPECT_EQ(ts.server->stats().connections_rejected, 1U);
    // Freeing a slot lets a new client in.
    clients.pop_back();
    std::optional<Json> ok;
    for (int attempt = 0; attempt < 50 && !ok; ++attempt) {
        std::this_thread::sleep_for(50ms);
        Client again = ts.connect();
        ok = again.request(order("late" + std::to_string(attempt), "BUY", 100, 1), 1s);
        if (ok && (*ok)["type"] != "ack") {
            ok.reset();
        }
    }
    EXPECT_TRUE(ok.has_value());
}

TEST(Server, IngressOverloadAnswersEveryRequestExactlyOnce) {
    ServerOptions o;
    o.ingress_capacity = 4;
    o.max_batch = 1;
    TestServer ts(true, o);  // per-command sync makes the sequencer slow enough to back up
    Client c = ts.connect();
    constexpr int kBurst = 300;
    std::string burst;
    for (int i = 0; i < kBurst; ++i) {
        burst += encode_frame(Json{{"protocol_version", 1},
                                   {"type", "new_limit"},
                                   {"request_id", "o" + std::to_string(i)},
                                   {"symbol", "AAPL"},
                                   {"side", "BUY"},
                                   {"price", 100 + i},
                                   {"quantity", 1}}
                                  .dump());
    }
    ASSERT_TRUE(c.send_raw(burst));
    std::map<std::string, Json> replies;
    int acks = 0;
    int overloaded = 0;
    while (static_cast<int>(replies.size()) < kBurst) {
        auto m = c.receive(10s);
        ASSERT_TRUE(m) << "missing replies: got " << replies.size();
        const std::string rid = (*m)["request_id"];
        ASSERT_FALSE(replies.contains(rid)) << "two replies for " << rid;
        replies[rid] = *m;
        if ((*m)["type"] == "ack") {
            ++acks;
        } else {
            ASSERT_EQ((*m)["code"], "SERVER_OVERLOADED");
            ++overloaded;
        }
    }
    EXPECT_GT(overloaded, 0);
    EXPECT_GT(acks, 0);
    ts.server->stop();
    EXPECT_EQ(ts.server->state().last_command_sequence(), static_cast<CommandSeq>(acks));
    EXPECT_EQ(ts.server->stats().overloaded, static_cast<std::uint64_t>(overloaded));
}

TEST(Server, ShutdownDrainsQueuedWorkAndRestartRecovers) {
    test::TempDir dir;
    const std::string journal = dir.file("restart.exj");
    std::string hash_before;
    std::uint64_t acked = 0;
    {
        ServerOptions o;
        TestServer ts(true, o, journal);
        Client c = ts.connect();
        Client sub = ts.connect();
        ASSERT_TRUE(sub.send(Json{{"type", "subscribe"}}));
        ASSERT_TRUE(sub.receive(kTimeout));
        std::string burst;
        for (int i = 0; i < 200; ++i) {
            burst += encode_frame(Json{{"protocol_version", 1},
                                       {"type", "new_limit"},
                                       {"request_id", "q" + std::to_string(i)},
                                       {"symbol", "MSFT"},
                                       {"side", i % 2 ? "BUY" : "SELL"},
                                       {"price", 20000 + i % 5},
                                       {"quantity", 3}}
                                      .dump());
        }
        ASSERT_TRUE(c.send_raw(burst));
        std::this_thread::sleep_for(20ms);
        ts.server->stop();  // with clients connected and requests possibly still queued
        while (auto m = c.receive(1s)) {
            acked += (*m)["type"] == "ack" ? 1U : 0U;
        }
        EXPECT_TRUE(c.closed());
        EXPECT_EQ(ts.server->state().last_command_sequence(), acked)
            << "every processed request was acknowledged before close";
        hash_before = state_hash(ts.server->state());
    }
    // Restart on the same journal: identical state, feed numbering continues.
    ServerOptions o;
    TestServer again(true, o, journal);
    EXPECT_EQ(again.server->recovery().records_replayed, acked);
    Client sub = again.connect();
    ASSERT_TRUE(sub.send(Json{{"type", "subscribe"}}));
    auto snap = sub.receive(kTimeout);
    ASSERT_TRUE(snap);
    EXPECT_EQ((*snap)["event_watermark"].get<std::uint64_t>(), acked);
    again.server->stop();
    EXPECT_EQ(state_hash(again.server->state()), hash_before);
}

TEST(Server, TrickledFrameIsDisconnectedAtDeadline) {
    ServerOptions o;
    o.partial_frame_timeout = 400ms;
    TestServer ts(false, o);
    Client c = ts.connect();
    const std::string f = encode_frame(std::string(2000, ' ') + "{}");
    const auto t0 = std::chrono::steady_clock::now();
    // One byte every 20 ms never lets the reader see an idle tick (slowloris pattern).
    std::size_t i = 0;
    for (; i < f.size() && !c.closed() && !c.write_failed(); ++i) {
        if (!c.send_raw(std::string_view(f).substr(i, 1))) {
            break;
        }
        std::this_thread::sleep_for(20ms);
        if (std::chrono::steady_clock::now() - t0 > 5s) {
            break;
        }
    }
    auto err = c.receive(kTimeout);
    ASSERT_TRUE(err);
    EXPECT_NE((*err)["message"].get<std::string>().find("timed out"), std::string::npos);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 3s);
    EXPECT_FALSE(c.receive(kTimeout).has_value());
}

TEST(Server, OutboundByteBudgetDisconnectsSnapshotHoarder) {
    ServerOptions o;
    o.outbound_byte_limit = 64 * 1024;
    o.socket_send_buffer = 8 * 1024;
    TestServer ts(false, o);
    {
        Client t = ts.connect();
        for (int i = 0; i < 300; ++i) {  // 300 price levels make each full snapshot large
            ASSERT_TRUE(t.request(order("lvl" + std::to_string(i), "BUY", 1000 + i, 1), kTimeout));
        }
    }
    Client hoarder = ts.connect();
    std::string burst;
    for (int i = 0; i < 400; ++i) {
        burst += encode_frame(Json{{"protocol_version", 1},
                                   {"type", "snapshot_request"},
                                   {"request_id", "h" + std::to_string(i)},
                                   {"depth", 0}}
                                  .dump());
    }
    ASSERT_TRUE(hoarder.send_raw(burst));  // and never read
    for (int i = 0; i < 100 && ts.server->stats().slow_consumer_disconnects == 0; ++i) {
        std::this_thread::sleep_for(20ms);
    }
    EXPECT_GE(ts.server->stats().slow_consumer_disconnects, 1U);
    Client ok = ts.connect();
    EXPECT_TRUE(ok.request(order("still-ok", "BUY", 100, 1), kTimeout));
}

TEST(Server, RefusesNonLoopbackBindWithoutOptIn) {
    ServerOptions o;
    o.host = "0.0.0.0";
    o.port = 0;
    EXPECT_THROW(Server{o}, std::invalid_argument);
}

TEST(Server, JournalFailureFailStopsOverTheNetwork) {
    test::TempDir dir;
    ServerOptions o;
    o.host = "127.0.0.1";
    o.port = 0;
    DurableOptions d;
    d.journal_path = dir.file("fail.exj");
    d.faults = journal::FaultPlan::parse("fail_sync=2");
    o.journal = d;
    Server server(o);
    server.start();
    Client c = Client::connect("127.0.0.1", server.port(), kTimeout);
    auto ok = c.request(order("a", "BUY", 100, 1), kTimeout);
    ASSERT_TRUE(ok);
    EXPECT_EQ((*ok)["type"], "ack");
    auto failed = c.request(order("b", "BUY", 100, 1), kTimeout);
    ASSERT_TRUE(failed);
    EXPECT_EQ((*failed)["code"], "JOURNAL_IO_ERROR");
    auto after = c.request(order("c", "BUY", 100, 1), kTimeout);
    ASSERT_TRUE(after);
    EXPECT_EQ((*after)["code"], "JOURNAL_IO_ERROR");
    auto snap = c.request(Json{{"type", "snapshot_request"}, {"request_id", "s"}}, kTimeout);
    ASSERT_TRUE(snap);
    EXPECT_EQ((*snap)["code"], "JOURNAL_IO_ERROR");
    EXPECT_TRUE(server.journal_failed());
    server.stop();
}

}  // namespace
}  // namespace enginex::net
