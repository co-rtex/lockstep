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
| Network | TCP order entry + sequenced `book_update` feed, snapshot handoff, read-only mirror | MoldUDP64 over UDP on loopback (sender + re-request answers, receiver with gap recovery), plus simulated channels |
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

Machine: Apple M3 MacBook Air (fanless), macOS 26.6, Apple clang 17, `-O3`. The ITCH and
matching-core numbers are in-process and single-threaded. The TCP, durable-ack and UDP rows
are loopback measurements on this one machine. None is an exchange wire latency.

| What | Result | Evidence |
|---|---|---|
| ITCH decode (real data) | ≈ 13 ns/msg, ≈ 2.3 GiB/s, 0 allocations | 3 sample days |
| ITCH full replay (real data, 8.7 k symbols, 1.5–1.6 M live orders) | 99 → 55–59 ns/msg after data-driven layout changes (3 days, two runs, at `562c56d`). Later changes: the 20-pair acceptance gate finds no measurable difference (current / old median ratio 1.04 replay, 0.97 apply), but this noisy host cannot resolve a few percent | `itch-real-sample-macos-m3.csv` |
| Book memory (real data) | ≈ 188–211 bytes per live order for the whole process, at 1.5–1.65 M live orders. The book's own structures account for 115–122 B of it: index pages 52–59 B (≈ 7 % occupied), order nodes 41 B, levels ≈ 20 B | `itch-real-memory-macos-m3.csv`, `itch-real-memory-breakdown-macos-m3.csv` |
| Real-data correctness | ≈ 64 M messages over 3 days: 0 decode errors, 0 anomalies under `--strict`, every live order equal to an independent reference model; every field of every message equal to an independent Python decoder, and every price level equal to an independent Python book (final books, plus best bid/ask of every book every 100 k messages) | `scripts/real_data_check.sh`, `tools/itch_pydecode.py`, `tools/itch_pybook.py` |
| Book apply per message, real data | p50 41–42 ns, p99 292–333 ns, p99.9 458–541 ns (3 days, timer included) | `itch-real-latency-macos-m3.csv` |
| Structural worst cases on real data | ≈ 10 ms vector-growth copy removed. A ≈ 95 µs chunk value-initialization on the day's first add was cut to ≈ 30–38 µs. Both found by locating the max sample | benchmarks.md, `itch-node-chunk-ab-macos-m3.txt` |
| Exchange matching core | +14–28 % throughput where nodes recycle (pmr pool), identical state hashes | `r4-core-pool-macos-m3.csv` |
| Exchange core vs history size | idempotency index rebuilt as a keyed flat table: 1 M commands 198 → 119 ns/cmd, 3 M 296 → 196, p99 ≈ halved, identical state hashes | `core-request-index-ab-macos-m3.csv` |
| Durable ack | ≈ 3 ms p50 with `F_FULLFSYNC`; group commit 3.7× throughput with 8 clients | R2/R3 tables |
| Loopback TCP round trip | ≈ 33 µs p50 (1 client, in-memory) | R3 table |
| MoldUDP64 over loopback UDP, real data | all 3 sample days exact at 50 k packets/s with 5 % live / 10 % retransmission loss; reorder buffer 121–147 → 20–21 ns/msg | `mold-udp-loopback-macos-m3.csv`, `mold-sequencer-macos-m3.csv` |

## Correctness work that found real bugs

* **Two server bugs, reproduced before fixing.** A subscriber whose snapshot was too large
  was registered with no baseline. A snapshot after an internal fail-stop could expose
  never-durable state.
* **Eight independent adversarial reviews**. Six led to fixes with regression tests. The
  reviews of the request-id index and of the bounded price levels found no defect. Findings
  included:
  * strict-replay counters drifting after a stop;
  * mirror exceptions escaping a session, and partial application of a malformed update;
  * unclamped directory growth in the paged index;
  * a JSON-output crash on non-ASCII bytes in ITCH alpha fields;
  * a UDP retry timer defeated by partly answered requests;
  * verification tools that could leave a server running when a step failed.

  Automated security review of the UDP transport added three more: no reflection
  amplification, a filter on the feed's source address, and a reorder-buffer memory bound
  that counts the capacity actually held.
* **Mutation checks prove the tests can fail.** A scripted harness (`tools/mutation_check.py`)
  kills 56/56 hand-chosen bugs across decoder, book, indexes, sequencer, mirror, matching,
  validation, journal, the UDP request planner and server. It found two real test gaps, both now covered. One was a
  duplicate of an already-buffered MoldUDP64 message being counted against the buffer bound.
  The other was a `book_update` published although its journal flush had failed.
* **A complexity audit** looked for input-triggered super-linear work and found real issues.
  * The ITCH fallback index used an invertible, unkeyed hash. Crafted order references cost
    43 µs per message at 40 000 adds, quadratic in total. After switching to a keyed SipHash
    (D34) they cost 83–114 ns per message up to 1 M.
  * `ordered_json` made parsing an object O(keys²): 40 000 keys took 1.39 s. Objects are now
    limited to 64 keys, and 1 M keys are refused in 0.10 s.
  * Two linear amplifications are measured and documented rather than fixed: replaying a
    100 000-execution result as a duplicate (≈ 0.15 s each), and full-depth snapshots.
* **Hostile arguments.** A sweep over every CLI flag (`tools/cli_arg_sweep.py`, 1 302 runs)
  found two capacity arguments that allocated until the OS killed the process. Reviewing which
  hostile values `serve` accepted found a real hang: `--send-timeout-ms 0` disabled the send
  timeout, so one subscriber that stopped reading blocked graceful shutdown indefinitely.
  Hostile *content* found a fourth issue: a command naming `request_id` twice was accepted
  with the last value, which makes the idempotency key ambiguous to anyone who reads the
  first. Repeated keys are now `INVALID_REQUEST` (D33). All four are fixed, with regression
  tests.
