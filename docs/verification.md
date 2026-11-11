# Verification

This page maps each spec test requirement to the tests that cover it, says where each suite
has actually been run, and lists known gaps. "Passed" means a run whose output was observed.
Commands and results are logged in [PROGRESS.md](PROGRESS.md).

## Test matrix (R1)

| Spec requirement (§11) | Covered by |
|---|---|
| Empty book, non-crossing BUY/SELL, exact-price and better-price crossing | `matching_test.cpp`: `EmptyBookRests*`, `NonCrossingOrdersBothRest`, `ExactPriceCrossTrades`, `BetterPriceCross*` |
| Single, partial-incoming, partial-resting, full fill, multi-level sweep | `matching_test.cpp`: `PartialIncoming*`, `PartialResting*`, `MultiLevelSweep*`, `SweepStopsAtNonMarketableLevel` |
| FIFO across three same-price orders; price beats age | `FifoAcrossThreeSamePriceOrders`, `BetterPriceBeatsOlderOrder`, fixtures `example-4`, `example-6` |
| Resting execution price, both directions; partial resting keeps priority | `BetterPriceCrossExecutesAtResting{Ask,Bid}Price`, `PartiallyFilledRestingRetainsPriority`, `example-4-second-input` |
| Independent symbols; top-N aggregation and sorting | `SymbolsAreIndependent`, `TopNAggregationAndSorting`, fixture `symbol-isolation` |
| Reject missing/invalid symbol, zero/negative/too-large price/quantity, unknown side | `state_test.cpp` `Validation.*`; `json_codec_test.cpp` `FieldErrorsMapToSpecificCodes`, `StructuralErrorsAreInvalidRequest` |
| Overflow-safe aggregation/multiplication; no state change on reject | `types_test.cpp` `Checked.DetectsOverflow`; `LargeQuantitiesAggregateWithoutOverflow`; `test::reject()` compares canonical bytes before and after **every** rejection |
| Cancel first/middle/last/only order; unknown/filled/already-cancelled; no dangling index | `cancel_test.cpp` (12 tests), fixtures `cancel-*`, `check_invariants()` after every accepted command |
| Rejected commands do not consume a sequence; acceptance order deterministic | `Sequencing.*`, `rejections-do-not-mutate` fixture |
| Idempotent retries (§6.1, invariant 10) | `Idempotency.*`, `Cancel.AlreadyCancelledRejectedButRetryReturnsOriginal` |
| Independent model in another language | `tools/exchange_pymodel.py`: matching and command rules written in Python from the spec (price-time priority, resting-price execution, ids, idempotent retries returning the original result, rejection codes in the documented order), compared with `enginex simulate` line by line (status, code, sequence, order id, remaining, order status, cancelled quantity, executions) and on the final books. 200 seeds × 5 000 commands agree: 677 k accepted, 323 k rejected (including multi-fault commands, malformed request ids and unparseable retries), 22 k duplicates, 501 k executions. ctest `exchange_pymodel_differential` runs 10 × 2 000. `--price-band LO:HI` and `--max-qty N` change the regime. 30 × 3 000 commands agreed in each of five regimes (console): the default band, 1–10⁸ (sparse deep books), 9 000–11 000 with quantities up to 5 000, 9 999–10 001 (constant crossing) and 5 000–15 000 with quantities up to 100 000 (large sweeps). A mutant that stops trading at exactly the resting price diverges at line 146 of seed 1. The same model over TCP: `--network` (serve + client submit; 20 × 3 000, plain and journaled), and `--restart` (journal, restart, resend everything; 10 × 2 000: 13 477 of 13 783 second-pass accepts are duplicates replaying their original results, and the rest are commands now valid against the recovered state). In the network modes a subscriber connects before the first command, and every `book_update` must equal the model's event: sequences, command, executions, every touched level with its new quantity and order count, and best bid and ask. That held for 39 291 events over journaled TCP, and for the 306 events published after a restart (the feed continues from the recovered sequence). A longer run, 100 seeds × 3 000 with a restart each (100 server restarts), also agreed: 203 607 duplicate replays and 4 770 post-restart events (console). ctest `exchange_pymodel_network_restart` runs 5 × 1 000. **Concurrency** (`--clients K`): the workload is split over K concurrent connections, so the interleaving is the server's, and the check is a linearizability check. Accepted replies must carry sequences 1..S, increasing per client, and equal the model run in that order. Every rejection or duplicate replay must equal the model's reply in some state between that client's neighbouring accepted commands. The feed and the final books must match. 20 × 5 000 commands from 32 journaled clients agreed: 65 950 accepted, 34 050 other replies placed, 65 950 events, and 65 652 client switches between consecutive sequences, which is evidence of real interleaving. So did 20 × 5 000 from 8 clients in a 3-tick crossing band, and 5 × 3 000 from 16 clients against the hardened-libc++ binary. Corrupting one observed reply made the check fail in all three tries: an execution quantity, an impossible rejection code, and `UNKNOWN_ORDER` instead of `ORDER_NOT_OPEN` for an order that existed in every state the command could have seen. ctest `exchange_pymodel_concurrent` runs 8 clients × 3 seeds × 2 000, journaled. This is an empirical substitute for TSan, which cannot run here. **Hostile connections alongside** (`--chaos M`): M threads keep opening connections while the clients trade. They send raw garbage or impossible frame lengths (0, 32 769, 2³²−1); half-sent frames followed by a drop; malformed JSON, repeated keys, unknown fields or 10 000-deep nesting, several per connection; subscribers that never read; snapshot requests dropped before the reply; and bare connect-then-close. They never send a valid mutating command. The server must stay up. Every malformed message must be answered `INVALID_REQUEST` with the connection kept open, and a timeout or reset counts as a violation. The honest traffic must still be linearizable. With `--journal`, `enginex replay --verify` of the journal must reproduce the server's final state hash. 10 seeds × 20 000 commands from 16 journaled clients alongside 8 hostile threads (2 544 hostile connections) passed, and so did 3 × 10 000 against the hardened-libc++ binary. At `15cfbde` (after the atomic `failed_` change), 12 seeds × 10 000 from 16 journaled clients with 8 hostile threads (1 324 hostile connections) passed too. Making one "malformed" message a valid request made the check fail. ctest `exchange_pymodel_chaos` runs 8 clients with 4 hostile threads |
| Property tests: invariants after every command, against a reference model (§11.2) | `invariants_test.cpp`: 100 seeds × 400 commands compared against `reference_model.hpp` (a naive linear-scan book); 20 000-command stress run; same-seed determinism |
| TradeGrid common fixtures (§11.3) | `fixtures_test.cpp` over `tests/testdata/tradegrid/` (10 cases) + `tools/compare_go_fixtures.py` |
| Canonical state determinism (§8.2) | `canonical_test.cpp`: exact bytes for the spec §7.2 example, repeated runs, streaming hash = string hash |
| CLI demo (§15) | `Simulate.DemoMatchesCommittedExpectedOutput` (golden file) + `scripts/check.sh smoke build/release` |
| Benchmark workloads are what they claim (§12.2) | `bench_test.cpp` (`Scenarios.*`), plus harness hash/count self-checks during every benchmark run |

