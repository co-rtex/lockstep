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
//      decode of every delivered message; checks in-order exactly-once delivery
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <span>

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
    if (const auto bad = a.check_invariants()) fail(bad->c_str());
    if (r.decoded + r.decode_error_count() != r.frames) fail("frame accounting");
    if (r.bytes_processed > data.size()) fail("bytes_processed beyond input");
    BookBuilder b;
    ReplayOptions strict;
    strict.strict = true;
    const ReplayResult s = replay(data, b, strict);
    if (s.frames > r.frames) fail("strict processed more frames than lenient");
    if (const auto bad = b.check_invariants()) fail(bad->c_str());
    BookBuilder c;
    (void)replay(data, c);
    if (a.state_hash() != c.state_hash()) fail("replay not deterministic");
}

void fuzz_mold(std::span<const std::uint8_t> data) {
    using namespace enginex::itch;
    mold::Sequencer::Options o;
    o.max_buffered_messages = 256;
    o.max_ahead = 1000;
    mold::Sequencer q(o);
    std::uint64_t expect = 1;
    BookBuilder book;
    auto deliver = [&](std::uint64_t seq, std::span<const std::uint8_t> m) {
        if (seq != expect++) fail("out-of-order or repeated delivery");
        const DecodeResult d = decode(m);
        if (const auto* msg = std::get_if<Message>(&d)) (void)book.apply(*msg);
    };
    std::size_t pos = 0;
    while (data.size() - pos >= 2) {
        const std::size_t len = (static_cast<std::size_t>(data[pos]) << 8U) | data[pos + 1];
        pos += 2;
        const std::size_t n = std::min(len, data.size() - pos);
        q.on_packet(data.subspan(pos, n), deliver);
        pos += n;
    }
    if (q.stats().delivered != expect - 1) fail("delivered count");
    if (const auto bad = book.check_invariants()) fail(bad->c_str());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    const std::span<const std::uint8_t> payload(data + 1, size - 1);
    if (data[0] % 2 == 0) {
        fuzz_replay(payload);
    } else {
        fuzz_mold(payload);
    }
    return 0;
}
