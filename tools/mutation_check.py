#!/usr/bin/env python3
"""Mutation check: apply plausible bugs one at a time, rebuild, run the relevant tests.

A mutant is KILLED if the tests fail, SURVIVED if they pass (a test gap), INVALID if it does
not compile. Every source file is restored after each mutant (try/finally), and the script
refuses to start with uncommitted changes in the files it mutates.

WARNING: mutants are written into the source tree. Do not build any other build directory
while this runs: it can compile a mutant (this happened once: a concurrent UBSan build
picked up a mutated book.cpp and 18 tests failed until it was rebuilt from clean sources).

  python3 tools/mutation_check.py [BUILD_DIR] [--only SUBSTRING]
      BUILD_DIR defaults to build/release (configure and build it first); --only runs the
      mutants whose name contains SUBSTRING.
"""
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
ARGS = sys.argv[1:]
ONLY = ""
if "--only" in ARGS:
    i = ARGS.index("--only")
    if i + 1 >= len(ARGS):
        sys.exit(__doc__)
    ONLY = ARGS[i + 1]
    del ARGS[i:i + 2]
# Validate everything before any mutant touches the source tree: an unknown option such as
# --help used to be taken as BUILD_DIR, and the run started mutating files.
if len(ARGS) > 1 or any(a.startswith("-") for a in ARGS):
    sys.exit(__doc__)
BUILD = pathlib.Path(ARGS[0] if ARGS else ROOT / "build" / "release")
TESTS = BUILD / "tests" / "enginex_tests"
if not (BUILD / "CMakeCache.txt").is_file() or not TESTS.is_file():
    sys.exit(f"{BUILD} is not a configured and built build directory "
             f"(need CMakeCache.txt and tests/enginex_tests)")

ITCH = "*Itch*:ByteReader*:StreamFramer*:OrderIndex*:PagedIndex*:Mold*"
CORE = "-Server.*:FeedMirrorSession.*:Points/*:*Itch*:Mold*"

