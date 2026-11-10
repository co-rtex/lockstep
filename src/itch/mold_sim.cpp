#include "enginex/itch/mold_sim.hpp"

#include <algorithm>
#include <utility>

#include "enginex/itch/book.hpp"
#include "enginex/itch/decoder.hpp"
#include "enginex/itch/moldudp64.hpp"
#include "enginex/rng.hpp"

namespace enginex::itch::mold {

namespace {

PacketStore packetize(const std::vector<Message>& stream, std::size_t mtu, const Session& session) {
    PacketStore s(session, mtu);
    for (const Message& m : stream) {
        s.append(encode(m));
    }
    s.finish();
    return s;
}

std::string hash_of_prefix(const std::vector<Message>& stream, std::size_t n) {
    BookBuilder b;
    for (std::size_t i = 0; i < n && i < stream.size(); ++i) {
        (void)b.apply(stream[i]);
    }
    return b.state_hash();
}

std::string state_name(Sequencer::State s) {
    switch (s) {
        case Sequencer::State::Running: return "running";
        case Sequencer::State::Gap: return "gap";
        case Sequencer::State::Ended: return "ended";
        case Sequencer::State::Failed: return "failed";
    }
    return "unknown";
}

}  // namespace

SimulationReport simulate(const std::vector<Message>& stream, const ChannelOptions& o) {
    const Session session = make_session("SIMULATED");
    const PacketStore store = packetize(stream, o.mtu, session);
    const auto end_packet = build_control(session, store.end_sequence(), kEndOfSession);
    Rng rng(o.seed);
    SimulationReport r;
    r.messages = stream.size();
    r.packets = store.size();

    // Live feed: loss, duplication, bounded reordering.
    std::vector<const std::vector<std::uint8_t>*> wire;
    for (std::size_t i = 0; i < store.size(); ++i) {
        const auto& p = store.packet(i);
        if (rng.below(100) < o.loss_percent) {
            ++r.packets_lost;
            continue;
        }
        wire.push_back(&p);
        if (rng.below(100) < o.duplicate_percent) {
            ++r.packets_duplicated;
            wire.push_back(&p);
        }
    }
    if (rng.below(100) >= o.loss_percent) {
        wire.push_back(&end_packet);
    }
    if (o.reorder_window > 1) {
        for (std::size_t i = 0; i + 1 < wire.size(); ++i) {
            const std::size_t j = i + rng.below(std::min(o.reorder_window, wire.size() - i));
            std::swap(wire[i], wire[j]);
        }
    }

    Sequencer q;
    BookBuilder book;
    std::uint64_t expect = 1;
    auto deliver = [&](std::uint64_t seq, std::span<const std::uint8_t> m) {
        r.in_order = r.in_order && seq == expect;
        ++expect;
        const DecodeResult d = decode(m);
        if (const auto* msg = std::get_if<Message>(&d)) {
            (void)book.apply(*msg);
        }
    };
    auto recover = [&] {
        ++r.rerequest_rounds;
        for (const auto& [first, count] : q.missing()) {
            for (std::size_t i = 0; i < store.size(); ++i) {
                const std::uint64_t pseq = store.first(i);
                const std::uint64_t n = store.count(i);
                if (pseq < first + count && pseq + n > first && rng.below(100) >= o.loss_percent) {
                    ++r.retransmitted_packets;
                    q.on_packet(store.packet(i), deliver);
                }
            }
        }
    };
    for (std::size_t i = 0; i < wire.size(); ++i) {
        q.on_packet(*wire[i], deliver);
        if (o.recovery && i % 64 == 63 && q.state() == Sequencer::State::Gap) {
            recover();
        }
    }
    if (o.recovery) {
        // The end-of-session marker itself may have been lost: a real consumer would learn the
        // end from the next heartbeat; the simulation re-sends it once per round.
        for (std::size_t round = 0; round < o.max_recovery_rounds &&
                                    q.state() != Sequencer::State::Ended &&
                                    q.state() != Sequencer::State::Failed;
             ++round) {
            q.on_packet(end_packet, deliver);
            recover();
        }
    }
    const Sequencer::Stats& s = q.stats();
    r.delivered = s.delivered;
    r.duplicates_dropped = s.duplicates;
    r.gaps_opened = s.gaps_opened;
    r.gaps_closed = s.gaps_closed;
    r.buffered_peak = s.buffered_peak;
    r.final_state = state_name(q.state());
    r.next_expected = q.next_expected();
    r.book_hash = book.state_hash();
    r.reference_hash = hash_of_prefix(stream, static_cast<std::size_t>(q.next_expected() - 1));
    r.full_hash = hash_of_prefix(stream, stream.size());
    return r;
}

}  // namespace enginex::itch::mold
