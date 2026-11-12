// Fuzz targets for the ITCH path, usable two ways:
//   * libFuzzer (Linux clang): cmake -DENGINEX_FUZZ=libfuzzer ... builds enginex_fuzz_itch with
//     -fsanitize=fuzzer,address,undefined; run ./enginex_fuzz_itch fuzz/corpus
//   * standalone (any compiler): the same entry point driven by fuzz/standalone_driver.cpp,
//     a seeded mutation loop over the corpus (not coverage-guided).
//
// The input's first byte selects the target; the rest is the payload:
//   0: replay() of a length-prefixed file, lenient and strict, then book invariants and
//      determinism (two replays give the same hash)
//   1: MoldUDP64 Sequencer fed packets carved from the input (2-byte lengths), then ITCH
//      decode of every delivered message; checks in-order exactly-once delivery and, after
//      every packet, that missing() is sorted, disjoint, non-adjacent and inside
//      [next_expected, horizon), and that holes + buffered messages fill that range exactly
//   2: parse_request() round trip, and PacketStore: the payload cut into messages, packed,
//      parsed back identically, with find() agreeing with every packet's range
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

#include "enginex/itch/book.hpp"
#include "enginex/itch/moldudp64.hpp"
#include "enginex/itch/replay.hpp"

namespace {

[[noreturn]] void fail(const char* what) {
    std::fprintf(stderr, "fuzz invariant violated: %s\n", what);
    std::abort();
}

void fuzz_replay(std::span<const std::uint8_t> data) {
    using namespace enginex::itch;
    BookBuilder a;
    const ReplayResult r = replay(data, a);
    if (const auto bad = a.check_invariants())
        fail(bad->c_str());
    if (r.decoded + r.decode_error_count() != r.frames)
        fail("frame accounting");
    if (r.bytes_processed > data.size())
        fail("bytes_processed beyond input");
    BookBuilder b;
    ReplayOptions strict;
    strict.strict = true;
    const ReplayResult s = replay(data, b, strict);
    if (s.frames > r.frames)
        fail("strict processed more frames than lenient");
    if (const auto bad = b.check_invariants())
        fail(bad->c_str());
    BookBuilder c;
    (void)replay(data, c);
    if (a.state_hash() != c.state_hash())
        fail("replay not deterministic");
    // The bounded level vector (D32) at its smallest limit: constant spill and refill must not
    // change any book.
    BookBuilder d(0, 2);
    (void)replay(data, d);
    if (const auto bad = d.check_invariants())
        fail(bad->c_str());
    if (a.state_hash() != d.state_hash())
        fail("level-vector limit changed the books");
}

void fuzz_mold(std::span<const std::uint8_t> data) {
    using namespace enginex::itch;
    mold::Sequencer::Options o;
    o.max_buffered_messages = 256;
    o.max_buffered_bytes = 4096;  // small: exercises the packet-copy accounting and Failed
    o.max_ahead = 1000;
    mold::Sequencer q(o);
    std::uint64_t expect = 1;
    BookBuilder book;
    auto deliver = [&](std::uint64_t seq, std::span<const std::uint8_t> m) {
        if (seq != expect++)
            fail("out-of-order or repeated delivery");
        const DecodeResult d = decode(m);
        if (const auto* msg = std::get_if<Message>(&d))
            (void)book.apply(*msg);
    };
    std::size_t pos = 0;
    while (data.size() - pos >= 2) {
        const std::size_t len = (static_cast<std::size_t>(data[pos]) << 8U) | data[pos + 1];
        pos += 2;
        const std::size_t n = std::min(len, data.size() - pos);
        q.on_packet(data.subspan(pos, n), deliver);
        pos += n;
        if (q.state() == mold::Sequencer::State::Failed)
            continue;
        std::uint64_t cursor = q.next_expected();
        std::uint64_t holes = 0;
        for (const auto& [first, count] : q.missing()) {
            if (count == 0 || first < cursor || (first == cursor && cursor != q.next_expected()))
                fail("missing() ranges not sorted, disjoint and non-adjacent");
            if (first + count > q.horizon())
                fail("missing() beyond the horizon");
            holes += count;
            cursor = first + count;
        }
        if (holes + q.buffered() != q.horizon() - q.next_expected())
            fail("holes + buffered != horizon - next_expected");
    }
    if (q.stats().delivered != expect - 1)
        fail("delivered count");
    if (const auto bad = book.check_invariants())
        fail(bad->c_str());
}

void fuzz_store(std::span<const std::uint8_t> data) {
    using namespace enginex::itch;
    if (const auto req = mold::parse_request(data)) {
        if (mold::build_request(req->session, req->sequence, req->count) !=
            std::vector<std::uint8_t>(data.begin(), data.end()))
            fail("parse_request round trip");
    } else if (data.size() == mold::kHeaderSize) {
        // Rejected 20-byte requests must have count 0 / 0xFFFF, sequence 0 or a wrap.
        const std::uint16_t count = static_cast<std::uint16_t>((data[18] << 8U) | data[19]);
        std::uint64_t seq = 0;
        for (int i = 10; i < 18; ++i)
            seq = (seq << 8U) | data[static_cast<std::size_t>(i)];
        if (count != 0 && count != mold::kEndOfSession && seq != 0 && seq <= UINT64_MAX - count)
            fail("valid request rejected");
    }
    if (data.empty())
        return;
    // Cut the payload into messages: each byte b starts a message of (b % 23) bytes.
    const std::size_t mtu = 64 + data[0] * 4U;
    mold::PacketStore store(mold::make_session("FUZZ"), mtu);
    std::vector<std::vector<std::uint8_t>> msgs;
    for (std::size_t pos = 1; pos < data.size();) {
        const std::size_t n = std::min<std::size_t>(data[pos] % 23U, data.size() - pos);
        msgs.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(pos),
                          data.begin() + static_cast<std::ptrdiff_t>(pos + n));
        store.append(msgs.back());
        pos += std::max<std::size_t>(n, 1);
    }
    store.finish();
    if (store.end_sequence() != msgs.size() + 1)
        fail("store end sequence");
    std::uint64_t next = 1;
    for (std::size_t i = 0; i < store.size(); ++i) {
        mold::ParsedPacket p;
        if (mold::parse(store.packet(i), p) || p.header.sequence != next ||
            p.messages.size() != store.count(i) || store.first(i) != next)
            fail("store packet does not parse back");
        if (store.count(i) > 1 && store.packet(i).size() > mtu)
            fail("packet over the MTU");
        for (std::size_t k = 0; k < p.messages.size(); ++k) {
            const auto& want = msgs[static_cast<std::size_t>(next - 1 + k)];
            if (!std::equal(p.messages[k].begin(), p.messages[k].end(), want.begin(), want.end()))
                fail("store changed a message");
            if (store.find(next + k) != i)
                fail("find() disagrees with the packet range");
        }
        next += store.count(i);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0)
        return 0;
    const std::span<const std::uint8_t> payload(data + 1, size - 1);
    switch (data[0] % 3) {
        case 0:
            fuzz_replay(payload);
            break;
        case 1:
            fuzz_mold(payload);
            break;
        default:
            fuzz_store(payload);
            break;
    }
    return 0;
}
