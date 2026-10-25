# Progress

Last updated: 2026-10-25. Status vocabulary: **done** (verified by a run whose output was
observed), **in progress**, **blocked** (external reason), **outstanding**.

## Current state

* R1 (core): **done**. The one item that cannot be finished locally is a GitHub Actions run,
  which needs a push and is not authorized. The equivalent Linux jobs pass in Docker.
* R2 (durable): **done** (tag `r2`). Verified on macOS, and on Linux in Docker under GCC,
  ASan/UBSan and TSan.
* R3 (networked): **done** (tag `r3`). Verified on macOS, and on Linux in Docker under GCC,
  ASan/UBSan, TSan and release.
* Final end-to-end pass: see "Final verification" below.

## R1 acceptance (spec §16)

| # | Criterion | Status | Evidence |
|---|---|---|---|
| R1.1 | Clean-clone C++20 build on documented compiler and Linux CI | done (local) / CI not run | macOS Apple clang 17 debug+release; Linux GCC 13 + Clang 18 via `scripts/linux_check.sh`; workflow `.github/workflows/ci.yml` written, never run on GitHub (no push authorization) |
| R1.2 | ≥2 symbols, both sides independent | done | `Matching.SymbolsAreIndependent`, fixture `symbol-isolation`, property tests over AAPL+MSFT |
| R1.3 | Price-time matching, partial/full fills, resting execution price | done | `matching_test.cpp`, 10 TradeGrid fixtures, 100-seed reference-model property tests |
| R1.4 | Cancels update queues, totals, index without invalid references | done | `cancel_test.cpp`, `check_invariants()` after every command, Linux ASan/UBSan clean |
| R1.5 | Invalid requests rejected atomically | done | `test::reject()` byte-compares canonical state around every rejection |
| R1.6 | Canonical state deterministic across runs | done | `canonical_test.cpp`, `PropertyTest.SameSeedIsDeterministic`, identical benchmark hashes across trials |
| R1.7 | Unit, invariant and Go common-fixture tests pass | done | 298/298 ctest; `tools/compare_go_fixtures.py`: 3 pass, 2 skipped (out of scope) |
| R1.8 | ASan/UBSan job passes | done (Linux container) | Clang 18 ASan+UBSan+LSan 298/298. macOS ASan runtime hangs even on an empty program (toolchain issue, documented in verification.md) |
| R1.9 | `simulate` works on demo with expected output | done | golden test + `scripts/check.sh smoke` |
| R1.10 | Benchmark outputs real results and methodology | done | `benchmarks/results/r1-core-macos-m3*.csv`, `docs/benchmarks.md` |
| R1.11 | README distinguishes completed from planned | done | README status table |

## Commands run (most recent results)

```text
# macOS (Apple M3, macOS 26.6.2, Apple clang 17.0.0)
cmake --preset debug && cmake --build --preset debug && ctest --preset debug -j8
  -> 100% tests passed out of 298
cmake --preset release && cmake --build --preset release
scripts/check.sh smoke build/release            -> smoke OK
CLANG_FORMAT=~/.cache/enginex-tools/bin/clang-format scripts/check.sh format   -> format OK
./build/release/enginex_bench --seed 42 --orders 100000 --trials 7 \
    --out benchmarks/results/r1-core-macos-m3.csv                              -> see docs/benchmarks.md
python3 tools/compare_go_fixtures.py --fixtures <tradegrid>/docs/fixtures      -> 3 passed, 0 failed, 2 skipped

# Linux container (ubuntu:24.04, aarch64)
scripts/linux_check.sh gcc-debug    -> GCC 13.3.0: 100% tests passed out of 298
scripts/linux_check.sh clang-asan   -> Clang 18.1.3 ASan+UBSan: 100% tests passed out of 298
scripts/linux_check.sh clang-tsan   -> Clang 18.1.3 TSan: 100% tests passed out of 298
scripts/linux_check.sh release      -> GCC 13.3.0 Release: 298/298, smoke OK
```

## R2 acceptance (spec §16)

| # | Criterion | Status | Evidence |
|---|---|---|---|
| R2.1 | All R1 acceptance remains true | done | full suite incl. R1 tests: 348/348 macOS and Linux (GCC, ASan/UBSan, TSan, release) |
| R2.2 | Versioned append-only journal, framing, checksum | done | EXJ1 header, length+CRC32C frames, pinned little-endian payload v1 (`journal_test.cpp`), `docs/journaling.md` §1 |
| R2.3 | Sync-mode ack gated on durable append and application | done | `DurableExchange::flush` releases only after write+sync; `IoFailure.*` (mutation-checked); `enginex run` writes each reply after `process()` returns |
| R2.4 | Restart/replay reconstructs canonical state and stable ids | done | `Recovery.Restart*`, `RepeatedRestartsStayIdentical`, `replay --verify` hash = `simulate` hash in smoke tests |
| R2.5 | Same request id retries do not duplicate accepted effects | done | `Idempotency.RetriesAfterRestartReturnOriginalResults`, `CrashBoundary.*`, SIGKILL harness |
| R2.6 | Truncated tail, corruption and I/O errors produce documented behavior | done | `TailRepair.*`, `JournalScan.*`, `Recovery.Corrupted*`, `IoFailure.*`; classification table in `docs/journaling.md` §4 |
| R2.7 | Crash/kill and error-injection tests pass; uncertainty documented | done | fork crash points at 4 boundaries, ctest `cli_fault_injection_kill9` (SIGKILL + in-flight), `docs/journaling.md` §3 (JOURNAL_IO_ERROR = outcome unknown) |

