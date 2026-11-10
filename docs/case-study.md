# Case study: a market-data path next to a synthetic exchange

This is the engineering story of the November 2026 sprint on EngineX. EngineX already had a
deterministic matching engine, a durable journal and a TCP order-entry/market-data service
(R1–R3). The sprint added a **historical binary market-data path**: Nasdaq TotalView-ITCH 5.0
decoding, order-book reconstruction and MoldUDP64 sequencing. It also hardened the existing
service. Every number below comes from a run recorded in `benchmarks/results/` or from a
console-only A/B that [benchmarks.md](benchmarks.md) labels as such. Every correctness claim
points at a test.

## 1. Two systems, one repository, no confusion

ITCH is an *outbound* feed: an exchange reporting what happened in its book. An easy shortcut
would have been to turn ITCH adds into EngineX `NewLimitOrder` commands and "replay the market"
through the matching engine. That was rejected (DECISIONS.md D20) because it would re-match
orders Nasdaq had already matched, invent fills, and present market data as order flow. The
ITCH path applies the executions the feed *reports*.

The boundary is enforced by types. `enginex::itch::Side` and `enginex::Side` are different
enums. The compiler caught the one place where both were in scope (`cli_itch.cpp`). The only
shared code is SHA-256 for state hashes.

## 2. Getting the format right before writing code

The field layouts come from Nasdaq's specification PDFs: the current revision, with its
2023-04-28 log entry, and the 2015 version. No PDF tooling was installed, and running a
downloaded parser was correctly refused by the sandbox. So a ~60-line stdlib-only script
inflated the PDF streams and rebuilt the text, and every offset and length came from those
tables. Two lessons:

* The later revision adds `J`, `h` and `O` and moves nothing older. All 23 types are decoded
  and golden-tested.
* The MoldUDP64 PDF uses a font encoding the script could not decode. Rather than trusting
  memory, the 20-byte header layout was confirmed from the FINRA and Japannext copies of the
  same specification and the Wireshark dissector. That provenance is documented, not hidden.

## 3. Design choices that matter

| Problem | Choice | Rejected alternative | Why |
|---|---|---|---|
| Reading big-endian fields | `ByteReader`: length checked first, integers assembled from bytes, every read bounds-checked | `reinterpret_cast` + `ntohl` | No alignment/aliasing UB, host-independent, out-of-range is an exception, not a crash |
| Message boundaries | Separate framing layer (`StreamFramer`, 2-byte length-prefixed files) from payload decoding | Decode straight from a `FILE*` | Chunk-boundary bugs are testable in isolation (every split offset is tested); zero-copy for whole messages |
| Order priority | Pool of nodes + intrusive FIFO per price level; replace = remove + add at the back | Store only aggregated levels | The spec's priority rules (E/X keep position, U loses it) become checkable, and queue position is observable |
| Contradictory input | Reject, count, never repair; `--strict` stops at the first anomaly | Clamp over-executions, auto-create unknown orders | Repairs fabricate state; counting keeps the first cause visible |
| Locked/crossed books | Report, don't reject | Treat as corruption | Real feeds show them transiently |
| Transport gaps | MoldUDP64 sequencer: buffer, re-request, or **stop** | Skip the hole and continue | A book after a skipped message is silently wrong |
| Gaps in files | Not claimed | Infer from timestamps | ITCH timestamps are not sequence numbers |

## 4. Verifying it, with the verifier under suspicion

The tests are designed so that the code and the tests cannot share one mistake:

* **Golden bytes** are written by a test-local writer, independent of the encoder, plus one
  fully literal Add Order. **Fixtures** are packed by Python `struct` from the spec tables,
  and their expected books are derived by hand.
* A **naive reference book** (`std::map`, arrival counters, brute-force levels) is compared
  message by message over 12 seeds, half with injected anomalies. It shares no data
  structures with the implementation. It does share the reading of the spec, so scenario
  tests pin the interpretation separately.
