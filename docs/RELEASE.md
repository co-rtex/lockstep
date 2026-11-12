# EngineX v1.0.0 release record

Date: 2026-11-12. This page says what v1.0.0 contains, what was checked before it was tagged,
and what it does not claim. Details live in the linked documents. Nothing here is new
measurement unless it says so.

## Scope

EngineX is an educational C++20 trading-systems lab, not production trading software.
v1.0.0 contains two deliberately separate systems:

* **A synthetic exchange** (spec releases R1–R3, all required criteria met):
  * deterministic price-time matching, cancellation, sequencing and idempotent retries
    (R1);
  * a write-ahead journal with acknowledgment after sync, crash recovery and fail-stop on
    I/O errors (R2);
  * a TCP order-entry and market-data server with bounded queues, snapshot handoff,
    slow-consumer disconnect and a read-only feed mirror (R3, plus the sprint).
  * The optional R4 work, justified by measurement: a `std::pmr` pool for book nodes and a
    keyed flat request-id index.
* **An independent market-data path:** Nasdaq TotalView-ITCH 5.0 decoding (all 23 message
  types) and order-book reconstruction, plus MoldUDP64 sequencing with gap recovery,
  simulated and over loopback UDP. It never sends anything to the exchange (D20).
  * Committed test data are synthetic.
  * Three public Nasdaq sample days were used for validation locally and are not
    redistributed.

Acceptance against the spec: [PROGRESS.md](PROGRESS.md) (R1.1–R1.11, R2.1–R2.7, R3.1–R3.7).

## Changes made in the release closeout

* **Server fix (D37).** The partial-frame (slowloris) deadline disconnected a client that
  kept completing frames when every read ended inside the next frame. The deadline is now
  per frame. The fix is in `71e469c`, with the regression test
  `Server.PipelinedFramesSplitAcrossReadsAreNotTimedOut`, which failed before the fix. An
  independent review of the server and journal found it. A second review, of the ITCH and
  MoldUDP64 code, found no release-critical defect.
* **Documentation.** The documents were reconciled with the passing Linux CI. The CI fixes
  and the GCC warning policy are recorded as D36. This record was added.

No other source file changed after `ae20847`.

## Evidence

| Check | Commit | Result |
|---|---|---|
| Linux CI, all 5 jobs (GCC 13 Debug, GCC 13 Release + smoke + ITCH demo + benchmark smoke, Clang 18 ASan+UBSan+LSan, Clang 18 TSan, clang-format 19.1.7) | `ae20847` | success, [run 43115996592](https://github.com/co-rtex/enginex/actions/runs/43115996592): 532 passed and 1 skipped of 533 in each test job; 0 sanitizer reports ([linux-validation.md](linux-validation.md)) |
| Linux CI, all 5 jobs, release candidate (with D37) | PR head | *recorded after the run* |
| macOS: Release, Debug, UBSan suites | `71e469c` | 534 tests each: 533 passed, 1 skipped (opt-in real data); 0 UBSan runtime errors |
| macOS: `scripts/check.sh smoke`, `scripts/itch_demo.sh`, clang-format 19.1.7 | `71e469c` | smoke OK (R1–R3 CLI, kill -9 harness, serve/client); demo exit 0; `format OK` |
| Mutation harness, the 6 server mutants, re-run after D37 | `71e469c` | 6/6 killed (`python3 tools/mutation_check.py build/release --only "server:"`) |
| Full local matrix: `scripts/verify_all.sh --repeat 3 --real-data --mutation` | `6d93043` | all 16 run steps passed ([summary](../benchmarks/results/verify-all-6d93043-macos-m3.md)): Release ×3, Debug, UBSan, hardened libc++, demos, 1 302-run hostile-argument sweep, 200 000 fuzz inputs, fault injection, three real Nasdaq days, mutation **69/69** |

The production sources at `6d93043` and `ae20847` are identical: `ae20847` changed only
build files and one test. Since `ae20847`, the only source change is D37 in `src/server.cpp`.
It touches only the server's read loop, so the real-data, ITCH, fuzz and journal evidence
from `6d93043` still applies to the release code. The network, fault-injection and Python
exchange-model tests in ctest were re-run with the fix (rows above).

**Benchmarks.** No number was re-measured for the release. Every published number names its
machine (Apple M3 MacBook Air, macOS 26.6, Apple clang 17), its commit and its raw CSV
([benchmarks.md](benchmarks.md)). D37 adds one flag per socket read in the server and is not
on any measured path except the loopback network round trip, which was not re-run. Headline
numbers, each a different thing:

* Matching core, in process, no I/O: `mixed` 10.69 M commands/s, p99 208 ns.
* Real-data ITCH replay, in process: 56.7 ns/msg, final sprint code.
* Durable acknowledgment with `F_FULLFSYNC`: p50 3.12 ms.
* Loopback TCP round trip, one client, in-memory server: p50 33.1 µs.

None of these is an exchange wire latency.

## Known limitations

* Not production software: no authentication or encryption, and the server refuses
  non-loopback binds unless `--allow-remote` is given. No risk checks, accounts, or market,
  IOC or stop orders.
* Durability under real power loss is untested. It relies on `fdatasync`/`F_FULLFSYNC` and
  on honest storage. There is no journal compaction, so memory and recovery time grow with
  history.
* One sequencer thread and one thread per connection, with a 64-client cap.
* Work amplification is measured but not rate-limited: duplicate replays of huge results,
  and full-depth snapshots ([verification.md](verification.md#known-limitations-r3)).
* MoldUDP64 over UDP is loopback unicast only. There is no multicast and no SoupBinTCP.
  Recovery is exact up to ≈ 50 k packets/s on the M3.
* Real data covers the first 256 MiB of three sample days: pre-market through ≈ 09:33–09:37
  ET, ≈ 21 M messages per day. That excludes full sessions and closing crosses.
* Platform coverage:
  * ASan and TSan run only on Linux CI, because the macOS toolchain's runtimes fail.
  * Real data, the mutation harness and all benchmarks ran only on macOS.
  * The coverage-guided libFuzzer build has not been run.
* GCC's five optimizer-dependent warnings are non-fatal under GCC (D36). The release CI log
  shows only the two known false positives in test code.

## Reproduce

```bash
cmake --preset release && cmake --build --preset release && ctest --preset release
scripts/check.sh smoke build/release && scripts/itch_demo.sh build/release
scripts/verify_all.sh --repeat 3 --real-data --mutation   # 26 min on the M3; real data: 768 MiB download
scripts/linux_check.sh                                    # Linux jobs in Docker
```