R2 commands run (most recent):

```text
ctest --preset debug -j8                                   -> 100% tests passed out of 348 (macOS)
scripts/linux_check.sh gcc-debug clang-asan clang-tsan release
                                                          -> 348/348 in each job; release smoke OK
scripts/check.sh smoke build/release                       -> smoke OK (includes R2 run/restart/replay,
                                                              fault_injection.py, durable+recovery modes)
python3 tools/fault_injection.py --enginex build/debug/enginex --rounds 8 --seed 1
                                                          -> ALL CHECKS PASSED
./build/release/enginex benchmark --scenario mixed --mode durable --durability {sync,fsync,none} ...
./build/release/enginex benchmark --scenario mixed --mode recovery --orders {100000,1000000} ...
                                                          -> benchmarks/results/r2-durable-macos-m3.csv
```

## R3 acceptance (spec §16)

| # | Criterion | Status | Evidence |
|---|---|---|---|
| R3.1 | All R2 acceptance remains true | done | full suite 375/375 on macOS and in each Linux job; fault harness passes |
| R3.2 | TCP framing handles arbitrary segmentation, rejects oversized messages | done | `Framing.*`, `Server.HandlesFragmentedAndCoalescedFrames`, `Server.OversizedFrameIsRejectedAndConnectionClosed`, `Server.TrickledFrameIsDisconnectedAtDeadline` |
| R3.3 | Concurrent clients cannot mutate state out of sequence | done | single sequencer thread (only state owner); `Server.ConcurrentClientsGetUniqueContiguousSequences` (replay in ack-sequence order = server state) |
| R3.4 | Snapshot/feed handoff has correct watermark/gap policy | done | D15; `Server.SubscribeHandoffUnderLoadHasNoGaps` (contiguous `W+1…`, mirrored book = snapshot), `Server.ReconnectGetsFreshSnapshotAndContinues`; docs/protocol.md §5 |
| R3.5 | Bounded queues and slow-consumer disconnect/recovery tested | done | `Server.SlowConsumerIsDisconnectedWithoutStallingTheSequencer`, `Server.OutboundByteBudgetDisconnectsSnapshotHoarder`, `Server.IngressOverloadAnswersEveryRequestExactlyOnce`, `Server.ConnectionLimitIsEnforced` |
| R3.6 | Cleanup and restart produce correct state; no observed races | done | `Server.ShutdownDrainsQueuedWorkAndRestartRecovers`; network SIGKILL scenario in `tools/fault_injection.py`; TSan (Linux Clang 18) 375/375 with no reports |
| R3.7 | End-to-end latency reported separately from pure matching | done | docs/benchmarks.md measurements 1–4, CSV labels `core_in_process`, `durable_ack_*`, `recovery_replay`, `network_rtt_*` |

R3 commands run (most recent):

```text
ctest --preset debug -j8                                     -> 100% tests passed out of 375
./build/debug/tests/enginex_tests --gtest_filter='Server*:BoundedQueue*' --gtest_repeat=15
                                                             -> all 15 repetitions passed
scripts/linux_check.sh gcc-debug clang-asan clang-tsan release
                                                             -> 375/375 in each job; release smoke OK
python3 tools/fault_injection.py --enginex build/release/enginex --rounds 4 --seed 9
                                                             -> ALL CHECKS PASSED (incl. network kill -9)
scripts/check.sh smoke build/release                         -> smoke OK (R1+R2+R3)
scripts/demo.sh                                              -> demo complete
./build/release/enginex benchmark --mode network --durability {memory,none,sync} --clients {1,8} ...
                                                             -> benchmarks/results/r3-network-macos-m3.csv
```

## Final verification (2026-10-25, commit after `24ca81a`)

An independent read-only review of the server and journal found three defects. All were
fixed with regression tests: idle subscriber fd leak, unbounded ack/event frame size, and
length-field corruption near EOF misread as a torn tail. Its robustness notes and two
automated security reviews were also addressed (DECISIONS.md D18).

```text
fresh clone: CLANG_FORMAT=... scripts/check.sh all   -> debug 375/375, release 375/375, smoke OK, format OK
spec §5.1 commands on the clone (simulate, benchmark, replay --verify, serve)   -> all work as documented
scripts/linux_check.sh gcc-debug clang-asan clang-tsan release
                                                    -> 375/375 in every job; TSan no reports; smoke OK
scripts/demo.sh                                     -> demo complete
git status                                          -> clean
```

## Blocked / external

* GitHub Actions: needs a push to a remote, which these instructions do not authorize.
* macOS sanitizers: Apple clang 17 ASan/TSan runtimes fail on macOS 26.6.2 for any program.
  Linux evidence is used instead.

## Next tasks

1. Final pass: fresh-clone build of every preset, full suites, README command check, Linux
   matrix on the final commit, clean `git status`.
2. Optional R4 (only with measured evidence): allocation/layout experiment on the order book.