# (name, file, original, mutated, occurrence (0-based), gtest filter)
MUTANTS = [
    ("itch: order at zero shares not removed", "src/itch/book.cpp",
     "if (n.shares == 0) {", "if (false) {", 0, ITCH),
    ("itch: execution of all remaining shares rejected", "src/itch/book.cpp",
     "if (shares > im.pool[*slot].shares) {", "if (shares >= im.pool[*slot].shares) {", 0, ITCH),
    ("itch: modify with wrong locate accepted", "src/itch/book.cpp",
     "if (im.pool[*slot].locate != locate) {",
     "if (im.pool[*slot].locate != locate && false) {", 0, ITCH),
    ("itch: non-printable C counted as printable", "src/itch/book.cpp",
     "if (msg.printable) {", "if (true) {", 0, ITCH),
    ("itch: add with zero shares accepted", "src/itch/book.cpp",
     "if (msg.shares == 0) {", "if (false) {", 0, ITCH),
    ("itch: replace with zero shares accepted", "src/itch/book.cpp",
     "if (msg.shares == 0) {", "if (false) {", 1, ITCH),
    ("itch: near-top scan off by one", "src/itch/book.cpp",
     "if ((it - 1)->rank < r) {", "if ((it - 1)->rank <= r) {", 0, ITCH),
    ("itch: level at the deep tier's best price routed to the vector", "src/itch/book.cpp",
     "        if (!deep_.empty() && r <= deep_.rbegin()->first) {",
     "        if (!deep_.empty() && r < deep_.rbegin()->first) {", 0, ITCH),
    ("itch: empty level kept after its last order leaves", "src/itch/book.cpp",
     "if (level.count == 0) {", "if (false) {", 0, ITCH),
    ("itch: removal leaves level shares stale", "src/itch/book.cpp",
     "level.shares -= n.shares;", "(void)0;", 0, ITCH),
    ("index: backward-shift deletion moves the wrong entries", "include/enginex/itch/order_index.hpp",
     "if (((j - home) & mask_) >= ((j - hole) & mask_)) {",
     "if (((j - home) & mask_) > ((j - hole) & mask_)) {", 0, ITCH),
    ("paged index: lookup ignores the hash fallback", "include/enginex/itch/paged_index.hpp",
     "if (v != kEmpty || far_.size() == 0) {", "if (true) {", 0, ITCH),
    ("paged index: page freed while an order is still live", "include/enginex/itch/paged_index.hpp",
     "if (--dir_[p]->live == 0) {", "if (--dir_[p]->live <= 1) {", 0, ITCH),
    ("codec: timestamp read one byte early", "src/itch/codec.cpp",
     "h.timestamp_ns = r.u48(5);", "h.timestamp_ns = r.u48(4);", 0, ITCH),
    ("codec: stock directory IPO flag read one byte early", "src/itch/codec.cpp",
     "m.ipo_flag = r.alpha(31);", "m.ipo_flag = r.alpha(30);", 0, ITCH + "|ctest:itch_pydecode"),
    ("codec: add-order side not validated", "src/itch/codec.cpp",
     "if (side != 'B' && side != 'S') {", "if (false) {", 0, ITCH),
    ("codec: add-order price read one byte early", "src/itch/codec.cpp",
     "m.price = r.u32(32);", "m.price = r.u32(31);", 0, ITCH),
    ("codec: printable flag not validated", "src/itch/codec.cpp",
     "if (printable != 'Y' && printable != 'N') {", "if (false) {", 0, ITCH),
    ("framer: zero-length prefix accepted", "include/enginex/itch/decoder.hpp",
     "if (len == 0) {", "if (false) {", 1, ITCH),
    ("mold: buffered duplicate not detected", "include/enginex/itch/moldudp64.hpp",
     "if (!fill_hole(seq)) {", "if (!fill_hole(seq) && false) {", 0, ITCH),
    ("mold: adjacent holes not merged", "src/itch/moldudp64.cpp",
     "if (!holes_.empty() && std::prev(holes_.end())->second == horizon_) {",
     "if (false) {", 0, ITCH),
    ("mold: filling a hole loses its left part", "src/itch/moldudp64.cpp",
     "if (start < seq) {", "if (start + 1 < seq) {", 0, ITCH),
    ("mold: missing() skips a one-message hole", "src/itch/moldudp64.cpp",
     "out.emplace_back(start, end - start);",
     "if (end - start > 1) out.emplace_back(start, end - start);", 0, ITCH),
    ("mold: closed gap not counted", "include/enginex/itch/moldudp64.hpp",
     "++stats_.gaps_closed;", "(void)0;", 0, ITCH),
    ("mold: buffered chunk recycled while still referenced", "include/enginex/itch/moldudp64.hpp",
     "if (--c.live == 0) {", "if (--c.live <= 1) {", 0, ITCH),
    ("mold: hole for a packet ahead not opened", "include/enginex/itch/moldudp64.hpp",
     "if (first > horizon_) {", "if (first > horizon_ + 1) {", 0, ITCH),
    ("mold: packet copies not counted against the byte bound", "src/itch/moldudp64.cpp",
     "if (held > room) {", "if ((void)held, (void)room, false) {", 0, ITCH),
    ("mold: stale end of session accepted", "include/enginex/itch/moldudp64.hpp",
     "} else if (h.sequence >= horizon_) {", "} else if (true) {", 0, ITCH),
    ("udp: partly answered request asked again before its retry timer", "src/itch/mold_udp.cpp",
     "first = it->second.end;  // covered up to there", "(void)0;  // covered up to there", 0, ITCH),
    ("udp: request runs over a later active request", "src/itch/mold_udp.cpp",
     "                end = it->first;\n", "                (void)it;\n", 0, ITCH),
    ("mold: trailing bytes accepted", "src/itch/moldudp64.cpp",
     "if (pos != packet.size()) {", "if (false) {", 0, ITCH),
    ("mold: buffered-bytes failure reported as the message-count bound",
     "include/enginex/itch/moldudp64.hpp",
     "? FailReason::BufferedMessages\n                                   : FailReason::BufferedBytes;",
     "? FailReason::BufferedMessages\n                                   : FailReason::BufferedMessages;",
     0, ITCH),
    ("mold store: find() returns the packet after the one holding the sequence",
     "src/itch/moldudp64.cpp",
     "std::distance(first_.begin(), it) - 1);", "std::distance(first_.begin(), it));", 0, ITCH),
    ("mold-sim: recovery skips the packet holding the first missing message",
     "src/itch/mold_sim.cpp",
     "for (std::size_t i = *start; i < store.size()",
     "for (std::size_t i = *start + 1; i < store.size()", 0, ITCH),
    ("index: hash ignores the secret key", "include/enginex/itch/order_index.hpp",
     "siphash24(key_, std::string_view(bytes, 8))", "siphash24(SipKey{}, std::string_view(bytes, 8))",
     0, ITCH),
    ("index: random key is a constant", "include/enginex/itch/order_index.hpp",
     "return SipKey{word(), word()};", "return (void)word, SipKey{1, 2};", 0, ITCH),
    ("json: nested objects share the parent's key set", "src/json_codec.cpp",
     "                open.emplace_back();\n",
     "                if (open.empty()) open.emplace_back();\n", 0, "JsonCodec.*"),
    ("json: repeated key not reported", "src/json_codec.cpp",
     "issue = KeyIssue::RepeatedKey;", "(void)0;", 0, "JsonCodec.*:Server.*"),
    ("json: one key above the per-object limit accepted", "src/json_codec.cpp",
     "if (open.back().size() >= kMaxKeysPerObject) {",
     "if (open.back().size() > kMaxKeysPerObject) {", 0, "JsonCodec.*"),
    ("mirror: update applied when not live", "src/mirror.cpp",
     "if (state_ != State::Live) {", "if (false) {", 0, "FeedMirror.*"),
    ("mirror: forward gap accepted", "src/mirror.cpp",
     "if (seq != last_event_ + 1) {", "if (seq < last_event_ + 1) {", 0, "FeedMirror.*"),
    ("mirror: verification ignores watermark", "src/mirror.cpp",
     "if (watermark != last_event_) {", "if (false) {", 0, "FeedMirror.*"),
    ("engine: cancel leaves level aggregate stale", "src/engine.cpp",
     "    level.aggregate -= loc.node->remaining;", "    (void)0;", 0, CORE),
    ("engine: an order at exactly the resting price does not trade", "src/engine.cpp",
     "level_price <= limit : level_price >= limit;", "level_price < limit : level_price >= limit;",
     0, CORE),
    ("engine: incoming sell hits the lowest bid", "src/engine.cpp",
     "auto level_it = kRestingSide == Side::Sell ? resting.begin() : std::prev(resting.end());",
     "auto level_it = resting.begin();", 0, CORE),
    ("journal: corrupted length near EOF passed off as a torn write", "src/journal.cpp",
     "!why.empty()) {", "false) {", 0, CORE),
    ("journal: zero-filled tail treated as corruption", "src/journal.cpp",
     "if (all_zero(rest)) {", "if (false) {", 0, CORE),
    ("journal: torn tail not truncated", "src/journal.cpp",
     "if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {",
     "if ((void)size, false) {", 0, CORE),
    ("core index: fingerprint match taken without comparing the request_id", "src/state.cpp",
     "            if (request_id_of(r.command) == request_id) {",
     "            if ((void)request_id, true) {", 0, CORE),
    ("core index: growth drops existing entries", "src/state.cpp",
     "        if (s.index_plus_one != 0) {", "        if (false) {", 0, CORE),
    ("state: conflicting body treated as duplicate", "src/state.cpp",
     "if (original->command == cmd) {", "if (true) {", 0, CORE),
    ("state: price one tick above the maximum accepted", "src/state.cpp",
     "order->price < kMinPrice || order->price > kMaxPrice",
     "order->price < kMinPrice || order->price > kMaxPrice + 1", 0, CORE),
    ("state: zero quantity accepted", "src/state.cpp",
     "order->quantity < kMinQuantity || order->quantity > kMaxQuantity",
     "order->quantity < kMinQuantity - 1 || order->quantity > kMaxQuantity", 0, CORE),
    ("state: open-order limit off by one", "src/state.cpp",
     "if (engine_.open_order_count() >= config_.max_open_orders) {",
     "if (engine_.open_order_count() > config_.max_open_orders) {", 0, CORE),
    ("state: request_id at the length limit rejected", "src/state.cpp",
     "if (id.size() > kMaxRequestIdBytes) {", "if (id.size() >= kMaxRequestIdBytes) {", 0, CORE),
    ("state: cancel may target a cancel command", "src/state.cpp",
     "if (!std::holds_alternative<NewLimitOrder>(record.command)) {", "if (false) {", 0, CORE),
    ("journal: record CRC not checked", "src/journal.cpp",
     "if (crc32c(payload) != crc) {",
     "if (crc32c(payload) != crc && false) {", 0, CORE),
    ("server: snapshot served after journal failure", "src/server.cpp",
     "if (backend_.failed()) {", "if (false) {", 0, "Server.*"),
    ("server: snapshot served after internal fail-stop", "src/server.cpp",
     "} else if (fatal_) {\n            // The in-memory state may hold",
     "} else if (false) {\n            // The in-memory state may hold", 0, "Server.*"),
    ("server: event published although the flush failed", "src/server.cpp",
     "if (err == ErrorCode::None && p.has_event) {", "if (p.has_event) {", 0, "Server.*"),
    ("server: reply after a failed flush claims success", "src/server.cpp",
     "if (err != ErrorCode::None && p.state_dependent) {", "if (false) {", 0, "Server.*"),
    ("server: batch flushed after an internal fail-stop", "src/server.cpp",
     "fatal_ ? ErrorCode::InternalError : backend_.flush();", "backend_.flush();", 0, "Server.*"),
    # Not listed: removing `staged_event_ = published_event_;` (watermark rollback after a
    # failed commit) survives because it is equivalent: both failures are permanent
    # fail-stops (DurableExchange::failed_ never clears; fatal_ short-circuits handle()), so
    # no event is ever staged or published again. The line is kept as defence in depth.
    ("protocol: book_update order_count off by one", "src/protocol.cpp",
     "it == map.end() ? 0U : static_cast<std::uint32_t>(it->second.orders.size());",
     "it == map.end() ? 0U : static_cast<std::uint32_t>(it->second.orders.size() + 1);", 0,
     "Server.*:FeedMirror*|ctest:exchange_pymodel_network"),
    ("server: slow-consumer notice reports stale watermark", "src/server.cpp",
     "s->last_event_queued = seq;", "(void)seq;", 0, "Server.*:FeedMirror*"),
]