## Test matrix (R2)

| Spec requirement (§11.4) | Covered by |
|---|---|
| Empty journal startup, valid header creation | `Recovery.CreatesJournalWithSyncedHeader`, `JournalFormat.HeaderBytes` |
| Two sessions: append, restart, compare canonical hash and next sequence ids | `Recovery.RestartReconstructsCanonicalStateAndContinuesSequences`, `scripts/check.sh` R2 smoke |
| Idempotent retry of accepted new order and cancel; changed body rejected | `Idempotency.RetriesAfterRestartReturnOriginalResults` |
| Crash before durable record / after record before reply / after reply | `CrashBoundary.*` (4 boundaries, fork + `_exit` at injected points); `tools/fault_injection.py` (real SIGKILL, in-flight requests) |
| Truncated payload and truncated header at EOF; safe tail handling | `TailRepair.*` (5 cut sizes), `Recovery.TruncatedFrameHeaderIsRepaired`, `IncompleteFileHeaderIsRewritten`, `ZeroFilledTailIsRepaired`, `JournalScan.TruncationAtEveryByteOfTheLastRecordIsAnIncompleteTail` |
| Full-record checksum mismatch; bad version/magic; sequence gap or regression | `JournalScan.ChecksumMismatchIsHardErrorAnywhere`, `BadHeaderIsCorrupt`, `SequenceGapAndRegressionAreCorrupt`, `InvalidLengthsAreCorrupt`, `Recovery.CorruptedRecordRefusesStartupAndLeavesFileUntouched` |
| Simulated failed write, failed fsync, disk full; fail-stop | `IoFailure.*` (EIO, ENOSPC with a partial write, error after a full record, failed sync), `Batch.FailedGroupFlushFailStops` |
| No state/log change after a rejected command | `Recovery.RejectedAndDuplicateCommandsAreNotJournaled` |
| Restart repeatedly; state and fills identical | `Recovery.RepeatedRestartsStayIdentical` |
| Replay divergence (catalog mismatch, duplicated request id, impossible cancel) | `Recovery.ReplayDivergenceIsCorrupt` |
| Single writer; symlink refusal | `Recovery.SecondWriterIsLockedOut`, `Recovery.SymlinkedJournalPathIsRefused` |
| Payload encoding pinned; malformed payloads rejected | `JournalFormat.*`, `Crc32c.KnownAnswers` (RFC 3720 vectors) |
| All sync modes persist across restart | `SyncModes.AllModesPersistAcrossRestart` |
| Journaled CLI end to end | ctest `cli_fault_injection_kill9`, `scripts/check.sh smoke build/release` (run → restart → replay hash = simulate hash) |

