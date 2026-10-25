# Progress

Last updated: 2026-10-25. Status vocabulary: **done** (verified by a run whose output was
observed), **in progress**, **blocked** (external reason), **outstanding**.

## Current state

* R1 (core): **done**. The one item that cannot be finished locally is a GitHub Actions run,
  which needs a push and is not authorized. The equivalent Linux jobs pass in Docker.
* R2 (durable): **done** (tag `r2`). Verified on macOS, and on Linux in Docker under GCC,
  ASan/UBSan and TSan.
* R3 (networked): **in progress**. Next task: framing codec and bounded queues.

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

## R3 acceptance (spec §16): outstanding

R3.1–R3.7 not started.

## Blocked / external

* GitHub Actions: needs a push to a remote, which these instructions do not authorize.
* macOS sanitizers: Apple clang 17 ASan/TSan runtimes fail on macOS 26.6.2 for any program.
  Linux evidence is used instead.

## Next tasks

1. R3: length-prefixed framing + incremental decoder; wire protocol codec.
2. R3: bounded queues, server (acceptor, readers, writers, sequencer with group commit).
3. R3: snapshot/subscribe handoff, slow-consumer overflow, client CLI, network tests, benchmark.
