# Linux validation

**Status: passed.** The sprint code (ITCH 5.0, MoldUDP64, feed mirror, server fixes, R4
pool, request-id index) passed all five Linux CI jobs on GitHub-hosted runners. This page
records that run, the failed runs before it and how each blocker was fixed, and the pre-CI
risk list with what actually happened. The R1–R3 code had already passed the same five jobs
at `418d247` ([run 40243146221](https://github.com/co-rtex/enginex/actions/runs/40243146221)).

## The passing run

[Run 43115996592](https://github.com/co-rtex/enginex/actions/runs/43115996592), push of
`ae208476f778` on `enginex-quant-sprint-20261109`, 2026-11-11 22:57–23:04 UTC.
GitHub-hosted `ubuntu-24.04`, image version 20261101.360.1, runner 2.338.2, x86_64.

| Job | Toolchain | Result |
|---|---|---|
| Linux GCC Debug | GCC 13.3.0, `-Werror` | success: 533 tests, 532 passed, 1 skipped (see below), 0 failed; 63 s |
| Linux Release, CLI smoke and seeded benchmark | GCC 13.3.0, `-O3 -Werror` | success: 533 / 532 passed / 1 skipped; `smoke OK`; `scripts/itch_demo.sh` exit 0; six seeded benchmark scenarios; `ci-benchmark` artifact uploaded |
| Linux Clang Debug + ASan/UBSan | Clang 18.1.3, `detect_leaks=1`, `halt_on_error=1` | success: 533 / 532 passed / 1 skipped; 271 s |
| Linux Clang + ThreadSanitizer | Clang 18.1.3, RelWithDebInfo | success: 533 / 532 passed / 1 skipped; 114 s |
| clang-format check | clang-format 19.1.7 (PyPI wheel) | success: `format OK` |

* `grep -c -E 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error:|ERROR: LeakSanitizer'`
  over the whole run log: **0**.
* The skipped test is `ItchRealData.MatchesReferenceModelIfAvailable`. It is opt-in: it
  needs a local copy of a Nasdaq sample file (`ENGINEX_ITCH_REAL_FILE`), which is not
  redistributed. Real-data validation is local only (see [itch.md](itch.md)).
* Compiler warnings: the GCC Debug log has none. The GCC Release log has exactly two, both
  `-Wnull-dereference` in test code (`tests/itch_codec_test.cpp:379` and `:416`): a
  `std::vector` sized by `*message_length(t)` and then indexed at 0. They are false
  positives; `message_length` returns the fixed length (≥ 1) of a known message type. They
  are printed but not fatal because of the GCC-only rule in `cmake/Warnings.cmake`
  ([DECISIONS.md D36](DECISIONS.md#d36-release-ci-fixes-and-gcc-flow-sensitive-warnings)).
  Every other warning is an error under GCC and Clang.
* Two benchmark executables, `enginex_core_alloc_probe` and `enginex_itch_bench`, are not
  built under TSan: they replace global `operator new`/`delete` to count allocations, which
  does not link with TSan's interceptors. Every test target builds and runs under TSan.
* Sanitizer builds use a 600 s per-test timeout (120 s otherwise). The slowest tests under
  ASan/UBSan were `ProtocolFuzz.HostileRequestsNeverBreakTheCommandPath` (165 s) and
  `fuzz_itch_standalone_smoke` (86 s, own 300 s limit).

The same checks reproduced locally on macOS (Apple M3, Apple clang 17) at `ae20847` on
2026-11-12: Release, Debug and UBSan 533 / 532 passed / 1 skipped each, 0 UBSan runtime
errors; `scripts/check.sh smoke` OK; `scripts/itch_demo.sh` exit 0; clang-format 19.1.7
`format OK`. ASan and TSan cannot start on that macOS version (see
[verification.md](verification.md#sanitizers-on-macos-known-toolchain-problem)), so Linux CI
is their only evidence.

The v1.0.0 release closeout then added one server fix and its regression test
([D37](DECISIONS.md#d37-the-partial-frame-deadline-is-per-frame)); the CI runs on that code are
recorded in [RELEASE.md](RELEASE.md) and below.

## Release-candidate runs and one flaky test

On the first release candidate, `19920df`, the push run
[43282131236](https://github.com/co-rtex/enginex/actions/runs/43282131236) passed all 5 jobs.
The pull_request run [43282157156](https://github.com/co-rtex/enginex/actions/runs/43282157156)
of the same commit failed one test in the Release job:
`FeedMirrorSession.StalledMirrorIsCutOffThenResyncsToTheExactBook`. The mirror was cut, it
reconnected, and it saw no gap or mismatch. But it did not end live and verified (last problem
`resync_required: slow_consumer`).

* **Cause.** The test ran 3.04 s. `run_mirror` returns before its 5 s give-up deadline without
  a final verification only when its 10 reconnect attempts are used up. So the recovered mirror
  was cut about ten times in the ≈ 1.5 s after its deliberate stall.
  * The test server's outbound queue holds only 8 frames, so the per-connection writer thread
    has a few hundred microseconds of slack.
  * The two traders ran at full speed until the session ended.
  * On a loaded 4-vCPU runner, that slack is exceeded, and each cut is correct server behaviour.
* **Fix** (`65993a5`, test only). The traders stop 500 ms after the stall instead of at the
  end. The stall still forces the cut-off, and every assertion is unchanged. Failure messages
  now include the connection count and final state.
* **Reproduction.** The failure was not reproduced locally: 0 of 48 runs on macOS and 0 of 70
  in an `ubuntu:24.04` GCC Release container at up to 10-way contention on 2 CPUs. With the
  change, 40 of 40 container runs passed every assertion, including the server's
  slow-consumer cut.

The candidate with the fix, `65993a5`, passed all 5 jobs in both its push run
[43286314612](https://github.com/co-rtex/enginex/actions/runs/43286314612) and its
pull_request run [43286328504](https://github.com/co-rtex/enginex/actions/runs/43286328504).
Runner images were `ubuntu-24.04` 20261101.360.1 and 20261111.370.1. Each test job ran 534
tests: 533 passed and 1 skipped (the opt-in real-data test). There were 0 sanitizer reports,
and the GCC Release log has only the two known warnings.

## How it got there

The sprint was developed in a sandbox without GCC, Docker or GitHub access, and macOS
sanitizers do not run there. The branch was pushed by a sync job, so CI ran on many sprint
commits, but nobody could read the results during the sprint. Every run from `4f3e363`
(2026-11-10) to `6d93043` (2026-11-11) failed. The causes, read from the logs afterwards:

| Cause | Jobs | First seen | Fixed in |
|---|---|---|---|
| clang-format violations in the sprint's new files (802 in 43 files by a later dry run) | format | run 42904632801 (`4f3e363`) | `6d93043`: formatting only (whitespace and `#include` order) |
| GCC `-Werror=null-dereference` false positives in `tests/itch_codec_test.cpp` (Release, `-O3`) | release | run 42904632801 | `3625d9a`: made non-fatal for GCC in `cmake/Warnings.cmake` (D36) |
| GCC: "ignoring return value" of `Json::parse` inside `EXPECT_NO_THROW` (nlohmann/json marks it warn-unused-result, which GCC does not let a `(void)` cast silence) | gcc-debug, release | run 42904632801 | `ae20847`: the parsed body is kept and checked |
| TSan link failure: allocation-counting benchmarks replace `operator new` | tsan | run 42904632801 | `ae20847`: those two benchmarks are not built under TSan |
| `ProtocolFuzz.HostileRequestsNeverBreakTheCommandPath` over the 120 s per-test timeout under ASan/UBSan | asan-ubsan | run 42904632801 | `ae20847`: 600 s timeout for sanitizer builds; no input or test removed |

The last failed run, [43098677254](https://github.com/co-rtex/enginex/actions/runs/43098677254)
on `6d93043`, had only the last three causes left. `ae20847` fixed them, and the next run
passed. No test, sanitizer, input count or warning flag was removed to get there, and
`ae20847` changed no production source file (only `CMakeLists.txt`, `tests/CMakeLists.txt`
and one test).

## Pre-CI risk list, and what happened

Before any CI log could be read, a portability review listed the places most likely to fail
on Linux. Outcomes from the passing run:

| Predicted risk | Outcome on Linux CI |
|---|---|
| GCC-only warnings under `-Werror` (`-Wconversion` on narrow types, `-Wdangling-reference`, `-Wmaybe-uninitialized` around `std::optional`) | Only `-Wnull-dereference` fired (two false positives in test code, above). The `[[nodiscard]]` error was not predicted |
| `MoldUdp.UnpacedBurstIntoASmallSocketBufferStillRebuildsTheExactBook` (Linux doubles `SO_RCVBUF`) | passed in all 4 test jobs (2.5 s Release, 6.3 s ASan, 4.2 s TSan) |
| `FeedMirrorSession.StalledMirrorIsCutOffThenResyncsToTheExactBook` (larger Linux socket buffers) | passed in all 4 |
| `MoldUdp.RequestsFromAnAddressOtherThanTheReceiversAreNotAnswered` (`connect()` to `0.0.0.0` reaching the local host) | passed in all 4 |
| `MoldUdp.AGapRevealedAfterAQuietSpellIsStillRecovered` (receiver starvation under TSan) | passed in all 4 |
| LeakSanitizer had never run on the sprint code | 0 reports |
| `fuzz_itch_standalone_smoke` against the sanitizer time limit | passed (2 000 inputs in sanitizer builds: 86 s under ASan/UBSan) |
| Missing standard includes under libstdc++ (45 added by `tools/include_audit.py`) | GCC builds compiled |
| A sanitizer-slow test over its timeout | yes: `ProtocolFuzz`, fixed by the 600 s sanitizer timeout |

## Reproducing

```bash
scripts/linux_check.sh gcc-debug clang-asan clang-tsan release   # ubuntu:24.04 in Docker
scripts/ci_local.sh                                              # the workflow itself, via act
python3 -m venv /tmp/cf && /tmp/cf/bin/pip install clang-format==19.1.7
CLANG_FORMAT=/tmp/cf/bin/clang-format scripts/check.sh format    # "format OK"
```

The container and `act` runs are close to, but not the same as, the hosted runner. The
GitHub-hosted runs are the authoritative result.

## Not covered on Linux

* Real Nasdaq data (local files, macOS only) and the MoldUDP64 real-day runs over UDP.
* Benchmarks: the CI benchmark is a smoke run with no thresholds. Every published number is
  from the Apple M3 ([benchmarks.md](benchmarks.md)).
* The coverage-guided libFuzzer build (`-DENGINEX_FUZZ_LIBFUZZER=ON`) has not been run.
  The standalone seeded fuzzer runs in every CI test job (`fuzz_itch_standalone_smoke`).
* The mutation harness (`tools/mutation_check.py`) has run on macOS only.