## Test matrix (R3)

| Spec requirement (§11.5) | Covered by (`networking_test.cpp` unless noted) |
|---|---|
| TCP fragmentation/coalescing, short reads/writes, invalid length and JSON | `Framing.DecodesAnySegmentation` (chunk sizes 1…100k), `Server.HandlesFragmentedAndCoalescedFrames` (byte-by-byte over a socket, split inside a header), `Framing.RejectsOversizedAndZeroLength…`, `Server.OversizedFrameIsRejectedAndConnectionClosed`, `Server.OrderEntryAckSnapshotAndErrors` (malformed JSON keeps the connection) |
| Concurrent submissions: unique global sequence; single sequencer consistent | `Server.ConcurrentClientsGetUniqueContiguousSequences` (8 clients × 150, contiguous 1..1200, replay in sequence order = server state hash) |
| Snapshot handoff during live mutation without an unreported gap | `Server.SubscribeHandoffUnderLoadHasNoGaps` (subscribe while 3 writers run; contiguous events; mirrored book = fresh snapshot) |
| Slow consumer overflow → disconnect/resync; sequencer stays responsive | `Server.SlowConsumerIsDisconnectedWithoutStallingTheSequencer`, `Server.OutboundByteBudgetDisconnectsSnapshotHoarder` |
| Reconnect gets fresh snapshot and later events in order | `Server.ReconnectGetsFreshSnapshotAndContinues` |
| Duplicate command on timeout/retry → one logical mutation | `Server.RetryAfterDisconnectProducesOneMutation`; `tools/fault_injection.py` network scenario (SIGKILL server, retries) |
| Clean shutdown with outstanding clients, queue saturation, journal flush | `Server.ShutdownDrainsQueuedWorkAndRestartRecovers`, `Server.IngressOverloadAnswersEveryRequestExactlyOnce` |
| Connection cap; partial-frame (slowloris) timeout; loopback-only bind | `Server.ConnectionLimitIsEnforced`, `Server.TrickledFrameIsDisconnectedAtDeadline`, `Server.RefusesNonLoopbackBindWithoutOptIn` |
| Journal failure over the network | `Server.JournalFailureFailStopsOverTheNetwork` |
| Restart after crash: acknowledged commands present, state correct | `tools/fault_injection.py` network kill -9 scenario (also in `scripts/check.sh smoke`) |
| Bounded queue semantics under concurrency | `BoundedQueue.*` (8 producers, per-producer FIFO) |
| No observed races | full suite under ThreadSanitizer (Linux, Clang 18): 375/375 pass, no reports |