def replace_nth(text, old, new, n):
    start = -1
    for _ in range(n + 1):
        start = text.find(old, start + 1)
        if start < 0:
            return None
    return text[:start] + new + text[start + len(old):]


def run(cmd, timeout):
    try:
        p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        return "timeout", ""


def main():
    files = sorted({m[1] for m in MUTANTS})
    dirty = subprocess.run(["git", "status", "--porcelain", "--", *files], cwd=ROOT,
                           capture_output=True, text=True).stdout.strip()
    if dirty:
        sys.exit(f"refusing to run with uncommitted changes:\n{dirty}")
    results = []
    for name, rel, old, new, nth, filt in MUTANTS:
        if ONLY not in name:
            continue
        path = ROOT / rel
        original = path.read_text()
        mutated = replace_nth(original, old, new, nth)
        if mutated is None:
            sys.exit(f"snippet not found for mutant {name!r}")
        t0 = time.time()
        try:
            path.write_text(mutated)
            # A filter may end in "|ctest:REGEX": ctest tests (e.g. the Python differential
            # decoder) that run when the gtest filter alone does not kill the mutant.
            gfilt, _, ctest_re = filt.partition("|ctest:")
            targets = ["enginex_tests"] + (["enginex"] if ctest_re else [])
            rc, out = run(["cmake", "--build", str(BUILD), "--target", *targets], 900)
            if rc != 0:
                verdict = "INVALID"
            else:
                rc, out = run([str(TESTS), f"--gtest_filter={gfilt}", "--gtest_brief=1"], 900)
                verdict = "SURVIVED" if rc == 0 else "KILLED"
                if verdict == "SURVIVED" and ctest_re:
                    rc, cout = run(["ctest", "--test-dir", str(BUILD), "-R", ctest_re], 900)
                    if rc != 0:
                        verdict = "KILLED"
                        out = f"[  FAILED  ] ctest -R {ctest_re}"
        finally:
            path.write_text(original)
        failed = sorted({l.split()[3] for l in out.splitlines()
                         if l.startswith("[  FAILED  ]") and len(l.split()) > 3
                         and "," not in l.split()[3]})[:4] if verdict == "KILLED" else []
        results.append({"mutant": name, "file": rel, "verdict": verdict,
                        "seconds": round(time.time() - t0, 1), "failing_tests": failed})
        print(f"{verdict:8s} {name}  {', '.join(failed)}", flush=True)
    run(["cmake", "--build", str(BUILD)], 1800)  # rebuild the unmutated tree
    dirty = subprocess.run(["git", "status", "--porcelain", "--", *files], cwd=ROOT,
                           capture_output=True, text=True).stdout.strip()
    if dirty:
        sys.exit(f"ERROR: files not restored:\n{dirty}")
    killed = sum(r["verdict"] == "KILLED" for r in results)
    valid = sum(r["verdict"] != "INVALID" for r in results)
    print(f"\nmutation score: {killed}/{valid} killed")
    (BUILD / "mutation_results.json").write_text(json.dumps(results, indent=2) + "\n")
    return 0 if killed == valid else 1


if __name__ == "__main__":
    sys.exit(main())
