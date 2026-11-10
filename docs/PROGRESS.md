# Progress

Last updated: 2026-11-10 (sprint section below); R1–R3 sections unchanged from 2026-10-25.
Status vocabulary: **done** (verified by a run whose output was
observed), **in progress**, **blocked** (external reason), **outstanding**.

## Current state

* R1 (core): **done** (tag `r1`). GitHub Actions passes on the pushed commit `418d247`:
  all 5 jobs green ([run 40243146221](https://github.com/co-rtex/enginex/actions/runs/40243146221)).
* R2 (durable): **done** (tag `r2`). Verified on macOS, on Linux in Docker, and in GitHub CI
  (GCC, ASan/UBSan, TSan, release; the release job's smoke step runs the R2 journal
  restart/replay and kill -9/corruption harness).
* R3 (networked): **done** (tag `r3`). Verified on macOS, on Linux in Docker, and in GitHub CI
  (the release job's smoke step runs serve + client + graceful shutdown + replay; TSan job
  375/375 with no reports).
* Final end-to-end pass: see "Final verification" below.

## R1 acceptance (spec §16)

| # | Criterion | Status | Evidence |
|---|---|---|---|
| R1.1 | Clean-clone C++20 build on documented compiler and Linux CI | done | GitHub Actions run 40243146221 on `418d247` (fresh checkout on `ubuntu-24.04`, GCC 13.3.0 and Clang 18.1.3): all 5 jobs succeed, 375/375 in each test job. Also: macOS Apple clang 17 debug+release from a fresh clone; `scripts/linux_check.sh`; `act` (`scripts/ci_local.sh`) |
| R1.2 | ≥2 symbols, both sides independent | done | `Matching.SymbolsAreIndependent`, fixture `symbol-isolation`, property tests over AAPL+MSFT |
| R1.3 | Price-time matching, partial/full fills, resting execution price | done | `matching_test.cpp`, 10 TradeGrid fixtures, 100-seed reference-model property tests |
| R1.4 | Cancels update queues, totals, index without invalid references | done | `cancel_test.cpp`, `check_invariants()` after every command, Linux ASan/UBSan clean |
| R1.5 | Invalid requests rejected atomically | done | `test::reject()` byte-compares canonical state around every rejection |
| R1.6 | Canonical state deterministic across runs | done | `canonical_test.cpp`, `PropertyTest.SameSeedIsDeterministic`, identical benchmark hashes across trials |
| R1.7 | Unit, invariant and Go common-fixture tests pass | done | 298/298 ctest; `tools/compare_go_fixtures.py`: 3 pass, 2 skipped (out of scope) |
| R1.8 | ASan/UBSan job passes | done | GitHub job "Linux Clang Debug + ASan/UBSan": 375/375, no sanitizer reports in the log; also the Linux container. (At the R1 tag: Clang 18 ASan+UBSan+LSan 298/298.) macOS ASan runtime hangs even on an empty program (toolchain issue, documented in verification.md) |
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
| R2.1 | All R1 acceptance remains true | done | full suite incl. R1 tests: 375/375 on macOS, in Docker and in every GitHub CI test job (348/348 at the `r2` tag) |
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
| R3.1 | All R2 acceptance remains true | done | full suite 375/375 on macOS, in each Linux container job and in each GitHub CI test job; fault harness passes (locally and in the CI smoke step) |
| R3.2 | TCP framing handles arbitrary segmentation, rejects oversized messages | done | `Framing.*`, `Server.HandlesFragmentedAndCoalescedFrames`, `Server.OversizedFrameIsRejectedAndConnectionClosed`, `Server.TrickledFrameIsDisconnectedAtDeadline` |
| R3.3 | Concurrent clients cannot mutate state out of sequence | done | single sequencer thread (only state owner); `Server.ConcurrentClientsGetUniqueContiguousSequences` (replay in ack-sequence order = server state) |
| R3.4 | Snapshot/feed handoff has correct watermark/gap policy | done | D15; `Server.SubscribeHandoffUnderLoadHasNoGaps` (contiguous `W+1…`, mirrored book = snapshot), `Server.ReconnectGetsFreshSnapshotAndContinues`; docs/protocol.md §5 |
| R3.5 | Bounded queues and slow-consumer disconnect/recovery tested | done | `Server.SlowConsumerIsDisconnectedWithoutStallingTheSequencer`, `Server.OutboundByteBudgetDisconnectsSnapshotHoarder`, `Server.IngressOverloadAnswersEveryRequestExactlyOnce`, `Server.ConnectionLimitIsEnforced` |
| R3.6 | Cleanup and restart produce correct state; no observed races | done | `Server.ShutdownDrainsQueuedWorkAndRestartRecovers`; network SIGKILL scenario in `tools/fault_injection.py`; TSan (Linux Clang 18) 375/375 with no reports, in Docker and in GitHub CI |
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
scripts/ci_local.sh (act 0.2.89)                     -> gcc-debug, clang-asan-ubsan, clang-tsan,
                                                       release, format: all "Job succeeded"
git status                                          -> clean
```

## GitHub CI (2026-10-25)

The user pushed `main` (`418d247`) and tags `r1`, `r2`, `r3` to the private repository
`co-rtex/enginex`. The `ci` workflow ran on the push:

```text
gh run watch 40243146221 -R co-rtex/enginex --exit-status       -> exit 0, conclusion success
  Linux GCC Debug (unit, invariant, fixture, journal, network)   success  375/375
  Linux Clang Debug + ASan/UBSan                                 success  375/375
  Linux Clang + ThreadSanitizer                                  success  375/375
  Linux Release, CLI smoke test and seeded benchmark             success  375/375, smoke OK,
                                                                          ci-benchmark artifact
  clang-format check                                             success  format OK
grep -c 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error:|LeakSanitizer' <run log>
                                                                 -> 0
```

The only annotation is GitHub's deprecation notice that `actions/checkout@v4.2.2` targets
Node.js 20. It is a warning, not a failure. The workflow was not changed after the green run.

Re-verification after the CI run (macOS, working tree = `418d247` + these doc edits):

```text
CLANG_FORMAT=... scripts/check.sh all          -> debug 375/375, release 375/375, smoke OK, format OK
python3 -I tools/compare_go_fixtures.py --fixtures <fresh clone of co-rtex/tradegrid@bb2e124>/docs/fixtures
                                               -> 3 passed, 0 failed, 2 skipped (out of scope)
every test name cited in PROGRESS/verification/journaling/protocol/README
  checked against `enginex_tests --gtest_list_tests`  -> all exist
```

Acceptance audit: every R1–R3 row above was rechecked against the evidence it cites. All
25 required criteria (R1.1–R1.11, R2.1–R2.7, R3.1–R3.7) are met. R4 (optional) is not
started.

Documentation-only commits made after `418d247` (recording this result) have not been pushed,
so GitHub has no run for them. They change no code, build file or workflow.

## Market-data sprint (2026-11-10, branch `enginex-quant-sprint-20261109`)

Local evidence only (macOS, Apple clang 17.0.0 clang-1700.6.4.2). These commits have not been
pushed, so GitHub CI has not run on them.

| Item | Status | Evidence |
|---|---|---|
| P0: subscribe with oversized snapshot no longer registers the subscriber | done | `Server.OversizedSubscribeSnapshotDoesNotRegisterSubscriber` (failed before the fix: event 41 delivered with no baseline) |
| P0: snapshot refused after internal fail-stop | done | `Server.SnapshotRefusedAfterInternalFailStop`; with the guard removed, the snapshot exposed a never-durable bid (`200x7`) |
| P0: fail-stop reply echoes request_id | done | same test (the client previously timed out) |
| ITCH 5.0 decoder, 23 message types, file framing | done | `ItchGolden.*`, `ItchMalformed.*`, `StreamFramer.*`, `ItchFixture.*` |
| ITCH book reconstruction + replay policy | done | `ItchBook.*`, `ItchDifferential.*` (12 seeds), `ItchReplay.*`, `ItchSynthetic.*` |
| Independent review of the ITCH code (separate agent, read-only) | done | 1 confirmed defect (strict replay counters after stop), fixed with regression tests in `ffec5ac`; test-coverage and comment findings addressed |
| Order index optimization, measured | done | docs/benchmarks.md § ITCH; `benchmarks/results/itch-macos-m3.csv` |
| Read-only feed mirror with gap refusal, resubscribe, in-band verification | done | `tests/mirror_test.cpp` (incl. a lossy TCP relay that drops one event, a stalled consumer, an oversized baseline, an unreachable server) |
| MoldUDP64 packet parsing + sequencing (duplicates, reordering, gaps, re-request ranges, fail-stop) | done | `tests/itch_moldudp64_test.cpp` (lossy-channel end-to-end book hash equality; mutation check) |
| Second independent review (mirror, order index, arena, server P0, bench) | done | fixed in `799e7c4`; server ordering guarantee and index/arena correctness confirmed by the reviewer |
| Price-level node arena, symbol-count scaling | done | `benchmarks/results/itch-scaling-macos-m3.csv` |
| Source-based coverage | done | `scripts/coverage.sh`: 94.8 % lines, 85.6 % branches over ITCH/mirror/server/CLI-ITCH (commit `2881634`); 94.7 % lines, 86.0 % branches at `8aaac44` |
| Optional R4: pmr pool for the matching core's book nodes | done | `enginex_core_alloc_probe` (2.0–3.5 → ≈ 1.0 allocs/cmd; 0.03 after D31); `benchmarks/results/r4-core-pool-macos-m3.csv`: +14–28 % where nodes recycle, identical hashes; release and UBSan 472/472 |
| Real-data validation (public Nasdaq sample prefix, local only): 21.26 M msgs decode, 0 anomalies under --strict, reference model agrees on all 1.54 M live orders | done | `ENGINEX_ITCH_REAL_FILE=… ctest`-style run of `ItchRealData.*`; docs/itch.md |
| Real-data-driven layout: sorted-vector levels + paged direct index (replay 99.3 → 62.1 ns/msg) | done | `benchmarks/results/itch-real-sample-macos-m3.csv`, `itch-structures-macos-m3.csv` |
| Real data on three sample days (2019-01-30, 2019-03-27, 2020-01-30): 0 decode errors, 0 anomalies, reference model agrees; all crossed books halted/paused | done | `scripts/real_data_check.sh` (all three dates) |
| Third independent review (sorted-vector levels, paged index) | done | no correctness bug in the new layout; directory growth clamped, spare page, memory bound documented in `deb39c3` |
| Fourth independent review (latest changes): confirmed JSON crash on non-ASCII wire bytes | fixed | `f6f2ffe`; hostile-bytes regression + CLI-level mutation fuzz in `tests/cli_itch_smoke.py` |
| Fifth independent review (UDP transport, reorder buffer) | fixed | `a8225d0`: retry timer keyed by requested range (`RequestPlanner`), POLLERR spin, port sharing, stale end of session, CLI ranges; no Sequencer contract break found |
| Independent Python decoder and book on real data | done | `tools/itch_pydecode.py` (63.7 M messages, every field), `tools/itch_pybook.py` (every level of every final book, plus live orders and every book's best bid/ask every 100 k messages: 210–213 checkpoints per day, identical), 3 days; whole `scripts/real_data_check.sh` re-run at `9028f57` (after D31/D32 and lazy nodes): all checks pass on all 3 days; again at `fee7b06` (with 100 k checkpoints and the day's fixes): exit 0, all checks pass on all 3 days (7.5 min) |
| Layout variants reconstruct identical books | done | ctest `itch_layout_variants_agree` |
| MoldUDP64 over UDP on loopback (D29): sender with re-request answers, receiver, `RequestPlanner`, source filter, answer budget | done | `MoldUdp.*`, `MoldRequestPlanner.*`, `tests/cli_itch_smoke.py`, `scripts/mold_udp_demo.sh`, `mold-udp-loopback-macos-m3.csv` (3 real days exact at 50 k packets/s with 5 %/10 % loss); macOS only |
| Reorder buffer redesign (D30) | done | `mold-sequencer-macos-m3.csv`: 121–147 → 20–21 ns/msg behind holes; set-model differential unchanged |
| R1 core benchmark reproducibility | done | `benchmarks/results/r1-core-macos-m3-rerun-20261110.csv`: same state hashes, medians +0.3 %…+3.6 % |
| Exchange core request-id index (D31): deque of records + keyed flat index | done | `core-request-index-ab-macos-m3.csv`: 198 → 119 ns/cmd at 1 M, 296 → 196 at 3 M, identical state hashes; recovery of 1 M records 2.09 → 4.06 M rec/s |
| Independent Python exchange model | done | `tools/exchange_pymodel.py`; ctest `exchange_pymodel_differential`, `exchange_pymodel_network_restart` (responses, feed events, restart) |
| ITCH order nodes constructed on first use | done | `itch-node-chunk-ab-macos-m3.txt`: first-add sample 92–102 → 30–39 µs; percentiles unchanged |
| Bounded ITCH level vector (D32): adversarial worst case | done | `itch-level-bound-macos-m3.csv`: hostile pattern 25 130 → 96 ns/msg; `ItchLevelBound.*`; real day identical hash at limits 2/16/8192 |
| R2 durable/recovery benchmark reproducibility | done | `r2-durable-macos-m3-rerun-20261110.csv` |
| R3 network benchmark reproducibility after the sprint's server changes | done | `benchmarks/results/r3-network-macos-m3-rerun-20261110.csv`: medians within 1.5 % of the R3 rows (sync p99 −3 %) |

Commands and results (latest):

* `cmake --preset release && cmake --build --preset release && ctest --preset release`:
  516/516 passed (commit `383ca9e`; 1 opt-in real-data test skips without its file). Debug
  build (`cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
  -DENGINEX_ENABLE_TESTS=ON`, local dependency cache): 516/516 at `383ca9e`. UBSan
  (below): 516/516 at `383ca9e`, no runtime errors. Re-run at `f8f605a` after the day's
  fixes: Release, Debug and UBSan 519/519 each.
* `python3 tools/mutation_check.py`: 56/56 mutants killed (≈ 5.8 min, release build, commit `c7b0fc6`); re-run at `9908922` (after the day's fixes to book, server and CLI): 56/56 again (5.9 min).
* UBSan (`cmake -S . -B build/ubsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENGINEX_SANITIZER=undefined`,
  `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ctest --test-dir build/ubsan`): 471/471
  at `799e7c4`; ITCH tests were also re-run under UBSan after every ITCH change.
* UBSan after the real-data layout change (`83bb670`): 476/476, and the opt-in real-data
  differential under UBSan (21.26 M messages, 65 s): passed, no runtime errors. After the
  chunked node pool (`8a40818`): UBSan 479/479, plus 1 000 000 standalone fuzz inputs with
  no violation.
* Leak checks with macOS `leaks --atExit` (LeakSanitizer cannot run here). The full test
  binary minus the fork-based crash-point tests (their `_exit` children hang under the tool):
  466 tests passed, **0 leaks**. Also 0 leaks for `enginex itch replay` of the real sample
  prefix, 20 000 fuzz inputs, and `itch mold-sim`.
* `scripts/real_data_check.sh` (opt-in; network): strict replay of 21 255 695 real messages
  with 0 decode errors and 0 anomalies; reference-model differential OK.
* `MallocScribble=1 MallocGuardEdges=1 MallocCheckHeapStart=1000 MallocCheckHeapEach=10000`
  on the ITCH tests (UBSan build): passed. Re-run at `9ab4162` on the whole gtest binary
  (hardened build, all but the opt-in real-data test): 502 passed. `leaks --atExit` at
  `9ab4162`: 498 tests (fork crash tests and real-data test excluded), 0 leaks; CLI replay with
  `--level-vector-limit 2 --checkpoint-every`, a failing `--strict` replay and `mold-sim`: 0 leaks.
* `python3 tools/itch_fixture.py --check` (ctest `itch_fixtures_up_to_date`): up to date.
* Hardened libc++ (`cmake --preset hardened`, `_LIBCPP_HARDENING_MODE_DEBUG`; a deliberate
  out-of-range `span` index in a scratch program aborts with exit 134 under the same flag):
  516/516 at `7a65a1a` plus the preset; one real day (21 051 558 messages) at level-vector
  limits 8192 and 2: no assertion, Release state hash; `tools/exchange_pymodel.py` with the
  hardened binary (4 × 50 000 simulate, 20 × 3 000 in a 5-tick band, 3 × 2 000 TCP + journal +
  restart): agree; standalone fuzz: 4 seeds × 40 000 inputs, no invariant violated and no assertion. The preset was configured with
  `-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=… -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=…` pointing at
  the Release build's downloaded sources (the sandbox blocks the download).

* Fresh clone (`git clone` of this repository into a temporary directory, Release, pinned
  dependencies from the local download cache): 471/471 passed; `scripts/itch_demo.sh` exit 0.
  Again at `c565469`: 520/520, and the binary reports `git c565469badf4` (not dirty).
* `scripts/check.sh smoke build/release` (R1–R3 CLI smoke incl. kill -9 harness, serve/client,
  network benchmark): `smoke OK`. `scripts/demo.sh`: exit 0 (crash recovery, retry idempotency,
  corruption refusal, slow-consumer cut-off, graceful shutdown).

Outstanding (needs the owner):

* **Residual processes.** Three in-memory test servers started during the sprint are still
  running, each bound to loopback only:
  * PID 62482 on 127.0.0.1:53158, confirmed by a snapshot request on 2026-11-10 ~05:40 EST;
  * PID 11913 on 127.0.0.1:47311;
  * PID 72536 on 127.0.0.1:57375, left by a shutdown test that raised before its cleanup on
    2026-11-10 ~14:40 EST.

  All three were listed by `lsof -nP -iTCP -sTCP:LISTEN` at 14:40. The sandbox denies signals
  to them, and the protocol has no remote shutdown. Stop them with `kill 62482 11913 72536`.

* **clang-format.** CI's format job runs clang-format 19.1.7 with `--dry-run --Werror` over all
  tracked C++ files. clang-format is not installed here, and running the downloaded binary was
  refused by the sandbox ("Code from External"), so the sprint's new files have **not** been
  formatted and that job will fail until someone runs
  `pip install clang-format==19.1.7 && git ls-files '*.cpp' '*.hpp' | xargs clang-format -i`
  and commits the result (formatting only; then re-run the test suite).

Blocked in this environment (exact symptoms), so they are not claimed for the sprint commits:

* ASan: even `int main(){}` built with `-fsanitize=address` hangs after
  `==N==FindDynamicShadowStart, space_size = 0x0fffffffffff` (seen with `verbosity=3`). The
  `asan` preset's test discovery therefore times out.
* TSan: a trivial two-thread program exits 139 (SIGSEGV) with no report.
* Sampling profilers: `sample <pid>` reports the target as not running, and `pgrep` is denied.
  The ITCH optimization used allocation counts and per-type timing instead (docs/benchmarks.md).
* Linux/Docker and GitHub CI: no Docker here, and pushing is not permitted in this sprint.
  In particular, **GCC 13 has not compiled the sprint code**. The CI workflow builds with
  `-Werror` on GCC, and only Apple clang 17 was available locally. Partial mitigation for two
  known GCC-stricter warnings, at `90afbc4`: a clang build with `-Wshadow-all` (GCC's
  `-Wshadow` also covers fields and constructor parameters) gave 0 warnings in project code;
  a scripted search for compound assignments to sub-`int` types (`uint16_t x; x += y`, which
  GCC's `-Wconversion` flags and clang's does not) found none. This does not replace a GCC
  build.

## Blocked / external

* None for R1–R3.
* macOS sanitizers: Apple clang 17 ASan/TSan runtimes fail on macOS 26.6.2 for any program.
  GitHub CI and Linux container evidence are used instead.

## Next tasks

1. Push the sprint branch and confirm CI (ASan/UBSan/TSan on Linux) covers the ITCH and P0
   changes; this needs the owner (pushing was out of scope for the sprint).
2. ~~Optional R4: allocation/layout experiment on the order book~~. Done in the sprint (D26).
3. Optional: move `actions/checkout` to a Node.js 24 release once a push can verify it.