The network tests were repeated 15× in a row locally with no failures, to check for
flakiness. One test made a platform assumption about socket buffer sizes that held on macOS
but not on Linux; it now bounds `SO_SNDBUF` and loops until the overflow is observed.

### Do the tests catch bugs?

Deliberate mutations, each reverted after the check:

* Executing at the incoming limit price instead of the resting price failed all 100
  property-test seeds.
* Reversing the expected FIFO order in fixture `example-4` failed that fixture.
* Disabling the journal CRC comparison failed `ChecksumMismatchIsHardErrorAnywhere` and
  `CorruptedRecordRefusesStartupAndLeavesFileUntouched`.
* Releasing an accepted reply even when the flush failed failed all 5 `IoFailure` tests.
* (Sprint) Removing the new `fatal_` check from snapshot requests failed
  `Server.SnapshotRefusedAfterInternalFailStop`: the snapshot exposed a never-durable bid.
* (Sprint, ITCH) Letting a same-price replace keep queue priority failed 2 scenario tests and
  all 12 `ItchDifferential` seeds.

**Scripted mutation check.** `tools/mutation_check.py` applies hand-chosen bugs (56 at first,
69 now) one at a time across the ITCH decoder, book builder, order indexes, framer, MoldUDP64 sequencer,
mirror, matching engine (price-time priority, marketability), idempotency, journal (CRC,
torn-tail classification, truncation), request validation bounds, the UDP request planner
and server (publication and replies after a failed flush, fail-stop). For each one it rebuilds and runs the
relevant tests, then restores the file. It refuses to start when those files have
uncommitted changes and checks afterwards that all were restored. The first run of 22 killed
19 of 20 valid mutants (2 had not compiled under `-Werror` and were rewritten). The one
survivor was real: if a retransmitted copy of a message *already buffered* behind a hole was
not recognised as a duplicate, `std::map::emplace` quietly ignored it, but the buffer
accounting still counted it. A lossy channel would then use up the buffer bound and fail for
no reason. `MoldSequencer.DuplicatesOfBufferedMessagesUseNoCapacity` now kills it. A later batch found a
second gap. If the server published a command's `book_update` although the journal flush had
failed, no test noticed, and subscribers would have seen an event that a restart would
lose. `Server.JournalFailureNeverPublishesTheUndurableEvent` now kills it. One mutant
was equivalent and is excluded with a comment in the script: dropping the event-watermark
rollback after a failed commit cannot be observed, because both failure modes are permanent
fail-stops. Full runs scored **56/56** (≈ 5.8 min on the M3 with an incremental release
build). Eight mutants were added afterwards for the newest code: a constant or ignored key in
the ITCH order index's keyed hash, nested JSON objects sharing their parent's key set, an
unreported repeated key, a key limit off by one, a wrong sequencer failure reason, an
off-by-one `PacketStore::find`, and mold-sim recovery skipping a packet. Each was run on its
own (`--only`) and killed. Two of them, the constant and the ignored key, and the
nested-object mutant first survived every existing test. `OrderIndex.TheKeyDecidesPlacementAndEachIndexDrawsItsOwn`
and `JsonCodec.KeyChecksArePerObjectAtEveryDepth` were added to kill them. Three more cover
the retransmission server's reflection limits: no per-request cap, answers to any source
address, and no answer rate limit. Before this, removing the cap or the source check failed
no test. `MoldUdp.OneRequestIsAnsweredOnlyUpToItsCap` and
`MoldUdp.RequestsFromAnAddressOtherThanTheReceiversAreNotAnswered` now kill them. The full
run at `3ab0ee2` scored **67/67** in 24.7 min. One mutant, an off-by-one `PacketStore::find`,
was killed only by the harness's 900 s timeout: the run hung instead of failing. That exposed
a real liveness bug, fixed in `c19c56d`: when no retransmission ever arrived, `mold-send` and
`mold-recv` both ran forever. A 68th mutant removes the fix. The latest full run (`cc26cb2`,
inside `verify_all.sh`) scored **68/68** in 11 min, and the slowest mutant took 27 s:
with the fix, the `find()` mutant now fails instead of hanging. A 69th mutant covers the follow-up fix (`fcb3d25`, the stall clock starts when a gap opens). The full run at `15cfbde` scored **69/69** in 12.2 min. Run it with
`python3 tools/mutation_check.py [build/release] [--only NAME]`; per-mutant results go to
`mutation_results.json` in the build directory. The latest full run, with up to four failing tests
per mutant (none listed when the test binary crashed), is in
[`benchmarks/results/mutation-check-macos-m3.json`](../benchmarks/results/mutation-check-macos-m3.json). Mutants are written into the source tree, so no other
build may run at the same time: a UBSan build compiled concurrently once picked up a mutated
`book.cpp` and failed 18 tests until rebuilt from clean sources. The mutants are chosen by
hand to probe specific rules, so the score is not a measure of coverage. It complements
`scripts/coverage.sh`.

