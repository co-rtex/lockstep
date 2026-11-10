# EngineX implementation plan

Source of truth: [`ENGINEERING_SPEC.md`](ENGINEERING_SPEC.md) (v1.0). This plan breaks the
required releases R1–R3 into small, verifiable tasks. Each task maps to acceptance criteria
(AC) from spec §16. Status lives in [`PROGRESS.md`](PROGRESS.md); design rationale lives in
[`DECISIONS.md`](DECISIONS.md).

AC labels: `R1.1`…`R1.11`, `R2.1`…`R2.7`, `R3.1`…`R3.7` in the order listed in spec §16.

**Status (2026-10-25):** every task below is complete and verified; tags `r1`, `r2`, `r3`
mark the release commits. Evidence per acceptance criterion: [PROGRESS.md](PROGRESS.md).
A few tasks changed during implementation; those changes are noted inline.

## R1 — Core (matching engine, CLI, tests, benchmark, CI)

| # | Task | Verification | AC |
|---|------|--------------|----|
| 1.1 | CMake/Ninja scaffold, presets (debug, release, asan, tsan), pinned GoogleTest + nlohmann/json via `FetchContent` with SHA-256, warnings-as-errors for owned code, `.clang-format`, `.gitignore` | `cmake --preset debug && ctest` builds and runs a placeholder test | R1.1 |
| 1.2 | Domain types, error codes, checked arithmetic | unit tests for overflow helpers | R1.3, R1.5 |
| 1.3 | `OrderBook` (price-level `std::map` + FIFO `std::list`) and `MatchingEngine` (multi-symbol books, active-order index) | matching unit tests (spec §11.1) | R1.2, R1.3 |
| 1.4 | Cancellation through the order index; no dangling iterators | cancel tests incl. ASan | R1.4 |
| 1.5 | `ExchangeStateMachine`: validation, deterministic sequencing, order/execution IDs, idempotent request IDs, atomic rejection | validation and sequence tests | R1.5 |
| 1.6 | Canonical state serializer + SHA-256 | known-answer SHA-256 vectors; repeated-run determinism | R1.6 |
| 1.7 | Invariant checker + seeded property tests against a naive reference matcher | property tests over many seeds | R1.3, R1.7 |
| 1.8 | TradeGrid common fixtures (matching-engine.md worked examples, fixtures 01–03) translated to EngineX format with attribution | `fixtures_test` | R1.7 |
| 1.9 | JSON codec at the boundary; `enginex simulate` CLI; demo files + expected output test | CLI golden test | R1.9 |
| 1.10 | Workload generator (6 scenarios) and `enginex benchmark` harness (warm-up, trials, percentiles, CSV, metadata) | benchmark smoke test; real run recorded in `docs/benchmarks.md` | R1.10 |
| 1.11 | ASan/UBSan local run; GitHub Actions workflow (GCC Debug, Clang ASan/UBSan, Release + smoke + bench, format check, TSan) | local sanitizer runs; Linux container run if Docker available | R1.1, R1.8 |
| 1.12 | README (status table, architecture, commands, real output), architecture.md, verification.md, tradegrid-compatibility.md, benchmarks.md | commands re-run from a clean clone | R1.11 |

## R2 — Durable (journal, replay, idempotency, recovery)

| # | Task | Verification | AC |
|---|------|--------------|----|
| 2.1 | CRC32C (software, known-answer vectors) | unit tests | R2.2 |
| 2.2 | Deterministic binary command payload codec (explicit little-endian) | round-trip + malformed-payload tests | R2.2 |
| 2.3 | Journal file format `EXJ1`: header, framed records, max 4096-byte payloads, open/scan/validate, tail classification, exclusive lock, directory sync | journal tests: header, truncation, CRC mismatch, bad magic/version, sequence gaps | R2.2, R2.6 |
| 2.4 | Storage abstraction with fault injection (failed write, short write, failed sync, ENOSPC) | journal fault tests | R2.6, R2.7 |
| 2.5 | `DurableExchange`: validate → apply in memory → encode journal record → write + sync (group commit) → respond; fail-stop on I/O or apply error. *Changed during implementation:* the original draft said validate → append → sync → apply; the implemented order is the one above (rationale in DECISIONS.md D8: nothing is observable before the ack, and an apply failure happens before the record reaches the journal). Durability modes `sync` / `none` | ack-gating tests; fail-stop tests | R2.3, R2.6 |
| 2.6 | Startup recovery + replay; idempotency map reconstruction; tail repair with preserved bytes | restart tests; repeated restart hash equality | R2.4, R2.5 |
| 2.7 | Crash-point tests via `fork` + `_exit` at each boundary; real `SIGKILL` test through the CLI | recovery_test | R2.7 |
| 2.8 | CLI: `enginex run --journal`, `enginex replay --journal --verify [--repair-tail]`; recovery-time and durable-latency benchmarks | CLI tests; benchmark rows | R2.4, R2.6 |
| 2.9 | `docs/journaling.md` (contract, crash matrix, fsync caveats, failure diagrams); README/R2 status | doc review against tests | R2.6, R2.7 |