* **Mutation checks**: three deliberately wrong versions were first run by hand and then
  reverted. A replace that keeps priority fails 2 scenario tests and all 12 differential
  seeds. A sequencer that skips gaps fails 5 of the 13 transport tests that existed at the
  time. A snapshot path without the fail-stop check exposes a never-durable bid. Later,
  `tools/mutation_check.py` scripted 51 such mutants across both systems. All are killed,
  after two real test gaps it exposed were closed (see
  [verification.md](verification.md#do-the-tests-catch-bugs)).
* **Adversarial reviews** by a separate agent, which read code only and was told to assume it
  was wrong. The first found a real defect: strict replay kept counting frames after it had
  stopped and could overwrite the stop reason. The second found mirror robustness gaps:
  exceptions escaping the session loop, a malformed update applied partially, an uncounted
  mismatch. It also confirmed the server's ordering guarantee that in-band verification
  relies on. Every finding was turned into a regression test.
* **Coverage** (clang source-based, `scripts/coverage.sh`) was used to find untested code, not
  as a target. It showed the CLI `generate`/`dump` paths, the stream read-error path and the
  capacity hint were never executed. All are tested now. The latest run (commit `9c98867`)
  measured 94.2 % lines and 86.4 % branches over the new code. After the UDP transport
  (`85d38f4`) it was 94.5 % lines and 85.5 % branches.
* **Fault injection** over real sockets: a TCP relay that silently drops one `book_update`
  (the mirror reports exactly one gap, applies nothing after it, resubscribes and verifies);
  a consumer stalled until the server cuts it off; a baseline larger than the frame limit;
  an unreachable server. In-process: a lossy, duplicating, reordering channel with lossy
  re-requests, after which the rebuilt book hash equals a direct replay.

What could not be verified here: ASan and TSan do not start in this sandboxed macOS
environment. Even an empty program hangs or segfaults; the exact symptoms are in
PROGRESS.md. GCC has not compiled the sprint code. UBSan, libmalloc guard modes and the full
suite pass locally. The Linux CI jobs (GCC, ASan/UBSan, TSan) will run once the branch is
pushed.

## 5. Two service defects found and fixed (P0)

1. **Subscriber without a baseline.** If the initial snapshot exceeded the frame limit, the
   server replied with an error but still registered the subscriber, and the client then
   received increments with nothing to apply them to. Reproduced: the test received event 41
   with no snapshot. Fix: registration requires a real snapshot frame.
2. **Non-durable state behind a snapshot.** After an internal fail-stop, snapshot requests
   (unlike subscribes) were still served from memory. Memory could contain commands that were
   staged but never journaled. Reproduced with a new test-only fault hook: a bid that was never
   acknowledged as durable appeared in the snapshot. While testing, a third defect surfaced:
   the fail-stop reply carried no `request_id`, so correlating clients timed out.

## 6. Performance: measured, not assumed

All numbers are in-process, single thread, Apple M3 laptop, Release build, from the CSVs in
`benchmarks/results/`. None of them is a wire-to-wire latency.

1. **Baseline** (1 M synthetic messages, 8 symbols): decoding 14.6 ns/msg (≈ 2.1 GiB/s) with
   zero allocations; book apply 52.5 ns/msg with **0.48 heap allocations per message**.
2. **Finding the cost without a profiler.** `sample` could not attach in this environment.
   Two substitutes were built into the benchmark: exact allocation counts (a counting global
   `operator new`) and a per-message-type cost table. Adds, deletes and replaces, the types
   that touch the order index, were the expensive ones. The index was a
   `std::unordered_map`, with one node allocation per insert.
3. **Change 1: open-addressing index** (linear probing, Fibonacci hash, backward-shift
   deletion). Book apply went to 32.2 ns/msg and allocations to 0.008/msg. The state hash was
   identical, and the old index stays behind a build flag so the A/B can be reproduced from
   one commit.
4. **A regression hidden by the average.** The *maximum* latency got worse (288 → 613 µs).
   The hypothesis was one full rehash while doubling. A capacity hint removed it (max 1.6 µs),
   and apply dropped further to 26.8 ns/msg.
5. **Over-correcting is also measurable.** A hint four times too large (a 32 MiB table) made
   apply *slower* than no hint at all: 46.7 vs 33.7 ns/msg. The table no longer fits in
   cache. A capacity hint should track peak live orders.
6. **Change 2, driven by a scaling sweep.** At similar live-order counts, apply cost rose from
   29.2 ns (8 symbols) to 70.6 ns (5000 symbols). Allocation counts rose with it (0.30/msg),
   from `std::map` nodes for short-lived price levels in many sparse books. A free-list
   arena for those nodes cut 11 % at 1000 symbols and 21 % at 5000, with no change at 8.
   The rest of the growth is cache misses. Optimizing that would require real symbol and
   price distributions, which this project does not have, so it was not attempted.

7. **The same lens on the exchange core (optional R4).** An allocation probe found the
   matching core making 2.0–3.5 heap calls per command. Each resting order cost a list node,
   an index node and sometimes a level node, all freed again on fill or cancel. Moving the
   book's containers to `std::pmr` on one pool per engine gave +14–28 % throughput in the
   scenarios that recycle nodes and no change in the one that never frees (`no-cross`), with
   identical state hashes. The subtle part was lifetime, not speed. A defaulted move
   assignment would have destroyed the pool under live containers. Making the engine
   non-move-assignable turned that into a compile error, and the compiler then flagged the
   state machine's unused move assignment.

8. **Cold caches, measured honestly.** Flushing 64 MiB before each whole-run pass barely
   moved the medians, because a run that builds its book from empty amortizes the cold
   start. The useful question was a *large* book going cold. Timing the 2 000 messages after
   a flush on a book of ≈ 89 k orders gave 2.6–2.9× the warm mean and 3× the warm p99.

9. **Real data overturned the synthetic conclusions.** A 256 MiB range download of a public
   Nasdaq sample gave 21.26 M real messages. All decoded, the book saw zero anomalies under
   `--strict`, and the reference model agreed on all 1.54 M live orders. It also showed that
   the synthetic workload had misled the optimization. Real adds open a new price level 43 %
   of the time (synthetic: 1.6 %), books are thousands of levels deep, and with 1.5 M live
   orders the hash index cost a DRAM miss per lookup. A throwaway flat-array experiment cut
   replay from 91 to 60 ns/msg, which proved the index was the bottleneck. The production
   version is a paged direct index (pages on demand, freed when empty, budget-capped, hash
   fallback) together with sorted-vector price levels. Real-data replay went from 99.3 to
   62.1 ns/msg (−37 %), and the rehash spike disappeared without any capacity hint.

10. **An outlier is a clue when it repeats.** On real data, book apply showed 2–10 ms maxima.
    Making the benchmark report *which* message produced the max showed the same message in
    every trial. The node pool, a `std::vector`, was doubling past ≈ 1 M live orders and
    copying ≈ 50 MB in one message. A chunked pool (64 Ki nodes, never moved) took the max to
    ≈ 100–170 µs at varying positions, which is noise. The fix had a price, and measuring it
    honestly took a controlled A/B. An old commit was built in a worktree, and an unchanged
    control binary first reproduced its recorded number, ruling out thermal drift on a
    fanless laptop. The chunk indirection cost ≈ 1 ns/msg (kept). Exception-handling blocks I
    had added for bad_alloc safety cost ≈ 0.7 ns/msg; they were removed, and the API now
    states that a builder is discarded after an allocation failure.

11. **Generalizing the real-data result.** Two more sample days, from 2019 and 2020, gave the
    same outcome: zero decode errors, zero anomalies, and agreement with the reference model on
    1.6 M live orders each. They also contained crossed books, and every one belonged to a
    halted or LULD-paused symbol. That is a direct, data-backed justification for reporting
    crosses rather than rejecting them, and the replay output now shows the trading state
    next to each crossed book.

## 7. Mistakes worth recording

* The first synthetic generator executed random orders and let crossing adds rest, so every
  book was permanently crossed. That was legal input but a useless workload. It was rebuilt
  with per-side priority sets: marketable orders show up only as executions against the
  contra side's best orders, and a test asserts that clean streams never lock or cross.
* A lossy-proxy test hung at teardown. On macOS, `accept()` ignores `SO_RCVTIMEO`. The fix
  was `poll()`.
* A README percentage ("13–21 %") did not match the CSV rows (11 % and 21 %). It was caught
  while cross-checking and corrected in its own commit.

## 8. Limits and next steps

* Real data: the first 256 MiB of three public sample days (2019-01-30, 2019-03-27,
  2020-01-30), through ≈ 09:33–09:37 ET. Full days, closing crosses and halts in volume
  remain untested.
* MoldUDP64 over UDP has run on loopback only (unicast, sender and re-request server in one
  process), up to ≈ 50 k packets/s here. There is no multicast and no SoupBinTCP.
* The synthetic book grows over a run (≈ 89 k live orders after 1 M messages). Compare
  per-message costs only at equal message counts.
* Next, with real data in hand: profile a full-day replay with a sampling profiler on Linux
  (`perf`), and evaluate a flatter price-level structure against measured symbol and price
  distributions.
