# EngineX: a quant/infrastructure SWE case study

A one-page orientation for interviewers, and talking points for the author. Everything here
is implemented and was measured or tested in this repository. The detailed engineering story
is in [case-study.md](case-study.md). Specifications are in [itch.md](itch.md),
[protocol.md](protocol.md) and [journaling.md](journaling.md). Raw numbers are in
[benchmarks.md](benchmarks.md) and `benchmarks/results/`.

## What it is

Two deliberately separate C++20 systems in one repository:

| | A. Synthetic exchange | B. Market-data research path |
|---|---|---|
| Direction | order **entry** → matching → acks and a feed | Nasdaq **TotalView-ITCH 5.0** outbound market data → book reconstruction |
| Core | deterministic price-time matching, single sequencer | decoder for all 23 ITCH 5.0 message types; order-level books |
| Durability / transport | EXJ1 write-ahead journal, ack after `F_FULLFSYNC`/`fdatasync`, crash recovery | 2-byte length-prefixed files; MoldUDP64 packet sequencing with gap detection |
| Network | TCP order entry + sequenced `book_update` feed, snapshot handoff, read-only mirror | none (MoldUDP64 is exercised on a *simulated* channel) |
| Data | synthetic commands | committed tests use synthetic fixtures only; validated locally on three real public Nasdaq sample days (not redistributed) |

ITCH data never become orders in the exchange. They have different types (`itch::Side` vs
`Side`), different code, and a different purpose (D20).

## Architecture in one breath

* **Exchange:** network threads parse and enqueue; one sequencer validates, sequences,
  matches, journals a batch with a single write+sync (group commit) and only then releases
  acks and events. Books use `std::pmr` containers on a per-engine pool.
* **ITCH:** bytes → `StreamFramer` (file) or MoldUDP64 `Sequencer` (packets) → `decode()`
  (length-checked, bounds-checked, big-endian, no casts) → `BookBuilder`. The builder holds
  a paged direct order index (pages of order references), a chunked node pool, and per-symbol
  books whose price levels are sorted vectors with intrusive FIFO queues. Output is
  depth/top plus a SHA-256 over the canonical book.

## Results that are measured, not asserted

Machine: Apple M3 MacBook Air (fanless), macOS 26.6, Apple clang 17, `-O3`. All numbers
are in-process and single-threaded; none is a wire latency.

| What | Result | Evidence |
|---|---|---|
| ITCH decode (real data) | ≈ 13 ns/msg, ≈ 2.3 GiB/s, 0 allocations | 3 sample days |
| ITCH full replay (real data, 8.7 k symbols, 1.5–1.6 M live orders) | 99 → 55–58 ns/msg after data-driven layout changes (3 days) | `itch-real-sample-macos-m3.csv` |
| Book memory (real data) | ≈ 188–211 bytes per live order, whole process, at 1.5–1.65 M live orders | `itch-real-memory-macos-m3.csv` |
| Real-data correctness | ≈ 64 M messages over 3 days: 0 decode errors, 0 anomalies under `--strict`, every live order equal to an independent reference model | `scripts/real_data_check.sh` |
| Worst single message on real data | ≈ 10 ms → ≈ 0.1–0.17 ms (vector growth removed) | benchmarks.md |
| Exchange matching core | +14–28 % throughput where nodes recycle (pmr pool), identical state hashes | `r4-core-pool-macos-m3.csv` |
| Durable ack | ≈ 3 ms p50 with `F_FULLFSYNC`; group commit 3.7× throughput with 8 clients | R2/R3 tables |
| Loopback TCP round trip | ≈ 33 µs p50 (1 client, in-memory) | R3 table |

## Correctness work that found real bugs

* **Two server bugs, reproduced before fixing.** A subscriber whose snapshot was too large
  was registered with no baseline. A snapshot after an internal fail-stop could expose
  never-durable state.
* **Four independent adversarial reviews**, each followed by fixes with regression tests.
  Findings included strict-replay counters drifting after a stop, mirror exceptions escaping
  a session, partial application of a malformed update, unclamped directory growth in the
  paged index, and a JSON-output crash on non-ASCII bytes in ITCH alpha fields.
* **Mutation checks prove the tests can fail.** A scripted harness (`tools/mutation_check.py`)
  kills 41/41 hand-chosen bugs across decoder, book, indexes, sequencer, mirror, matching,
  validation, journal and server. It found two real test gaps, both now covered. One was a
  duplicate of an already-buffered MoldUDP64 message being counted against the buffer bound.
  The other was a `book_update` published although its journal flush had failed.
* **Fuzzing:** a libFuzzer-compatible harness, more than 8 M inputs without a violation (UBSan
  on part of them). **Coverage:** 94 % of lines. **Leaks:** 0. **Fault injection:** a lossy
  TCP relay, a stalled consumer, lossy MoldUDP64 channels with re-request. **Property tests**
  over 100 k hostile exchange requests, 200 k random feed messages, and 400 randomly damaged
  journals (recovery yields an exact prefix or refuses, never a divergent state). Each test asserts
  that its generator actually reaches the interesting paths; that assertion caught three
  generators that were too weak to mean anything.

## Talking points (questions this project answers)

1. *"How do you parse a binary feed safely?"* Validate the length before reading any field,
   assemble big-endian integers byte by byte, and bounds-check every read. Keep framing errors
   (fatal, because boundaries are lost) separate from payload errors (skippable).
2. *"Where does gap detection belong?"* In the transport, which carries sequence numbers
   (MoldUDP64). Not in ITCH timestamps. When the sequencer can't fill a hole it stops rather
   than skipping, because a book after a skipped message is silently wrong.
3. *"How did you find the bottleneck without a profiler?"* Allocation counts and per-message
   type costs, then on real data a throwaway flat-array experiment that isolated the order
   index (91 → 60 ns/msg) before building a bounded production version.
4. *"What did real data teach you that synthetic data did not?"* 43 % of adds open a new
   price level (synthetic: 1.6 %), books are thousands of levels deep, and crossed books are
   real but only in halted or paused symbols.
5. *"Tell me about a tail-latency bug."* The max sample hit the same message in every trial.
   It was a vector doubling at ≈ 1 M orders and copying ≈ 50 MB. Chunked storage fixed it,
   and an A/B against a control binary priced the fix at ≈ 1 ns/msg.
6. *"Memory and lifetime?"* The pmr pool must outlive its containers. The engine is
   therefore movable but not move-assignable (the compiler then rejected an unsafe defaulted
   operator). Level objects live in a stable deque pool, and Node pointers stay valid until a
   level's last order leaves.
7. *"Durability?"* Acknowledge only after sync. Retries are idempotent across restarts. Torn
   tails are repaired while corruption refuses startup. Fail-stop on I/O errors.

## Honest limits

* No UDP receiver, live retransmission or SoupBinTCP. MoldUDP64 is tested on simulated
  channels.
* Real data covers the first ≈ 6.5 h of three days, not full sessions or closing crosses.
* GCC, ASan, TSan and clang-format have not run on the sprint's code. The sandbox blocked
  them locally, and the branch has not been pushed to CI. UBSan, coverage, fuzzing and leak
  checks ran.
* Sorted-vector price levels have no adversarial bound (documented). The `std::map` variant
  is one build flag away.
* It is a lab, not trading software: no risk checks, no real connectivity, no
  authentication.