## R3 — Networked (TCP ingress, sequenced feed, backpressure, resync)

| # | Task | Verification | AC |
|---|------|--------------|----|
| 3.1 | Length-prefixed framing (big-endian u32, 32 KiB max) with incremental decoder | fragmentation/coalescing tests, oversized rejection | R3.1 |
| 3.2 | Wire protocol codec (`protocol_version: 1`): new_limit, cancel, ack, error, snapshot_request/response, subscribe, book_update, resync_required | codec tests | R3.1, R3.3 |
| 3.3 | Bounded MPSC/SPSC queues (mutex + condvar) | unit + TSan tests | R3.4 |
| 3.4 | Server: acceptor with client cap, per-connection reader/writer threads, single sequencer thread, group commit with ack-after-sync, ordered event publication (+ byte budget, slowloris deadline, loopback-only bind after security review) | integration tests | R3.2, R3.3 |
| 3.5 | Atomic snapshot/subscribe handoff at batch boundaries; event watermark; slow-consumer overflow → `resync_required` + disconnect | handoff-under-load test, slow consumer test | R3.3, R3.4 |
| 3.6 | Client library + CLI (`enginex client submit|subscribe|snapshot`), `enginex serve` with clean shutdown | end-to-end tests; manual demo | R3.5 |
| 3.7 | Network tests: invalid frames, malformed JSON, disconnects, duplicates on retry, reconnect, restart, concurrent clients; TSan build | networking_test under TSan | R3.2, R3.5 |
| 3.8 | Network round-trip benchmark, reported separately from core/durable numbers (`benchmark --mode network`) | `docs/benchmarks.md` | R3.7 |
| 3.9 | `docs/protocol.md`, architecture update, README R3 status, `scripts/demo.sh` | doc review | R3.* |
| 3.10 | Network SIGKILL crash scenario (`tools/fault_injection.py`) | harness passes | R3.6 |

## Final pass

Fresh clone build (debug, release, asan, tsan), full test suites, demo scripts, benchmarks,
README command verification, Linux verification (Docker if available), clean `git status`.

## Market-data sprint (2026-11, branch `enginex-quant-sprint-20261109`)

A second phase after R1–R3, kept separate from the exchange's semantics. It has no spec
acceptance criteria, so each item is closed by its own tests and measurements. The item list
with evidence is in [PROGRESS.md](PROGRESS.md#market-data-sprint-2026-11-10-branch-enginex-quant-sprint-20261109);
rationale is in DECISIONS.md D19–D32.

| # | Work item | Verified by | Status |
|---|---|---|---|
| S.0 | Audit; fix the two server defects (subscribe without baseline, snapshot after fail-stop) | regression tests that failed before the fix | done |
| S.1 | ITCH 5.0 decoder (23 types) and book reconstruction, with a strict anomaly policy | goldens, malformed inputs, reference-model differential, fuzzing, independent Python decoder and book (final state and mid-stream checkpoints), 3 real sample days | done |
| S.2 | Read-only feed mirror; MoldUDP64 sequencing, simulation and loopback UDP transport | gap/refusal/resubscribe tests, lossy relays, set-model differential, real days over UDP | done (macOS loopback only) |
| S.3 | Performance work driven by measurement: ITCH layout, exchange request index (D31), bounded level vector (D32) | interleaved A/B runs with identical state hashes; raw CSVs in `benchmarks/results/` | done |
| S.4 | Independent checks of the exchange: Python model over simulate, TCP, journal and restart | ctest `exchange_pymodel_*` | done |
| S.5 | Verification sweep: mutation testing (56/56), UBSan, hardened libc++, leaks, coverage, doc commands executed | docs/verification.md | done locally |
| S.6 | GCC, ASan and TSan on the sprint code; clang-format | needs a push to CI (blocked in this environment) | outstanding |
