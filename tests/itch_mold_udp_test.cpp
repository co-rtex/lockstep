// MoldUDP64 over real UDP sockets on loopback: request parsing, the packet store, and a sender
// and receiver in two threads with deliberate loss on both the live feed and retransmissions.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "enginex/itch/book.hpp"
#include "enginex/itch/decoder.hpp"
#include "enginex/itch/mold_udp.hpp"
#include "enginex/itch/synthetic.hpp"
#include "enginex/socket.hpp"

namespace enginex::itch::mold {
namespace {

using namespace std::chrono_literals;

const Session kSession = make_session("UDPTEST");

TEST(MoldRequest, ParsesOnlyWellFormedRequests) {
    const auto req = build_request(kSession, 1234, 7);
    const auto q = parse_request(req);
    ASSERT_TRUE(q);
    EXPECT_EQ(q->session, kSession);
    EXPECT_EQ(q->sequence, 1234U);
    EXPECT_EQ(q->count, 7U);
    EXPECT_FALSE(parse_request(build_request(kSession, 1234, 0)));       // count 0
    EXPECT_FALSE(parse_request(build_request(kSession, 1234, 0xFFFF)));  // end-of-session code
    EXPECT_FALSE(parse_request(build_request(kSession, 0, 3)));          // sequences start at 1
    auto longer = req;
    longer.push_back(0);
    EXPECT_FALSE(parse_request(longer));
    EXPECT_FALSE(parse_request(std::span<const std::uint8_t>(req.data(), 19)));
    EXPECT_FALSE(parse_request(build_request(kSession, UINT64_MAX, 2)));  // would wrap
}

TEST(MoldRequestPlanner, RetriesOnlyAfterTheTimerEvenWhenAHolePartlyFills) {
    using R = std::vector<std::pair<std::uint64_t, std::uint16_t>>;
    udp::RequestPlanner p(50ms, 16, 4096);
    const auto t0 = udp::RequestPlanner::Clock::now();
    EXPECT_EQ(p.plan({{100, 500}}, 100, t0), (R{{100, 500}}));
    // Part of the answer arrived: the hole is now [110, 600). Still covered: nothing to ask.
    EXPECT_EQ(p.plan({{110, 490}}, 110, t0 + 5ms), R{});
    // A fill in the middle split it: still covered.
    EXPECT_EQ(p.plan({{110, 90}, {201, 399}}, 110, t0 + 10ms), R{});
    // The hole grew past the request (e.g. a heartbeat): only the new tail is asked.
    EXPECT_EQ(p.plan({{110, 90}, {201, 499}}, 110, t0 + 15ms), (R{{600, 100}}));
    // After the retry timer, what is still missing is asked again.
    EXPECT_EQ(p.plan({{110, 90}, {201, 499}}, 110, t0 + 60ms), (R{{110, 90}, {201, 399}}));
    // Long holes are asked in pieces of at most max_messages; ranges per round are capped.
    udp::RequestPlanner q(50ms, 2, 1000);
    EXPECT_EQ(q.plan({{1, 2500}, {3000, 5}, {4000, 5}}, 1, t0), (R{{1, 1000}, {3000, 5}}));
    EXPECT_EQ(q.plan({{1, 2500}, {3000, 5}, {4000, 5}}, 1, t0 + 1ms), (R{{1001, 1000}, {4000, 5}}));
}

TEST(MoldPacketStore, PacketsAreContiguousWithinTheMtuAndFindable) {
    PacketStore store(kSession, 100);
    for (std::uint8_t i = 1; i <= 50; ++i) {
        store.append(std::vector<std::uint8_t>(i % 9 + 1, i));
    }
    store.append(std::vector<std::uint8_t>(300, 0xAB));  // larger than the MTU: sent alone
    store.finish();
    EXPECT_EQ(store.end_sequence(), 52U);
    std::uint64_t next = 1;
    for (std::size_t i = 0; i < store.size(); ++i) {
        EXPECT_EQ(store.first(i), next);
        ParsedPacket p;
        ASSERT_FALSE(parse(store.packet(i), p));
        EXPECT_EQ(p.header.sequence, next);
        EXPECT_EQ(p.messages.size(), store.count(i));
        if (store.count(i) > 1) {
            EXPECT_LE(store.packet(i).size(), 100U);
        }
        for (std::uint64_t s = next; s < next + store.count(i); ++s) {
            ASSERT_EQ(store.find(s), i) << s;
        }
        next += store.count(i);
    }
    EXPECT_EQ(next, 52U);
    EXPECT_FALSE(store.find(0));
    EXPECT_FALSE(store.find(52));
}

struct Feed {
    std::vector<Message> stream;
    PacketStore store{kSession, 1400};
    std::string full_hash;
};

Feed make_feed(std::size_t messages) {
    Feed f;
    SyntheticOptions so;
    so.seed = 21;
    so.messages = messages;
    so.symbols = 4;
    f.stream = generate(so);
    BookBuilder direct;
    for (const Message& m : f.stream) {
        f.store.append(encode(m));
        (void)direct.apply(m);
    }
    f.store.finish();
    f.full_hash = direct.state_hash();
    return f;
}

std::string prefix_hash(const std::vector<Message>& stream, std::uint64_t n) {
    BookBuilder b;
    for (std::uint64_t i = 0; i < n; ++i)
        (void)b.apply(stream[i]);
    return b.state_hash();
}

struct Outcome {
    udp::SenderReport sent;
    udp::ReceiverReport received;
    std::string book_hash;
};

Outcome transfer(const Feed& f, udp::SenderOptions so, udp::ReceiverOptions ro,
                 const std::function<void(std::uint16_t, std::uint16_t)>& before = {},
                 std::chrono::milliseconds receiver_delay = 0ms) {
    ro.sequencer.session = kSession;
    udp::Receiver rx(ro);
    so.dest.port = rx.port();
    udp::Sender tx(f.store, so);
    rx.set_request_port(tx.port());
    if (before)
        before(tx.port(), rx.port());
    Outcome o;
    BookBuilder book;
    std::thread sender([&] { o.sent = tx.run(); });
    std::this_thread::sleep_for(receiver_delay);  // datagrams pile up in the socket buffer
    o.received = rx.run([&](std::uint64_t, std::span<const std::uint8_t> m) {
        const DecodeResult d = decode(m);
        if (const auto* msg = std::get_if<Message>(&d))
            (void)book.apply(*msg);
    });
    sender.join();
    o.book_hash = book.state_hash();
    return o;
}

TEST(MoldUdp, LossyLoopbackRebuildsTheExactBook) {
    const Feed f = make_feed(30'000);
    udp::SenderOptions so;
    so.drop_percent = 5;
    so.retransmit_drop_percent = 10;
    so.seed = 3;
    // Recovery needs the sender to still be answering. 300 ms was too short for a starved
    // receiver in a loaded UBSan -j8 run (it ended in "gap" at 14 090 of 30 010 messages);
    // the same reason the unpaced-burst test lingers 2 s.
    so.linger = 2000ms;
    const Outcome o = transfer(f, so, udp::ReceiverOptions{});
    EXPECT_GT(o.sent.dropped, 0U);
    EXPECT_GT(o.sent.requests, 0U);
    EXPECT_GT(o.sent.retransmit_dropped, 0U);
    EXPECT_GT(o.received.requests_sent, 0U);
    EXPECT_GT(o.received.gaps_opened, 0U);
    EXPECT_EQ(o.received.final_state, "ended");
    EXPECT_EQ(o.received.delivered, f.stream.size());
    EXPECT_TRUE(o.received.in_order);
    EXPECT_EQ(o.book_hash, f.full_hash);
}

// The answer budget (anti-amplification) can starve recovery for a while; the receiver's
// retries must still complete the book once tokens come back.
TEST(MoldUdp, RateLimitedAnswersStillCompleteTheBook) {
    const Feed f = make_feed(30'000);
    udp::SenderOptions so;
    so.drop_percent = 5;
    so.seed = 8;
    so.linger = 2000ms;  // recovery needs a sender that is still answering (see above)
    so.max_answer_packets_per_second = 100;  // 10-packet burst, then 1 per 10 ms
    const Outcome o = transfer(f, so, udp::ReceiverOptions{});
    EXPECT_GT(o.sent.answers_rate_limited, 0U);
    EXPECT_EQ(o.received.final_state, "ended");
    EXPECT_EQ(o.book_hash, f.full_hash);
}

TEST(MoldUdp, WithoutRecoveryTheBookStopsExactlyAtTheHole) {
    const Feed f = make_feed(10'000);
    udp::SenderOptions so;
    so.drop_percent = 5;
    so.seed = 4;
    so.linger = 100ms;
    udp::ReceiverOptions ro;
    ro.recovery = false;
    ro.idle_timeout = 300ms;
    const Outcome o = transfer(f, so, ro);
    EXPECT_GT(o.sent.dropped, 0U);
    EXPECT_EQ(o.sent.requests, 0U);
    EXPECT_EQ(o.received.final_state, "gap");
    EXPECT_LT(o.received.delivered, f.stream.size());
    EXPECT_TRUE(o.received.in_order);
    EXPECT_EQ(o.book_hash, prefix_hash(f.stream, o.received.delivered));
}

// Stray datagrams queued before the feed starts: garbage and a well-formed data packet from
// another session (which, without a configured session, would claim the sequencer), plus bad
// requests to the sender. The book must still be exact.
TEST(MoldUdp, HostileDatagramsAreCountedAndIgnored) {
    const Feed f = make_feed(5'000);
    udp::SenderOptions so;
    so.drop_percent = 3;
    so.seed = 5;
    so.linger = 300ms;
    const Outcome o =
        transfer(f, so, udp::ReceiverOptions{}, [&](std::uint16_t tx, std::uint16_t rx) {
            net::Socket s(::socket(AF_INET, SOCK_DGRAM, 0));
            ASSERT_TRUE(s.valid());
            auto send = [&](std::uint16_t port, const std::vector<std::uint8_t>& bytes) {
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_port = htons(port);
                a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                ASSERT_EQ(::sendto(s.fd(), bytes.data(), bytes.size(), 0,
                                   reinterpret_cast<const sockaddr*>(&a), sizeof a),
                          static_cast<ssize_t>(bytes.size()));
            };
            send(rx, {1, 2, 3});
            send(rx, build_packet(make_session("INTRUDER"), 1, {encode(f.stream[0])}));
            send(tx, {9, 9, 9, 9, 9});
            send(tx, build_request(make_session("INTRUDER"), 1, 5));
            send(tx, build_request(kSession, 10'000'000, 5));  // out of range
        });
    EXPECT_GE(o.received.malformed, 1U);
    EXPECT_GE(o.received.foreign_session, 1U);
    EXPECT_GE(o.sent.bad_requests, 3U);
    EXPECT_EQ(o.received.final_state, "ended");
    EXPECT_EQ(o.book_hash, f.full_hash);
}

// An unpaced burst into a small socket buffer that nobody reads for 100 ms: the kernel itself
// drops most datagrams (how many depends on the OS). Recovery must rebuild the exact book.
TEST(MoldUdp, UnpacedBurstIntoASmallSocketBufferStillRebuildsTheExactBook) {
    const Feed f = make_feed(100'000);
    udp::SenderOptions so;
    so.packets_per_second = 0;
    // Generous timeouts: this test starts the receiver late on purpose, and under a loaded
    // parallel ctest run a starved receiver must still find the sender answering (one
    // failure was seen with a 500 ms linger in a full -j8 run; not reproducible alone).
    so.linger = 2000ms;
    udp::ReceiverOptions ro;
    ro.receive_buffer_bytes = 32 << 10;
    ro.idle_timeout = 5000ms;
    // The CLI's reorder bound (`mold-recv`, 2^20 messages), not the library default (65 536).
    // With the default, a starved receiver in a loaded -j8 run buffered 65 536 messages behind
    // the first hole before its answer arrived and fail-stopped by design (buffered peak 65 536,
    // state failed): correct behaviour, but not what this test is about.
    ro.sequencer.max_buffered_messages = std::size_t{1} << 20;
    const Outcome o = transfer(f, so, ro, {}, 100ms);
    std::printf(
        "unpaced burst: %llu packets sent, %llu gaps, %llu requests, %llu retransmitted, "
        "buffered peak %llu, delivered %llu, state %s\n",
        static_cast<unsigned long long>(o.sent.sent),
        static_cast<unsigned long long>(o.received.gaps_opened),
        static_cast<unsigned long long>(o.received.requests_sent),
        static_cast<unsigned long long>(o.sent.retransmitted),
        static_cast<unsigned long long>(o.received.buffered_peak),
        static_cast<unsigned long long>(o.received.delivered), o.received.final_state.c_str());
    EXPECT_GT(o.received.gaps_opened, 0U);  // the kernel did drop datagrams
    EXPECT_EQ(o.received.final_state, "ended");
    EXPECT_EQ(o.received.delivered, f.stream.size());
    EXPECT_EQ(o.book_hash, f.full_hash);
}

// The receiver accepts feed datagrams only from the expected address: with the feed expected
// from 127.0.0.2, everything the real sender (127.0.0.1) sends is rejected before sequencing.
TEST(MoldUdp, DatagramsFromAnUnexpectedSourceAreRejected) {
    const Feed f = make_feed(2'000);
    udp::SenderOptions so;
    so.linger = 100ms;
    udp::ReceiverOptions ro;
    ro.feed_from = "127.0.0.2";
    ro.recovery = false;
    ro.idle_timeout = 300ms;
    const Outcome o = transfer(f, so, ro);
    EXPECT_GT(o.received.foreign_source, 0U);
    EXPECT_EQ(o.received.delivered, 0U);
    EXPECT_TRUE(o.received.timed_out);
}

// A gap that can never be filled (every retransmission is lost) must end the run. It used to
// run forever: the sender's repeated end-of-session packets kept resetting the receiver's
// idle timer, and the receiver's requests kept extending the sender's linger. Now a receiver
// in the gap state gives up after `idle_timeout` without accepting a new message. A watchdog
// stops both sides after 15 s so a regression fails instead of hanging the suite.
TEST(MoldUdp, AGapThatCanNeverBeFilledEndsTheRunInsteadOfWaitingForever) {
    const Feed f = make_feed(5'000);
    udp::ReceiverOptions ro;
    ro.sequencer.session = kSession;
    ro.idle_timeout = 1000ms;
    udp::Receiver rx(ro);
    udp::SenderOptions so;
    so.dest.port = rx.port();
    so.drop_percent = 10;
    so.retransmit_drop_percent = 100;
    so.seed = 3;
    so.linger = 300ms;
    udp::Sender tx(f.store, so);
    rx.set_request_port(tx.port());
    std::atomic<bool> stop{false};
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        for (int i = 0; i < 150 && !done.load(); ++i)
            std::this_thread::sleep_for(100ms);
        stop = true;
    });
    udp::SenderReport sent;
    std::thread sender([&] { sent = tx.run(&stop); });
    const auto t0 = std::chrono::steady_clock::now();
    const udp::ReceiverReport r =
        rx.run([](std::uint64_t, std::span<const std::uint8_t>) {}, &stop);
    const double receiver_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    sender.join();
    const bool watchdog_fired = stop.load();
    done = true;
    watchdog.join();
    EXPECT_FALSE(watchdog_fired) << "receiver " << receiver_s
                                 << " s: the run did not end by itself";
    EXPECT_GT(sent.dropped, 0U);
    EXPECT_GT(r.requests_sent, 0U);
    EXPECT_EQ(r.final_state, "gap");
    EXPECT_TRUE(r.timed_out);
    EXPECT_TRUE(r.gap_stalled);
    EXPECT_LT(r.delivered, f.stream.size());
}

// A UDP socket on 127.0.0.1 with an ephemeral port: sends datagrams, counts what comes back.
struct ScriptedPeer {
    net::Socket s{::socket(AF_INET, SOCK_DGRAM, 0)};
    std::uint16_t port = 0;
    ScriptedPeer() {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof a;
        if (!s.valid() || ::bind(s.fd(), reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0 ||
            ::getsockname(s.fd(), reinterpret_cast<sockaddr*>(&a), &len) != 0) {
            throw std::runtime_error("scripted peer socket");
        }
        port = ntohs(a.sin_port);
    }
    void send_to(std::uint16_t to, const std::vector<std::uint8_t>& bytes) const {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(to);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::sendto(s.fd(), bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&a),
                     sizeof a) != static_cast<ssize_t>(bytes.size())) {
            throw std::runtime_error("scripted peer send");
        }
    }
    // Waits up to `timeout` for one datagram.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> wait(
        std::chrono::milliseconds timeout) const {
        pollfd p{s.fd(), POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(timeout.count())) <= 0)
            return std::nullopt;
        std::vector<std::uint8_t> buf(2048);
        const ssize_t n = ::recv(s.fd(), buf.data(), buf.size(), MSG_DONTWAIT);
        if (n < 0)
            return std::nullopt;
        buf.resize(static_cast<std::size_t>(n));
        return buf;
    }
    [[nodiscard]] std::size_t drain() const {
        std::size_t n = 0;
        std::uint8_t buf[2048];
        while (::recv(s.fd(), buf, sizeof buf, MSG_DONTWAIT) >= 0)
            ++n;
        return n;
    }
};

// The stall rule above measures time from when the gap opened, not from the last accepted
// message. A sparse feed accepts message 1, then sends only heartbeats for longer than the
// idle timeout. A heartbeat then reveals that messages 2..3 were lost. The receiver must
// re-request them, not give up at once (an independent review found that it did), and end
// exact when they arrive. The feed is scripted; the script plays the re-request server.
TEST(MoldUdp, AGapRevealedAfterAQuietSpellIsStillRecovered) {
    const ScriptedPeer feed;
    udp::ReceiverOptions ro;
    ro.sequencer.session = kSession;
    ro.idle_timeout = 500ms;
    udp::Receiver rx(ro);
    rx.set_request_port(feed.port);
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    udp::ReceiverReport r;  // read only after join()
    std::thread receiver([&] {
        r = rx.run([](std::uint64_t, std::span<const std::uint8_t>) {}, &stop);
        finished = true;
    });
    const std::vector<std::uint8_t> m1{1}, m2{2}, m3{3};
    bool requested = false;
    try {
        feed.send_to(rx.port(), build_packet(kSession, 1, {m1}));
        for (int i = 0; i < 9; ++i) {  // 900 ms of heartbeats: no new message, still alive
            std::this_thread::sleep_for(100ms);
            feed.send_to(rx.port(), build_control(kSession, 2, kHeartbeat));
        }
        feed.send_to(rx.port(), build_control(kSession, 4, kHeartbeat));  // 2..3 were lost
        if (const auto req = feed.wait(1000ms)) {
            const auto parsed = parse_request(*req);
            requested = parsed && parsed->sequence == 2;
            feed.send_to(rx.port(), build_packet(kSession, 2, {m2, m3}));
            feed.send_to(rx.port(), build_control(kSession, 4, kEndOfSession));
        }
    } catch (...) {
        stop = true;
        receiver.join();
        throw;
    }
    for (int i = 0; i < 50 && !finished.load(); ++i)
        std::this_thread::sleep_for(100ms);
    stop = true;
    receiver.join();
    EXPECT_TRUE(requested) << "final state " << r.final_state << ", gap_stalled " << r.gap_stalled;
    EXPECT_FALSE(r.gap_stalled);
    EXPECT_EQ(r.final_state, "ended");
    EXPECT_EQ(r.delivered, 3U);
}

// Reflection limits of the retransmission server, with a scripted requester and the feed sent
// to a socket nobody reads. One request asking for 60 000 messages is answered only up to the
// per-request cap (the packets holding messages 1..10), not with the whole store.
TEST(MoldUdp, OneRequestIsAnsweredOnlyUpToItsCap) {
    const Feed f = make_feed(20'000);
    const ScriptedPeer sink;
    const ScriptedPeer requester;
    udp::SenderOptions so;
    so.dest.port = sink.port;
    so.linger = 300ms;
    so.max_request_messages = 10;
    udp::Sender tx(f.store, so);
    requester.send_to(tx.port(), build_request(kSession, 1, 60'000));  // queued before run()
    const udp::SenderReport r = tx.run();
    std::uint64_t expected = 0;
    for (std::size_t i = 0; i < f.store.size() && f.store.first(i) < 1 + 10; ++i)
        ++expected;
    ASSERT_GE(f.store.size(), 100U * expected);  // the uncapped answer would be far larger
    EXPECT_EQ(r.requests, 1U);
    EXPECT_EQ(r.retransmitted + r.answers_rate_limited, expected);
    EXPECT_EQ(requester.drain(), r.retransmitted);
}

// Anti-reflection: the sender answers only the receiver's address (`dest.host`). The receiver
// is configured as 0.0.0.0: a UDP connect() there reaches the local host (macOS and Linux),
// but no datagram ever comes from 0.0.0.0, so the requester at 127.0.0.1 is "another
// address". (127.0.0.2 would be more natural, but macOS has only 127.0.0.1 on lo0.) The same
// request with `answer_any_source` is answered, so the refusal is the source check alone.
TEST(MoldUdp, RequestsFromAnAddressOtherThanTheReceiversAreNotAnswered) {
    const Feed f = make_feed(2'000);
    const ScriptedPeer sink;
    for (const bool any_source : {false, true}) {
        const ScriptedPeer requester;
        udp::SenderOptions so;
        so.dest.host = "0.0.0.0";
        so.dest.port = sink.port;
        so.linger = 300ms;
        so.answer_any_source = any_source;
        udp::Sender tx(f.store, so);
        requester.send_to(tx.port(), build_request(kSession, 1, 5));
        const udp::SenderReport r = tx.run();
        if (!any_source) {
            EXPECT_EQ(r.foreign_requests, 1U);
            EXPECT_EQ(r.requests, 0U);
            EXPECT_EQ(r.retransmitted, 0U);
            EXPECT_EQ(requester.drain(), 0U);
        } else {
            EXPECT_EQ(r.foreign_requests, 0U);
            EXPECT_EQ(r.requests, 1U);
            EXPECT_GE(r.retransmitted, 1U);
            EXPECT_EQ(requester.drain(), r.retransmitted);
        }
    }
}

// The request socket allows SO_REUSEPORT (answer sockets share its port), but a second
// sender must not silently share an explicitly chosen port.
TEST(MoldUdp, ASecondSenderCannotTakeAPortInUse) {
    const Feed f = make_feed(100);
    udp::SenderOptions so;
    so.dest.port = 9;  // never used: run() is not called
    udp::Sender first(f.store, so);
    so.bind.port = first.port();
    EXPECT_THROW(udp::Sender second(f.store, so), std::runtime_error);
}

TEST(MoldUdp, LoopbackCheck) {
    EXPECT_TRUE(udp::is_loopback("127.0.0.1"));
    EXPECT_TRUE(udp::is_loopback("127.1.2.3"));
    EXPECT_FALSE(udp::is_loopback("10.0.0.1"));
    EXPECT_FALSE(udp::is_loopback("localhost"));  // literals only
    EXPECT_FALSE(udp::is_loopback("0.0.0.0"));
}

}  // namespace
}  // namespace enginex::itch::mold
