#pragma once

// Simulated MoldUDP64 delivery of an ITCH message stream, for demos and tests. This is a
// SIMULATION: no sockets, no multicast, no real re-request server. It packs messages into
// MTU-bounded MoldUDP64 packets, sends them over a seeded channel that drops, duplicates and
// reorders packets, and, unless recovery is disabled, answers the sequencer's missing() ranges
// from the sender's packet store (itself lossy). The consumer is the real Sequencer and the
// real BookBuilder, so the result shows whether the transport layer reproduced the exact book.

#include <cstdint>
#include <string>
#include <vector>

#include "enginex/itch/messages.hpp"

namespace enginex::itch::mold {

struct ChannelOptions {
    std::uint64_t seed = 1;
    std::uint32_t loss_percent = 3;       // per packet, on the live feed and on retransmissions
    std::uint32_t duplicate_percent = 5;  // per delivered packet
    std::size_t reorder_window = 8;       // packets may swap within this distance
    std::size_t mtu = 1400;               // max packet bytes
    bool recovery = true;                 // answer missing() ranges from the sender's store
    std::size_t max_recovery_rounds = 50;
};

struct SimulationReport {
    std::size_t messages = 0;
    std::size_t packets = 0;
    std::size_t packets_lost = 0;
    std::size_t packets_duplicated = 0;
    std::size_t rerequest_rounds = 0;
    std::size_t retransmitted_packets = 0;
    std::uint64_t delivered = 0;
    std::uint64_t duplicates_dropped = 0;
    std::uint64_t gaps_opened = 0;
    std::uint64_t gaps_closed = 0;
    std::uint64_t buffered_peak = 0;
    bool in_order = true;          // every delivery was next_expected, exactly once
    std::string final_state;       // running | gap | ended | failed
    std::uint64_t next_expected = 0;
    std::string book_hash;         // book built from delivered messages
    std::string reference_hash;    // book built from the first next_expected-1 messages directly
    std::string full_hash;         // book built from every message directly
};

[[nodiscard]] SimulationReport simulate(const std::vector<Message>& stream,
                                        const ChannelOptions& options);

}  // namespace enginex::itch::mold