### Hostile command-line arguments

`python3 tools/cli_arg_sweep.py` (≈ 3.5 min) gives 21 hostile values to every value flag of:
* every offline subcommand,
* `serve`,
* `itch mold-recv`,
* the client commands, run against a closed port.

The values include empty, non-numeric, negative, `2^64`, `2^32`, non-ASCII and 5 000 bytes. The
sweep fails if any run dies on a signal, aborts, prints an uncaught-exception banner, or exits
outside 0–4.

Its first run (861 runs) found two arguments that allocated until the OS killed the process:
`itch replay --expected-orders 2^64−1` and `benchmark --orders 2^32`. Both are now bounded
(`87a883b`; test `ItchBook.ExpectedOrdersAboveCapacityIsRejected`).

Reviewing which hostile `serve` values were *accepted* found a defect the sweep itself cannot
detect, because it is a hang rather than a crash. With `--send-timeout-ms 0`, or `2^64−1`,
which wrapped negative, the writer had no send timeout. One subscriber that stopped reading
then blocked graceful shutdown indefinitely: the server was still running 15 s after SIGTERM,
against 0.2 s at the default. The server now refuses non-positive timeouts (test
`Server.RefusesNonPositiveTimeouts`), and every `*-ms` flag is range-checked (`788e1b1`).

At that commit the sweep passes: 1 302 runs, 0 suspicious. Requests for billions of trials or
generated messages run as asked; the sweep stops them after 20 s and lists them. Capacity flags
such as `--max-clients` still accept any positive value: that is the operator's choice, not
validated.

### Repeated runs (non-determinism)

The suite has real-socket tests that depend on scheduling. A flakiness audit ran the full
`ctest -j8` five times back to back on the loaded laptop (load average ≈ 7).
`MoldUdp.UnpacedBurstIntoASmallSocketBufferStillRebuildsTheExactBook` failed in four of them.
A diagnostic line showed the cause: the receiver buffered exactly the library default of
65 536 messages behind the first hole of a 100 010-message feed, then fail-stopped. That is
the designed outcome when a starved receiver cannot be refilled in time, because it never
skips. The test now uses the CLI's 2²⁰ bound (`aba4124`). The other UDP tests use feeds of
at most 30 000 messages, which cannot reach the bound. Afterwards, 6 consecutive full runs
passed at load average 8.8 (Release), and so did 3 each of the slower Debug and UBSan builds,
521/521 every time at load average 7–8.5. An earlier flake of the same test, a sender that stopped
lingering too soon, had been fixed by raising its timeouts.

## Test matrix (ITCH 5.0 market data)

