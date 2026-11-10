// MoldUDP64 packet parsing, sequencing/gap handling, and an end-to-end lossy-network replay.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <set>

#include "enginex/itch/book.hpp"
#include "enginex/itch/decoder.hpp"
#include "enginex/itch/mold_sim.hpp"
#include "enginex/itch/moldudp64.hpp"
#include "enginex/itch/synthetic.hpp"
#include "enginex/rng.hpp"

namespace enginex::itch::mold {
namespace {

std::vector<std::uint8_t> msg(std::uint8_t tag, std::size_t n = 3) {
    return std::vector<std::uint8_t>(n, tag);
}

TEST(MoldParse, GoldenPacketBytes) {
    // Session "SESSION001", sequence 0x0102030405060708, 2 blocks: [AA BB] and [CC].
    const std::vector<std::uint8_t> bytes = {
        'S', 'E', 'S', 'S', 'I', 'O', 'N', '0', '0', '1', 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x00, 0x02, 0x00, 0x02, 0xAA, 0xBB, 0x00, 0x01, 0xCC};
    ParsedPacket p;
    ASSERT_FALSE(parse(bytes, p));
    EXPECT_EQ(std::string(p.header.session.data(), 10), "SESSION001");
    EXPECT_EQ(p.header.sequence, 0x0102030405060708ULL);
    EXPECT_EQ(p.header.count, 2U);
    ASSERT_EQ(p.messages.size(), 2U);
    EXPECT_EQ(std::vector<std::uint8_t>(p.messages[0].begin(), p.messages[0].end()),
              (std::vector<std::uint8_t>{0xAA, 0xBB}));
    EXPECT_EQ(p.messages[1].size(), 1U);
    EXPECT_EQ(build_packet(make_session("SESSION001"), 0x0102030405060708ULL,
                           {{0xAA, 0xBB}, {0xCC}}),
              bytes);
}

TEST(MoldParse, ControlPackets) {
    const Session s = make_session("S1");
    ParsedPacket p;
    ASSERT_FALSE(parse(build_control(s, 42, kHeartbeat), p));
    EXPECT_EQ(p.header.count, kHeartbeat);
    EXPECT_EQ(p.header.sequence, 42U);
    EXPECT_TRUE(p.messages.empty());
    ASSERT_FALSE(parse(build_control(s, 43, kEndOfSession), p));
    EXPECT_EQ(p.header.count, kEndOfSession);
    EXPECT_EQ(build_request(s, 7, 3).size(), kHeaderSize);
    EXPECT_THROW((void)build_control(s, 1, 5), std::invalid_argument);
}

TEST(MoldParse, MalformedPacketsDeliverNothing) {
    const Session s = make_session("S1");
    ParsedPacket p;
    const auto good = build_packet(s, 1, {msg(1), msg(2)});
    for (std::size_t n = 0; n < kHeaderSize; ++n) {
        EXPECT_EQ(parse(std::span(good).first(n), p), PacketError::TooShort) << n;
    }
    for (std::size_t n = kHeaderSize; n < good.size(); ++n) {  // every truncation
        EXPECT_EQ(parse(std::span(good).first(n), p), PacketError::BlockOverrun) << n;
        EXPECT_TRUE(p.messages.empty());
    }
    auto trailing = good;
    trailing.push_back(0);
    EXPECT_EQ(parse(trailing, p), PacketError::TrailingBytes);
    EXPECT_TRUE(p.messages.empty());
    auto hb = build_control(s, 1, kHeartbeat);
    hb.push_back(0);
    EXPECT_EQ(parse(hb, p), PacketError::ControlWithPayload);
    const auto wrap = build_packet(s, UINT64_MAX, {msg(1), msg(2)});
    EXPECT_EQ(parse(wrap, p), PacketError::SequenceOverflow);
}

TEST(MoldParse, RandomBytesNeverCrash) {
    Rng rng(64);
    ParsedPacket p;
    for (int i = 0; i < 100'000; ++i) {
        std::vector<std::uint8_t> b(rng.below(64));
        for (auto& x : b) x = static_cast<std::uint8_t>(rng.next());
        if (b.size() >= kHeaderSize && rng.chance(50)) {
            b[18] = 0;
            b[19] = static_cast<std::uint8_t>(rng.below(4));  // plausible small counts
        }
        if (!parse(b, p)) {
            std::size_t total = kHeaderSize;
            for (const auto& m : p.messages) total += 2 + m.size();
            ASSERT_EQ(total, b.size());
        }
    }
}

// ---- sequencer ------------------------------------------------------------------------------

struct Sink {
    std::vector<std::uint64_t> seqs;
    std::vector<std::uint8_t> tags;
    void operator()(std::uint64_t s, std::span<const std::uint8_t> m) {
        seqs.push_back(s);
        tags.push_back(m.empty() ? 0 : m[0]);
    }
};

const Session kS = make_session("TEST");

TEST(MoldSequencer, InOrderDuplicatesAndOverlap) {
    Sequencer q;
    Sink out;
    q.on_packet(build_packet(kS, 1, {msg(1), msg(2)}), out);
    q.on_packet(build_packet(kS, 1, {msg(1), msg(2)}), out);          // duplicate packet
    q.on_packet(build_packet(kS, 2, {msg(2), msg(3), msg(4)}), out);  // overlaps message 2
    EXPECT_EQ(out.seqs, (std::vector<std::uint64_t>{1, 2, 3, 4}));
    EXPECT_EQ(out.tags, (std::vector<std::uint8_t>{1, 2, 3, 4}));
    EXPECT_EQ(q.stats().duplicates, 3U);
    EXPECT_EQ(q.state(), Sequencer::State::Running);
    EXPECT_EQ(q.next_expected(), 5U);
}

TEST(MoldSequencer, ReorderedPacketsAreReleasedInOrder) {
    Sequencer q;
    Sink out;
    q.on_packet(build_packet(kS, 1, {msg(1), msg(2)}), out);
    q.on_packet(build_packet(kS, 5, {msg(5), msg(6)}), out);
    EXPECT_EQ(q.state(), Sequencer::State::Gap);
    EXPECT_EQ(q.missing(), (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{3, 2}}));
    EXPECT_EQ(out.seqs.size(), 2U);  // nothing after the hole is released
    q.on_packet(build_packet(kS, 3, {msg(3), msg(4)}), out);
    EXPECT_EQ(out.seqs, (std::vector<std::uint64_t>{1, 2, 3, 4, 5, 6}));
    EXPECT_EQ(q.state(), Sequencer::State::Running);
    EXPECT_EQ(q.stats().gaps_opened, 1U);
    EXPECT_EQ(q.stats().gaps_closed, 1U);
    EXPECT_EQ(q.stats().buffered_peak, 2U);
    EXPECT_TRUE(q.missing().empty());
}

TEST(MoldSequencer, HeartbeatRevealsAGapWithoutData) {
    Sequencer q;
    Sink out;
    q.on_packet(build_packet(kS, 1, {msg(1)}), out);
    q.on_packet(build_control(kS, 4, kHeartbeat), out);  // messages 2 and 3 exist
    EXPECT_EQ(q.state(), Sequencer::State::Gap);
    EXPECT_EQ(q.missing(), (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{2, 2}}));
    q.on_packet(build_packet(kS, 2, {msg(2), msg(3)}), out);  // retransmission
    EXPECT_EQ(q.state(), Sequencer::State::Running);
    EXPECT_EQ(out.seqs, (std::vector<std::uint64_t>{1, 2, 3}));
    EXPECT_EQ(q.stats().heartbeats, 1U);
}

