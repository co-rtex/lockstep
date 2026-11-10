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
| Property tests: invariants after every command, against a reference model (§11.2) | `invariants_test.cpp`: 100 seeds × 400 commands compared against `reference_model.hpp` (a naive linear-scan book); 20 000-command stress run; same-seed determinism |
| TradeGrid common fixtures (§11.3) | `fixtures_test.cpp` over `testdata/tradegrid/` (10 cases) + `tools/compare_go_fixtures.py` |
| Canonical state determinism (§8.2) | `canonical_test.cpp`: exact bytes for the spec §7.2 example, repeated runs, streaming hash = string hash |
| CLI demo (§15) | `Simulate.DemoMatchesCommittedExpectedOutput` (golden file) + `scripts/check.sh smoke` |
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
| Journaled CLI end to end | ctest `cli_fault_injection_kill9`, `scripts/check.sh smoke` (run → restart → replay hash = simulate hash) |

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

**Scripted mutation check.** `tools/mutation_check.py` applies 22 hand-chosen bugs one at a
time across the ITCH decoder, book builder, framer, MoldUDP64 sequencer, mirror, matching
engine, idempotency, journal and server. For each one it rebuilds and runs the relevant tests,
then restores the file. The script refuses to start when those files have uncommitted changes
and checks afterwards that all of them were restored. The first run killed 19 of 20 valid
mutants (2 did not compile because of `-Werror` unused-variable warnings and were rewritten).
The survivor was real: if a retransmitted copy of a message *already buffered* behind a hole
was not recognised as a duplicate, `std::map::emplace` quietly ignored it, but the buffer
accounting still counted it. A lossy channel would then use up the buffer bound and fail
for no reason. `MoldSequencer.DuplicatesOfBufferedMessagesUseNoCapacity` now kills it. The
current score is **22/22**. Run it with `python3 tools/mutation_check.py [build/release]
[--only NAME]`. Each mutant costs one incremental rebuild plus a filtered test run, and the
results go to `mutation_results.json` in the build directory.
These mutants are chosen by hand to probe the specific rules, so the score is not a measure
of coverage. They complement `scripts/coverage.sh`.

## Test matrix (ITCH 5.0 market data)

Full table in [itch.md § Verification](itch.md#verification): golden bytes for all 23 types,
Python-packed fixtures with hand-derived books, malformed and truncated input, framing at every
split offset, scenario tests per book rule, a message-by-message differential test against a
naive reference book, corrupted-file fuzzing, and `OrderIndex` against `std::unordered_map`.
The P0 server regressions (`Server.OversizedSubscribeSnapshotDoesNotRegisterSubscriber`,
`Server.SnapshotRefusedAfterInternalFailStop`) are in `networking_test.cpp`.

Two adversarial tests were also added in the sprint:

* `ProtocolFuzz.HostileRequestsNeverBreakTheCommandPath` (`protocol_fuzz_test.cpp`): 200 k
  hostile client requests (random bytes, random JSON, damaged and corrupted requests) go
  through `parse_client_message` into a real `ExchangeStateMachine`. Nothing throws, every
  error serializes, invariants hold, and identical inputs reproduce identical state.
* `Recovery.RandomDamageNeverYieldsAStateOutsideThePrefixes` (`recovery_test.cpp`): 400 random
  truncations and bit flips of a journal. Recovery yields an exact journaled prefix or
  refuses, never anything else. Disabling the CRC check fails it.
* `FeedMirror.RandomMessagesNeverBreakTheRules` (`mirror_test.cpp`): 200 k random server
  messages. The mirror applies an update only when live and contiguous, and is stale after
  every failure verdict. Making it accept forward gaps fails this test.

## Where suites have run

| Environment | Compiler | Build | Result |
|---|---|---|---|
| macOS 26.6.2, Apple M3 | Apple clang 17.0.0 | Debug (`cmake --preset debug`) | 375/375 tests pass |
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

## Known limitations (R3)

* No authentication or encryption (spec non-goal). Binding to non-loopback addresses
  requires `--allow-remote`.
* Thread per connection: fine for the 64-client cap, not for thousands of connections.
* No historical event replay: recovery is snapshot-based resynchronization only.
* Subscribers receive all symbols; there are no symbol filters.
* Network benchmarks are loopback on one laptop (client and server share CPUs).

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