* **Fuzzing:** a libFuzzer-compatible harness, more than 56 M inputs without a violation (UBSan
  on part of them). **Coverage:** 94 % of lines. **Leaks:** 0. **Fault injection:** a lossy
  TCP relay, a stalled consumer, lossy MoldUDP64 channels with re-request. **Property tests**
  over 100 k hostile exchange requests, 200 k random feed messages, and 400 randomly damaged
  journals (recovery yields an exact prefix or refuses, never a divergent state). Each test asserts
  that its generator actually reaches the interesting paths; that assertion caught three
  generators that were too weak to mean anything.
* **Second implementations in another language.** Three checks are written in Python from the
  specs and compared output for output:
  * an ITCH decoder (every field of 63.7 M real messages);
  * an ITCH book (every price level of three real days);
  * an exchange model (every response of 1 M random commands, and over TCP with a journal
    across a server restart, where resent commands must replay their original results; every
    `book_update` a subscriber receives, about 39 k events, also matches the model). With up
    to 32 concurrent clients it checks linearizability. Accepted replies must equal the model
    run in sequence order, and each rejection must match some state its client could have
    seen. 100 k commands passed. This substitutes for TSan, which cannot run here. A further
    200 k commands from 16 clients passed with hostile connections alongside: 2 544 of them,
    sending garbage, bad lengths, half frames, malformed JSON, stalled subscribers and abrupt
    drops. The server stayed up and answered every malformed message, and the journal replayed
    to the server's final state hash.

  Writing the exchange model was itself a spec check. It first reported a retry's *current*
  order status; the spec says a retry returns the original result, which is what the C++
  does.

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
8. *"What broke when you put it on a real socket?"* Three things, each found by measuring.
   * The receiver re-requested every hole every round, so answers in flight were requested
     again. The result was a retransmission storm: 305 s and 1.8 M requests for a lossless
     run. A 50 ms retry timer per requested range fixed it.
   * An unconnected `sendto()` cost ≈ 69 µs against 1.7 µs for a connected `send()`, so the
     hot paths moved to connected sockets.
   * The reorder buffer cost 121–147 ns/msg behind a hole against 4 ns in order. A receiver
     under steady loss is always behind a hole, so it fell off a cliff. A window over packet
     copies brought that to 20 ns. The differential test proved the semantics unchanged,
     and the remaining ceiling (≈ 50 k packets/s) is consistent with the measured ≈ 12.8 µs
     per `recv()`.
   * That cost is per call, not per byte: 13–15 µs from 64 B to 9 KB. So 8 KB datagrams
     halved a lossless real day on loopback, 11.7 s down to 5.6 s. With 5 % loss they
     gained only 1.25×, because a lost 8 KB packet stalls about 257 messages per retry. To
     show that, the sequencer now reports *which* bound stopped it. It is the look-ahead
     window first, then the byte bound, and it never skips messages.
9. *"Your matching engine gets slower the longer it runs. Why?"* The engine keeps every
   request id so retries are idempotent. In a node-based hash map, each new id missed DRAM
   twice. Benchmarking at four history sizes exposed it (108 → 296 ns/cmd from 100 k to 3 M
   commands). Rehashing was not the cause, since the benchmark already reserves capacity
   before timing. (An audit caught that my extra reserve experiment had been redundant.)
   An isolated probe priced
   the miss (≈ 75 vs 23 ns). The replacement is a flat fingerprint table over a record deque,
   keyed with SipHash because the ids come from clients. The rewrite only counts if the state
   hashes are identical, and they are.
10. *"How do you test a concurrent server without TSan?"* Make the server's interleaving the
    thing under test. K clients send concurrently, and the accepted replies, ordered by their
    sequence numbers, must equal a single-threaded model run in that order. A rejection has
    no sequence number. Per-connection FIFO bounds when it can have run, between its client's
    neighbouring accepted commands, so it must match the model in some state in that window.
    Corrupting one observed reply, even into a plausible error code, makes the check fail.
11. *"An optimization you rejected?"* Software prefetch of the order index and node a few
    messages ahead. The flat-array experiment pointed at cache misses, so it looked right.
    On this noisy machine, 3-trial medians ranged from 50 to 106 ns/msg in *both* arms, so I
    switched to 20 back-to-back pairs. The median ratio was 0.99. The code was reverted, and
    the patch and raw pairs are kept.

## Honest limits

* MoldUDP64 over UDP has only run on loopback (unicast, the sender doubles as the re-request
  server). There is no multicast and no SoupBinTCP. Here it is exact up to ≈ 50 k packets/s,
  a limit consistent with a measured ≈ 12.8 µs per `recv()` (the receiver was not profiled).
* Real data covers the first ≈ 6.5 h of three days, not full sessions or closing crosses.
* GCC, ASan, TSan and clang-format have not run on the sprint's code. The sandbox blocked
  them locally, and the branch has not been pushed to CI. UBSan, a hardened-libc++ build
  (bounds-checked containers and spans: whole suite, a real day, 160 k fuzz inputs), coverage,
  fuzzing and leak checks ran.
* The price-level structure is tuned for real books (≤ 3.8 k levels per side here). Its
  adversarial worst case was measured at 25 µs/msg and is now bounded (96 ns/msg, D32), but a
  monotone-improving 200 k-level side is slower than before (54 vs 17 ns/msg).
* Without authentication, a client can make the sequencer do work linear in a previous
  result per small request. That happens with duplicates of a huge sweep and with full-depth
  snapshots of a huge book. Both are measured, and neither is rate-limited.
* It is a lab, not trading software: no risk checks, no real connectivity, no
  authentication.