TEST(MoldSequencer, MissingRangesAroundSeveralHoles) {
    Sequencer q;
    Sink out;
    q.on_packet(build_packet(kS, 3, {msg(3)}), out);
    q.on_packet(build_packet(kS, 6, {msg(6), msg(7)}), out);
    q.on_packet(build_control(kS, 10, kHeartbeat), out);
    EXPECT_EQ(q.missing(),
              (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{1, 2}, {4, 2}, {8, 2}}));
    EXPECT_TRUE(out.seqs.empty());
}

TEST(MoldSequencer, EndOfSession) {
    Sequencer q;
    Sink out;
    q.on_packet(build_packet(kS, 1, {msg(1), msg(2)}), out);
    q.on_packet(build_control(kS, 3, kEndOfSession), out);
    EXPECT_EQ(q.state(), Sequencer::State::Ended);
    Sequencer r;  // end-of-session that reveals missing messages is a gap, not an end
    Sink o2;
    r.on_packet(build_packet(kS, 1, {msg(1)}), o2);
    r.on_packet(build_control(kS, 3, kEndOfSession), o2);
    EXPECT_EQ(r.state(), Sequencer::State::Gap);
    r.on_packet(build_packet(kS, 2, {msg(2)}), o2);
    EXPECT_EQ(r.state(), Sequencer::State::Ended);
}