Full table in [itch.md § Verification](itch.md#verification): golden bytes for all 23 types,
Python-packed fixtures with hand-derived books, malformed and truncated input, framing at every
split offset, scenario tests per book rule, a message-by-message differential test against a
naive reference book, corrupted-file fuzzing, and `OrderIndex` against `std::unordered_map`.
The P0 server regressions (`Server.OversizedSubscribeSnapshotDoesNotRegisterSubscriber`,
`Server.SnapshotRefusedAfterInternalFailStop`) are in `networking_test.cpp`.

MoldUDP64 over real UDP (loopback) is covered by `itch_mold_udp_test.cpp`. The tests cover
lossy delivery to the exact book, stopping at the hole without recovery, hostile datagrams
and requests, kernel drops from an unread socket buffer, and source filtering. The envelope
on real data is in [itch.md § MoldUDP64 over UDP](itch.md#moldudp64-over-udp-loopback).

Three adversarial property tests were also added in the sprint:

* `ProtocolFuzz.HostileRequestsNeverBreakTheCommandPath` (`protocol_fuzz_test.cpp`): 100 k
  hostile client requests (20 seeds × 5 000, each sequence replayed twice) (random bytes, random JSON, damaged and corrupted requests) go
  through `parse_client_message` into a real `ExchangeStateMachine`. Nothing throws, every
  error serializes, invariants hold, and identical inputs reproduce identical state.
* `Recovery.RandomDamageNeverYieldsAStateOutsideThePrefixes` (`recovery_test.cpp`): 400 random
  truncations and bit flips of a journal. Recovery yields an exact journaled prefix or
  refuses, never anything else. Disabling the CRC check fails it.
* `FeedMirror.RandomMessagesNeverBreakTheRules` (`mirror_test.cpp`): 200 k random server
  messages. The mirror applies an update only when live and contiguous, and is stale after
  every failure verdict. Making it accept forward gaps fails this test.

### One command for the local matrix

`scripts/verify_all.sh [--repeat N] [--real-data] [--mutation] [--format]` runs, in order:

1. the Release suite N times;
2. the Debug, UBSan and hardened-libc++ suites;
3. the CLI smoke test, the exchange, ITCH and MoldUDP64 demos;
4. the hostile-argument sweep, 200 000 fuzz inputs and fault injection;
5. optionally the real-data check, clang-format, and the mutation harness, which runs last.

Every step runs even after a failure. Exit codes and durations go to `build/verify/summary.md`;
skipped steps are listed as skipped, and the exit status is non-zero if any step failed.

Its first run (`3aa3d03`) passed 12 of 13 run steps. UBSan failed:
`MoldUdp.LossyLoopbackRebuildsTheExactBook` ended in `gap`, because the sender's 300 ms linger
ran out while a starved receiver was still recovering. That is the same load-dependent cause
as the earlier flake. Fixed in `bf781c8` (2 s linger in both recovery tests); afterwards
UBSan passed 3 of 3 runs at load averages up to 12.

## Where suites have run

| Environment | Compiler | Build | Result |
|---|---|---|---|
| macOS 26.6.2, Apple M3 | Apple clang 17.0.0 | Debug (`cmake --preset debug`) | 375/375 tests pass (R3, commit `418d247`) |
| macOS 26.6, Apple M3 (sprint, final) | Apple clang 17.0.0 | `scripts/verify_all.sh --repeat 3 --real-data --mutation` | At `6d93043` (2026-11-11, after the clang-format commit, 26 min), all 16 run steps passed:<br>• Release 533/533 three times; Debug, UBSan and hardened libc++ 533/533 each<br>• CLI smoke, exchange, ITCH and MoldUDP64 demos, argument sweep, 200 000 fuzz inputs, fault injection<br>• real data on all 3 days (strict replay, reference-model differential, independent Python decoder and book identical), mutation **69/69** (757 s)<br>A fresh clone at the same commit (local dependency cache) built and passed 533/533. clang-format skipped (no 19.1.7 binary here). Summary: [`verify-all-6d93043-macos-m3.md`](../benchmarks/results/verify-all-6d93043-macos-m3.md) |
| macOS 26.6, Apple M3 (sprint, earlier) | Apple clang 17.0.0 | `scripts/verify_all.sh --repeat 3 --real-data --mutation` | At `cc26cb2` (2026-11-11, 27.5 min), all 16 run steps passed:<br>• Release 531/531 three times; Debug, UBSan and hardened libc++ 531/531 each<br>• CLI smoke, exchange, ITCH and MoldUDP64 demos<br>• 1 302-run argument sweep (0 suspicious, 7 timeouts from huge requested workloads), 200 000 fuzz inputs, fault injection<br>• real data on all 3 days, mutation **68/68** (11 min)<br>clang-format skipped: the step needs `--format` with clang-format 19.1.7, which cannot be run here ([linux-validation.md](linux-validation.md)). Summary: [`verify-all-cc26cb2-macos-m3.md`](../benchmarks/results/verify-all-cc26cb2-macos-m3.md) |
| macOS 26.6, Apple M3 (sprint, earlier) | Apple clang 17.0.0 | the same | At `a505d51`, all 16 run steps passed:<br>• Release 525/525 three times; Debug, UBSan and hardened libc++ 525/525 each<br>• CLI smoke, exchange, ITCH and MoldUDP64 demos<br>• 1 302-run argument sweep, 200 000 fuzz inputs, fault injection<br>• real data on all 3 days, mutation 56/56<br>clang-format skipped: the step needs `--format` with clang-format 19.1.7, which cannot be run here ([linux-validation.md](linux-validation.md)). Summary: [`verify-all-a505d51-macos-m3.md`](../benchmarks/results/verify-all-a505d51-macos-m3.md) |
| macOS 26.6, Apple M3 (sprint) | Apple clang 17.0.0 | Release, Debug, Debug + UBSan | Release, Debug and Debug + UBSan (`-fno-sanitize-recover`): UBSan 524/524 at `88bcfea`; UBSan 523/523 at `4f3e363` (after D34 and the JSON key bound); 520/520 each at `05a1607` (Release at `447cc34`, same code); 519/519 each at `f8f605a`; 516/516 each at `383ca9e`; earlier 486/486 in each at `63a2475`. GCC/ASan/TSan not re-run for the sprint (see below) |
| macOS 26.6, Apple M3 (sprint) | Apple clang 17.0.0 | Debug + hardened libc++ (`cmake --preset hardened`: `_LIBCPP_HARDENING_MODE_DEBUG`, bounds-checked containers, spans and iterators) | 524/524 at `88bcfea`; 523/523 at `4f3e363`; 520/520 at `447cc34`, plus `scripts/mold_udp_demo.sh` (lossy MoldUDP64 over loopback UDP, book hash equals the direct replay) and `scripts/itch_demo.sh` with the hardened binaries; 516/516 at `7a65a1a` + preset; one real day (21 051 558 messages) replayed at level-vector limits 8192 and 2 with no assertion and the Release state hash; `tools/exchange_pymodel.py` against the hardened binary: 4 × 50 000 commands, 20 × 3 000 in a 5-tick crossing band, 3 × 2 000 over TCP with journal and restart, all agreeing; fuzz: 160 000 standalone inputs (4 seeds × 40 000, ≈ 6 ms per input in this Debug build), no invariant violated, no assertion |
| macOS 26.6.2, Apple M3 | Apple clang 17.0.0 | Release + `scripts/check.sh smoke` + `scripts/demo.sh` | pass |
| Linux (Docker `ubuntu:24.04`, aarch64) | GCC 13.3.0 | Debug | 375/375 |
| Linux (Docker) | Clang 18.1.3 | Debug + ASan + UBSan (LeakSanitizer on, `halt_on_error`) | 375/375 |
| Linux (Docker) | Clang 18.1.3 | RelWithDebInfo + TSan | 375/375, no race reports |
| Linux (Docker) | GCC 13.3.0 | Release + CLI smoke (R1–R3) + seeded benchmark | pass |
| `.github/workflows/ci.yml` executed with `act` 0.2.89 (Docker, `catthehacker/ubuntu:act-24.04`, arm64) | GCC 13 / Clang 18 as installed by the workflow | all 5 jobs: gcc-debug, clang-asan-ubsan, clang-tsan, release (+ smoke, benchmark, artifact upload), format | **all 5 jobs succeeded**, 375/375 tests in each test job (`scripts/ci_local.sh`) |
| GitHub-hosted Actions (`ubuntu-24.04` image 20261021.618, x86_64), [run 40243146221](https://github.com/co-rtex/enginex/actions/runs/40243146221) on commit `418d247` | GCC 13.3.0 / Clang 18.1.3 | all 5 jobs: gcc-debug, clang-asan-ubsan, clang-tsan, release (+ smoke, benchmark, artifact upload), format | **all 5 jobs succeeded** (2026-10-25, 3 min 27 s); 375/375 tests in each of the 4 test jobs; no ASan/UBSan/LSan/TSan reports in any log; `smoke OK`; `format OK`; `ci-benchmark` artifact uploaded |

`scripts/linux_check.sh [gcc-debug|clang-asan|clang-tsan|release]` reproduces the Linux rows,
and `scripts/ci_local.sh` runs the actual workflow file with `act`. Running it under `act`
exposed a workflow bug (it relied on CMake being preinstalled on the runner image), which is
now fixed: every job installs its own toolchain.
The container is the same Ubuntu release as the CI runners, but it is not the GitHub runner
itself; the GitHub-hosted row above is the authoritative CI result. The seeded benchmark in
the release job has no performance thresholds: shared runners are noisy, so its numbers are a
smoke check only and are not quoted anywhere as results. The only annotation on the run is
GitHub's notice that `actions/checkout@v4.2.2` targets the deprecated Node.js 20 runtime
(the runner executes it on Node.js 24); it is a warning, not a failure.

## Sanitizers on macOS: known toolchain problem

On this macOS 26.6.2 machine with Apple clang 17.0.0, sanitizer runtimes fail **even for an
empty `int main(){}`**: `-fsanitize=address` hangs at startup and `-fsanitize=thread`
segfaults (exit 139); `-fsanitize=undefined` works. This is an OS/toolchain incompatibility,
not an EngineX defect, so ASan/TSan evidence comes from GitHub CI and the Linux container.
The `asan`/`tsan` CMake presets still exist for machines where the runtimes work.
As a partial substitute for ASan's bounds checks, the `hardened` preset builds everything
(including GoogleTest) with libc++'s debug hardening mode (`_GLIBCXX_ASSERTIONS` under
libstdc++), which aborts on out-of-range `operator[]`, `span` access, invalid iterators and
similar library-level misuse. It does not detect raw-pointer overflows, use-after-free or
leaks; those are covered on Linux by ASan/LSan (and locally by `leaks` and MallocGuardEdges).

## Known limitations (R3)

* No authentication or encryption (spec non-goal). Binding to non-loopback addresses
  requires `--allow-remote`.
* Thread per connection: fine for the 64-client cap, not for thousands of connections.
* No historical event replay: recovery is snapshot-based resynchronization only.
* Subscribers receive all symbols; there are no symbol filters.
* Network benchmarks are loopback on one laptop (client and server share CPUs).
* **Work amplification, measured but not mitigated.** These come from a complexity audit and
  are linear per request, not quadratic. Each lets a small request occupy the single
  sequencer thread, which an unauthenticated server cannot prevent.
  * *Duplicate replay of a large result.* A retry returns the original result, so resending a
    sweep that executed K resting orders rebuilds and serializes all K executions every time.
    The retry is never journaled and, past 16 MiB, its reply is truncated. Measured through
    `run` at K = 100 000: ≈ 0.15 s per 114-byte duplicate. The cost is linear in K, and
    quantities allow up to 10⁶.
  * *Full-depth snapshots.* `depth: 0` means the whole book, and `subscribe` always sends
    the whole book. Measured at 200 000 price levels: ≈ 0.14 s per snapshot.

  Possible mitigations, not implemented: a byte-capped cache of encoded large results,
  per-connection rate limits on duplicates and full-depth snapshots, and a cap on depth 0.
  `simulate`/`run` input lines that alternate orders with `executions` queries are also
  O(n²) overall, because each query lists every execution so far. They are local tools.

## Known limitations (R2)

* Power loss and kernel crashes are not tested. Durability under them rests on the documented
  `fdatasync` / `F_FULLFSYNC` guarantees and honest storage (see
  [journaling.md](journaling.md) §5).
* There are no checkpoints, compaction or rotation; recovery replays the whole journal.
* Recovery-time numbers are warm page cache only.

## Known limitations (R1)

* Memory grows with history. Every accepted command (the idempotency map) and every
  execution are kept for the life of the process, because replay and retries depend on them.
  There is no compaction or snapshotting.
* The open-order limit is checked conservatively. `LIMIT_EXCEEDED` is returned at the cap
  even if the new order would fully fill (DECISIONS.md D5).
* The symbol catalog is fixed at startup (`--symbols`); there is no runtime listing.
* Cancellation has no cross-language parity evidence, because TradeGrid's Sprint 2 matching
  spec does not define cancellation.
