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

### Do the tests catch bugs?

Deliberate mutations, each reverted after the check:

* Executing at the incoming limit price instead of the resting price failed all 100
  property-test seeds.
* Reversing the expected FIFO order in fixture `example-4` failed that fixture.
* Disabling the journal CRC comparison failed `ChecksumMismatchIsHardErrorAnywhere` and
  `CorruptedRecordRefusesStartupAndLeavesFileUntouched`.
* Releasing an accepted reply even when the flush failed failed all 5 `IoFailure` tests.

## Where suites have run

| Environment | Compiler | Build | Result |
|---|---|---|---|
| macOS 26.6.2, Apple M3 | Apple clang 17.0.0 | Debug (`cmake --preset debug`) | all tests pass |
| macOS 26.6.2, Apple M3 | Apple clang 17.0.0 | Release + `scripts/check.sh smoke` | pass |
| Linux (Docker `ubuntu:24.04`, aarch64) | GCC 13.3.0 | Debug | all tests pass |
| Linux (Docker) | Clang 18.1.3 | Debug + ASan + UBSan (LeakSanitizer on, `halt_on_error`) | all tests pass |
| Linux (Docker) | Clang 18.1.3 | RelWithDebInfo + TSan | all tests pass |
| Linux (Docker) | GCC 13.3.0 | Release + CLI smoke + seeded benchmark | pass |
| GitHub Actions | — | `.github/workflows/ci.yml` | **not run**: nothing has been pushed (no remote authorization) |

`scripts/linux_check.sh [gcc-debug|clang-asan|clang-tsan|release]` reproduces the Linux rows.
The container is the same Ubuntu release as the CI runners, but it is not the GitHub runner
itself.

## Sanitizers on macOS: known toolchain problem

On this macOS 26.6.2 machine with Apple clang 17.0.0, sanitizer runtimes fail **even for an
empty `int main(){}`**: `-fsanitize=address` hangs at startup and `-fsanitize=thread`
segfaults (exit 139); `-fsanitize=undefined` works. This is an OS/toolchain incompatibility,
not an EngineX defect, so ASan/TSan evidence comes from the Linux container and, once pushed,
from CI. The `asan`/`tsan` CMake presets still exist for machines where the runtimes work.

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