TEST(MoldSequencer, BufferLimitFailsInsteadOfSkipping) {
    Sequencer::Options o;
    o.max_buffered_messages = 3;
    Sequencer q(o);
    Sink out;
    q.on_packet(build_packet(kS, 2, {msg(2), msg(3), msg(4)}), out);  // 1 is missing
    EXPECT_EQ(q.state(), Sequencer::State::Gap);
    q.on_packet(build_packet(kS, 5, {msg(5)}), out);  // would exceed the bound
    EXPECT_EQ(q.state(), Sequencer::State::Failed);
    q.on_packet(build_packet(kS, 1, {msg(1)}), out);  // too late: nothing is delivered
    EXPECT_TRUE(out.seqs.empty());
}

TEST(MoldSequencer, DuplicatesOfBufferedMessagesUseNoCapacity) {
    // Retransmissions of a message already held behind a hole are duplicates. They must not be
    // counted against the buffer bound (a lossy channel would otherwise fail spuriously) or
    // leave the accounting behind once the hole fills.
    Sequencer::Options o;
    o.max_buffered_messages = 2;
    o.max_buffered_bytes = 8;
    Sequencer q(o);
    Sink out;
    for (int i = 0; i < 5; ++i) q.on_packet(build_packet(kS, 2, {msg(2)}), out);  // 1 is missing
    EXPECT_EQ(q.state(), Sequencer::State::Gap);
    EXPECT_EQ(q.buffered(), 1U);
    EXPECT_EQ(q.stats().duplicates, 4U);
    EXPECT_EQ(q.stats().buffered_bytes_peak, 3U);
    q.on_packet(build_packet(kS, 1, {msg(1)}), out);
    EXPECT_EQ(out.seqs, (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(q.buffered(), 0U);
    q.on_packet(build_packet(kS, 4, {msg(4), msg(5)}), out);  // the full bound is still usable
    EXPECT_EQ(q.state(), Sequencer::State::Gap);
    q.on_packet(build_packet(kS, 3, {msg(3)}), out);
    EXPECT_EQ(out.seqs, (std::vector<std::uint64_t>{1, 2, 3, 4, 5}));
    EXPECT_EQ(q.state(), Sequencer::State::Running);
}

TEST(MoldSequencer, ConfiguredSessionCannotBeClaimedByAStrayPacket) {
    const auto stray = build_packet(make_session("INTRUDER"), 1, {msg(9)});
    const auto real = build_packet(kS, 1, {msg(1), msg(2)});
    Sequencer open;  // no configured session: the first packet seen fixes it
    Sink a;
    open.on_packet(stray, a);
    open.on_packet(real, a);
    EXPECT_EQ(a.tags, (std::vector<std::uint8_t>{9}));  // the stray packet won
    EXPECT_EQ(open.stats().foreign_session_packets, 1U);
    Sequencer::Options o;
    o.session = kS;
    Sequencer fixed(o);
    Sink b;
    fixed.on_packet(stray, b);
    fixed.on_packet(real, b);
    EXPECT_EQ(b.tags, (std::vector<std::uint8_t>{1, 2}));
    EXPECT_EQ(fixed.stats().foreign_session_packets, 1U);
}

// Each packet brings one new message plus many already-buffered ones, so it is mostly
// duplicates. Its copy still costs memory and must count against the byte bound: otherwise
// a few-byte payload could pin a whole packet copy each time, far beyond the bound.
TEST(MoldSequencer, PacketCopiesCountAgainstTheByteBound) {
    Sequencer::Options o;
    o.max_buffered_bytes = 1 << 20;
    Sequencer q(o);
    Sink out;
    auto packet = [&](std::uint64_t first) {
        std::vector<std::vector<std::uint8_t>> m(1000, std::vector<std::uint8_t>(10, 7));
        return build_packet(kS, first, m);
    };
    q.on_packet(packet(10'000), out);  // 1 is missing: all buffered (≈ 12 KB copy)
    std::size_t sent = 1;
    for (std::uint64_t first = 9'999; q.state() != Sequencer::State::Failed && first > 1; --first) {
        q.on_packet(packet(first), out);  // one new message, 999 duplicates, ≈ 12 KB copy
        ++sent;
    }
    EXPECT_EQ(q.state(), Sequencer::State::Failed);
    EXPECT_LE(sent, 100U);  // ≈ 1 MiB / 12 KB packets, not thousands
    EXPECT_LT(q.stats().buffered_bytes_peak, 20'000U);  // the payload alone stayed small
    EXPECT_TRUE(out.seqs.empty());
}

TEST(MoldSequencer, ByteBoundAndFarAheadPacketsFailInsteadOfGrowing) {
    Sequencer::Options o;
    o.max_buffered_bytes = 10;
    Sequencer q(o);
    Sink out;
    q.on_packet(build_packet(kS, 2, {msg(2, 6)}), out);  // 6 bytes buffered behind a hole
    EXPECT_EQ(q.state(), Sequencer::State::Gap);
    EXPECT_EQ(q.stats().buffered_bytes_peak, 6U);
    q.on_packet(build_packet(kS, 3, {msg(3, 5)}), out);  // 11 > 10
    EXPECT_EQ(q.state(), Sequencer::State::Failed);

    Sequencer::Options far;
    far.max_ahead = 100;
    Sequencer r(far);
    Sink o2;
    r.on_packet(build_packet(kS, 1, {msg(1)}), o2);
    r.on_packet(build_packet(kS, 102, {msg(9)}), o2);  // 100 ahead of next (2): still allowed
    EXPECT_EQ(r.state(), Sequencer::State::Gap);
    r.on_packet(build_control(kS, 1'000'000'000, kHeartbeat), o2);  // corrupt or huge loss
    EXPECT_EQ(r.state(), Sequencer::State::Failed);
    EXPECT_EQ(o2.seqs, (std::vector<std::uint64_t>{1}));
}

TEST(MoldSequencer, ForeignSessionAndMalformedPacketsAreIgnored) {
    Sequencer q;
    Sink out;
    q.on_packet(build_packet(kS, 1, {msg(1)}), out);
    q.on_packet(build_packet(make_session("OTHER"), 2, {msg(9)}), out);
    auto bad = build_packet(kS, 2, {msg(2)});
    bad.pop_back();
    q.on_packet(bad, out);
    EXPECT_EQ(out.seqs, (std::vector<std::uint64_t>{1}));
    EXPECT_EQ(q.stats().foreign_session_packets, 1U);
    EXPECT_EQ(q.stats().malformed_packets, 1U);
    EXPECT_EQ(q.state(), Sequencer::State::Running);
}

// ---- end to end: ITCH over a lossy, reordering, duplicating channel --------------------------

struct Packetized {
    std::vector<std::vector<std::uint8_t>> packets;  // in send order
    std::vector<std::pair<std::uint64_t, std::size_t>> index;  // first seq, packet number
};

// Packs encoded ITCH messages into MoldUDP64 packets of at most `mtu` bytes.
Packetized packetize(const std::vector<Message>& stream, std::size_t mtu, Rng& rng) {
    Packetized out;
    std::uint64_t seq = 1;
    std::size_t i = 0;
    while (i < stream.size()) {
        const std::size_t want = 1 + rng.below(12);
        std::vector<std::vector<std::uint8_t>> batch;
        std::size_t bytes = kHeaderSize;
        while (i < stream.size() && batch.size() < want) {
            auto e = encode(stream[i]);
            if (!batch.empty() && bytes + 2 + e.size() > mtu) break;
            bytes += 2 + e.size();
            batch.push_back(std::move(e));
            ++i;
        }
        out.index.emplace_back(seq, out.packets.size());
        out.packets.push_back(build_packet(kS, seq, batch));
        seq += batch.size();
    }
    out.packets.push_back(build_control(kS, seq, kEndOfSession));
    return out;
}

std::string direct_hash(const std::vector<Message>& stream, std::size_t prefix) {
    BookBuilder b;
    for (std::size_t i = 0; i < prefix; ++i) (void)b.apply(stream[i]);
    return b.state_hash();
}

TEST(MoldEndToEnd, LossyChannelWithReRequestsRebuildsTheExactBook) {
    SyntheticOptions so;
    so.seed = 8;
    so.messages = 30'000;
    so.symbols = 4;
    const std::vector<Message> stream = generate(so);
    for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
        Rng rng(seed);
        const Packetized pk = packetize(stream, 1400, rng);
        // Channel: 3 % loss, 5 % duplication, reordering within a window of 8 packets.
        std::vector<std::vector<std::uint8_t>> wire;
        for (const auto& p : pk.packets) {
            if (rng.chance(3)) continue;
            wire.push_back(p);
            if (rng.chance(5)) wire.push_back(p);
        }
        for (std::size_t i = 0; i + 1 < wire.size(); ++i) {
            const std::size_t j = i + rng.below(std::min<std::size_t>(8, wire.size() - i));
            std::swap(wire[i], wire[j]);
        }
        Sequencer q;
        BookBuilder book;
        std::uint64_t expect_seq = 1;
        bool ordered = true;
        auto deliver = [&](std::uint64_t s, std::span<const std::uint8_t> m) {
            ordered = ordered && s == expect_seq++;
            const DecodeResult d = decode(m);
            ASSERT_TRUE(std::holds_alternative<Message>(d));
            (void)book.apply(std::get<Message>(d));
        };
        // Re-request: answer missing ranges from the sender's store (the retransmission
        // itself is also lossy, so several rounds may be needed).
        auto rerequest = [&] {
            for (const auto& [first, count] : q.missing()) {
                for (const auto& [pseq, pno] : pk.index) {
                    const std::size_t n = pk.packets[pno][18] * 256U + pk.packets[pno][19];
                    if (pseq < first + count && pseq + n > first && !rng.chance(10)) {
                        q.on_packet(pk.packets[pno], deliver);
                    }
                }
            }
        };
        for (std::size_t i = 0; i < wire.size(); ++i) {
            q.on_packet(wire[i], deliver);
            if (i % 50 == 0) rerequest();
        }
        for (int round = 0; round < 20 && q.state() == Sequencer::State::Gap; ++round) {
            rerequest();
        }
        if (q.state() != Sequencer::State::Ended) {
            q.on_packet(pk.packets.back(), deliver);  // end-of-session itself may have been lost
            rerequest();
        }
        ASSERT_EQ(q.state(), Sequencer::State::Ended) << "seed " << seed;
        EXPECT_TRUE(ordered);
        EXPECT_EQ(q.stats().delivered, stream.size());
        EXPECT_GT(q.stats().duplicates, 0U);
        EXPECT_GT(q.stats().gaps_opened, 0U);
        EXPECT_EQ(book.state_hash(), direct_hash(stream, stream.size())) << "seed " << seed;
    }
}

// Differential property test: the sequencer against a model that only remembers *which*
// sequence numbers have arrived. After every packet the delivered stream, next expected
// sequence, horizon, buffered count, missing() ranges, state and counters must equal what
// that set implies. Traffic: a sender with loss, duplication, delay (reordering), random
// overlapping retransmissions, heartbeats, and re-requests of the reported missing ranges.
std::vector<std::uint8_t> payload_of(std::uint64_t seq) {
    return std::vector<std::uint8_t>(1 + seq % 7, static_cast<std::uint8_t>(seq * 31));
}

std::vector<std::uint8_t> range_packet(std::uint64_t first, std::uint64_t count) {
    std::vector<std::vector<std::uint8_t>> msgs;
    for (std::uint64_t s = first; s < first + count; ++s) msgs.push_back(payload_of(s));
    return build_packet(kS, first, msgs);
}

struct SequencerModel {
    std::set<std::uint64_t> received;
    std::uint64_t next = 1;
    std::uint64_t horizon = 1;
    std::uint64_t duplicates = 0;
    std::uint64_t heartbeats = 0;
    std::uint64_t gaps_opened = 0;
    std::uint64_t gaps_closed = 0;
    bool gap = false;

    void data(std::uint64_t first, std::uint64_t count) {
        for (std::uint64_t s = first; s < first + count; ++s) {
            duplicates += received.insert(s).second ? 0 : 1;
        }
        horizon = std::max(horizon, first + count);
        settle();
    }
    void heartbeat(std::uint64_t next_seq) {
        ++heartbeats;
        horizon = std::max(horizon, next_seq);
        settle();
    }
    void settle() {
        while (received.contains(next)) ++next;
        const bool now_gap = next < horizon;
        gaps_opened += (now_gap && !gap) ? 1 : 0;
        gaps_closed += (!now_gap && gap) ? 1 : 0;
        gap = now_gap;
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> missing() const {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
        for (std::uint64_t s = next; s < horizon;) {
            if (received.contains(s)) {
                ++s;
                continue;
            }
            const std::uint64_t start = s;
            while (s < horizon && !received.contains(s)) ++s;
            out.emplace_back(start, s - start);
        }
        return out;
    }
    std::size_t buffered() const {
        return static_cast<std::size_t>(std::distance(received.upper_bound(next), received.end()));
    }
};

TEST(MoldSequencer, MatchesASetModelUnderRandomTraffic) {
    std::uint64_t gaps = 0;
    std::uint64_t dups = 0;
    std::uint64_t multi_hole = 0;
    std::uint64_t peak_buffered = 0;
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        Rng rng(seed);
        Sequencer q;
        SequencerModel m;
        std::vector<std::uint64_t> seqs;
        bool payload_ok = true;
        auto feed = [&](const std::vector<std::uint8_t>& pkt) {
            q.on_packet(pkt, [&](std::uint64_t s, std::span<const std::uint8_t> msg) {
                seqs.push_back(s);
                const auto want = payload_of(s);
                payload_ok = payload_ok && std::equal(msg.begin(), msg.end(), want.begin(), want.end());
            });
        };
        auto check = [&](int step) {
            ASSERT_EQ(q.state(), m.gap ? Sequencer::State::Gap : Sequencer::State::Running)
                << "seed " << seed << " step " << step;
            ASSERT_EQ(q.next_expected(), m.next) << "seed " << seed << " step " << step;
            ASSERT_EQ(q.horizon(), m.horizon);
            ASSERT_EQ(q.buffered(), m.buffered());
            ASSERT_EQ(q.missing(), m.missing()) << "seed " << seed << " step " << step;
            ASSERT_EQ(seqs.size(), m.next - 1);  // exactly the contiguous prefix, in order
            ASSERT_TRUE(seqs.empty() || seqs.back() == m.next - 1);
            ASSERT_EQ(q.stats().delivered, m.next - 1);
            ASSERT_EQ(q.stats().duplicates, m.duplicates);
            ASSERT_EQ(q.stats().heartbeats, m.heartbeats);
            ASSERT_EQ(q.stats().gaps_opened, m.gaps_opened);
            ASSERT_EQ(q.stats().gaps_closed, m.gaps_closed);
            multi_hole += m.missing().size() >= 2 ? 1 : 0;
            peak_buffered = std::max<std::uint64_t>(peak_buffered, q.buffered());
        };
        std::uint64_t cursor = 1;  // sender's next sequence
        std::vector<std::pair<int, std::pair<std::uint64_t, std::uint64_t>>> delayed;
        for (int step = 0; step < 1500; ++step) {
            std::pair<std::uint64_t, std::uint64_t> send{0, 0};
            const auto r = rng.below(100);
            if (r < 70) {  // the sender's next packet
                send = {cursor, 1 + rng.below(8)};
                cursor += send.second;
            } else if (r < 80 && cursor > 1) {  // a random overlapping retransmission
                const std::uint64_t first = 1 + rng.below(cursor - 1);
                send = {first, std::min<std::uint64_t>(1 + rng.below(12), cursor - first)};
            } else if (r < 88) {  // re-request: the first missing range as the model reports it
                if (const auto miss = m.missing(); !miss.empty()) {
                    send = {miss.front().first, std::min<std::uint64_t>(miss.front().second, 20)};
                }
            } else {
                feed(build_control(kS, cursor, kHeartbeat));
                m.heartbeat(cursor);
                check(step);
                continue;
            }
            if (send.second == 0) continue;
            const auto fate = rng.below(100);
            if (fate < 10) continue;  // lost
            if (fate < 25) {          // delayed by up to 6 steps (reordering)
                delayed.push_back({step + static_cast<int>(1 + rng.below(6)), send});
            } else {
                feed(range_packet(send.first, send.second));
                m.data(send.first, send.second);
                if (fate < 30) {  // duplicated on the wire
                    feed(range_packet(send.first, send.second));
                    m.data(send.first, send.second);
                }
            }
            check(step);
            for (auto it = delayed.begin(); it != delayed.end();) {
                if (it->first <= step) {
                    feed(range_packet(it->second.first, it->second.second));
                    m.data(it->second.first, it->second.second);
                    check(step);
                    it = delayed.erase(it);
                } else {
                    ++it;
                }
            }
        }
        // Fill every remaining hole: everything up to the horizon is delivered exactly once.
        for (const auto& [first, count] : m.missing()) {
            feed(range_packet(first, count));
            m.data(first, count);
        }
        check(-1);
        EXPECT_EQ(q.state(), Sequencer::State::Running);
        EXPECT_EQ(q.next_expected(), m.horizon);
        EXPECT_TRUE(payload_ok) << "seed " << seed;
        for (std::size_t i = 0; i < seqs.size(); ++i) ASSERT_EQ(seqs[i], i + 1);
        gaps += m.gaps_opened;
        dups += m.duplicates;
    }
    // The traffic must actually exercise reordering, duplicates and multi-hole gaps.
    EXPECT_GT(gaps, 300U);
    EXPECT_GT(dups, 1000U);
    EXPECT_GT(multi_hole, 1000U);
    EXPECT_GT(peak_buffered, 20U);
    std::printf("sequencer model: %llu gaps, %llu duplicates, %llu multi-hole states, peak buffered %llu\n",
                static_cast<unsigned long long>(gaps), static_cast<unsigned long long>(dups),
                static_cast<unsigned long long>(multi_hole),
                static_cast<unsigned long long>(peak_buffered));
}

TEST(MoldEndToEnd, WithoutRecoveryTheBookStopsAtTheHoleNeverSkips) {
    SyntheticOptions so;
    so.seed = 9;
    so.messages = 5'000;
    so.symbols = 2;
    const std::vector<Message> stream = generate(so);
    Rng rng(4);
    const Packetized pk = packetize(stream, 1400, rng);
    const std::size_t lost = pk.packets.size() / 2;
    Sequencer q;
    BookBuilder book;
    for (std::size_t i = 0; i < pk.packets.size(); ++i) {
        if (i == lost) continue;
        q.on_packet(pk.packets[i], [&](std::uint64_t, std::span<const std::uint8_t> m) {
            (void)book.apply(std::get<Message>(decode(m)));
        });
    }
    EXPECT_EQ(q.state(), Sequencer::State::Gap);
    const std::uint64_t hole = pk.index[lost].first;
    EXPECT_EQ(q.next_expected(), hole);
    ASSERT_FALSE(q.missing().empty());
    EXPECT_EQ(q.missing().front().first, hole);
    // The book reflects exactly the messages before the hole.
    EXPECT_EQ(book.state_hash(), direct_hash(stream, hole - 1));
}

TEST(MoldSimulation, RecoversTheFullBookOrStopsExactlyAtTheHole) {
    SyntheticOptions so;
    so.seed = 12;
    so.messages = 20'000;
    so.symbols = 3;
    const std::vector<Message> stream = generate(so);
    ChannelOptions o;
    o.seed = 5;
    o.loss_percent = 5;
    const SimulationReport rec = simulate(stream, o);
    EXPECT_GT(rec.packets_lost, 0U);
    EXPECT_GT(rec.gaps_opened, 0U);
    EXPECT_TRUE(rec.in_order);
    EXPECT_EQ(rec.final_state, "ended");
    EXPECT_EQ(rec.delivered, stream.size());
    EXPECT_EQ(rec.book_hash, rec.full_hash);

    o.recovery = false;
    const SimulationReport none = simulate(stream, o);
    EXPECT_NE(none.final_state, "ended");
    EXPECT_LT(none.delivered, stream.size());
    EXPECT_EQ(none.book_hash, none.reference_hash);  // exactly the delivered prefix
    EXPECT_NE(none.book_hash, none.full_hash);
}

}  // namespace
}  // namespace enginex::itch::mold
