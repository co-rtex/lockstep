# EngineX

EngineX is a deterministic C++20 limit-order matching engine, built as an educational
trading-systems lab. It accepts limit buy and sell orders and cancellations for a small set
of symbols. It matches them by price-time priority (best price first, then first-come), with
partial and full fills at the resting order's price. Every accepted command gets a sequence
number, so the same input always produces byte-for-byte the same state, fills and ids. That
state can be hashed, replayed and compared.

The project is built in releases. R1 is the matching core, its tests, a command-line
simulator and a reproducible benchmark suite. R2 adds a write-ahead command journal with
crash recovery and idempotent retries. R3 adds a TCP order-entry and market-data service
with bounded queues and snapshot resynchronization. EngineX is a standalone personal project
inspired by the club's [TradeGrid](https://github.com/co-rtex/tradegrid) Go exchange. It is
not a replacement for it and not real trading software.

A second, independent system decodes **Nasdaq TotalView-ITCH 5.0** binary market data and
rebuilds per-symbol order books from it ([docs/itch.md](docs/itch.md); the engineering story,
with rejected options and measurements, is in [docs/case-study.md](docs/case-study.md), and a
one-page summary for reviewers is [docs/QUANT_SWE_CASE_STUDY.md](docs/QUANT_SWE_CASE_STUDY.md)). ITCH is an outbound
market-data feed, not an order-entry protocol. That path never sends anything to the
synthetic exchange. Its committed test data are synthetic fixtures built from the public
specification. It was also validated locally on three public Nasdaq sample days (≈ 64 M real
messages in total), which are not redistributed (`scripts/real_data_check.sh` reproduces the
runs).

## Status

| Feature | Release | Status |
|---|---|---|
| Price-time matching, partial/full fills, resting-price execution, multi-symbol books | R1 | **Implemented, tested** |
| Cancellation with O(1) order index | R1 | **Implemented, tested** |
| Atomic validation with stable error codes; deterministic sequencing; idempotent request ids | R1 | **Implemented, tested** |
| Canonical state serialization + SHA-256 state hash | R1 | **Implemented, tested** |
| Property tests against an independent reference model; TradeGrid common fixtures | R1 | **Implemented, passing** |
| `enginex simulate` CLI with demo and golden output | R1 | **Implemented, tested** |
| Seeded benchmark suite (6 workloads), CSV output, recorded results | R1 | **Implemented, measured** |
| Linux builds: GCC, Clang ASan/UBSan, TSan | R1 | **Passing on GitHub-hosted Ubuntu 24.04 runners** and in a local Ubuntu 24.04 container |
| GitHub Actions CI workflow | R1 | **Passing on GitHub**: all 5 jobs green on commit `418d247` ([run 40243146221](https://github.com/co-rtex/enginex/actions/runs/40243146221)); also reproducible locally with `act` (`scripts/ci_local.sh`) |
| Versioned append-only journal (EXJ1, CRC32C framing), ack after `fdatasync`/`F_FULLFSYNC` | R2 | **Implemented, tested** |
| Startup replay with verification, tail repair, corruption refusal, fail-stop on I/O errors | R2 | **Implemented, tested** (fork crash points, SIGKILL harness, injected write/sync/ENOSPC failures) |
| Idempotent retries across restarts | R2 | **Implemented, tested** |
| `enginex run` / `enginex replay --verify`, durable-ack and recovery benchmarks | R2 | **Implemented, measured** |
| TCP server: length-prefixed JSON protocol v1, multiple clients, single sequencer with group commit | R3 | **Implemented, tested** |
| Sequenced `book_update` feed, atomic snapshot/subscribe handoff (event watermark), reconnect resync | R3 | **Implemented, tested** (handoff under load with book mirroring) |
| Bounded ingress/outbound queues, slow-consumer disconnect, connection cap, graceful shutdown | R3 | **Implemented, tested** (TSan clean on Linux) |
| `enginex serve` / `enginex client`, network round-trip benchmark | R3 | **Implemented, measured** |
| Read-only feed mirror (`enginex client mirror`): refuses updates without a baseline or after a gap, resubscribes after cut-off, in-band snapshot verification | Sprint | **Implemented, tested** (slow-consumer cut-off and oversized-baseline scenarios over real sockets) |
| ITCH 5.0 decoder: all 23 message types, bounds-checked big-endian fields, incremental length-prefixed file framing | ITCH | **Implemented, tested** (golden bytes, Python-packed fixtures, malformed/truncated input) |
| ITCH order-book reconstruction: add/execute/cancel/delete/replace, FIFO priority, depth, anomaly policy, deterministic state hash | ITCH | **Implemented, tested** (differential vs. a naive reference model, corrupted-file fuzzing) |
| `enginex itch generate / replay / dump`, ITCH decode/reconstruction benchmark with allocation counts | ITCH | **Implemented, measured** |
| MoldUDP64 packet parsing and sequencing: duplicates, overlap, bounded reordering, gap detection with re-request ranges, fail-stop instead of skipping | ITCH | **Implemented, tested** on simulated lossy/reordering channels and over loopback UDP (UDP row below) |
| Validation on real Nasdaq data: three public sample days (2019-01-30, 2019-03-27, 2020-01-30), ≈ 21 M messages each through the 09:30 open; zero decode errors, zero book anomalies, every live order matching the reference model; crossed books only in halted/paused symbols | ITCH | **Verified locally** (data not redistributed; `scripts/real_data_check.sh`; docs/itch.md) |
| MoldUDP64 over real UDP (`itch mold-send` / `mold-recv`): paced feed, re-request answers, receiver gap recovery; exact book on all three real sample days at 50 k packets/s with 5 % live and 10 % retransmission loss | ITCH | **Implemented, tested on loopback only** (`scripts/mold_udp_demo.sh`) |
| UDP multicast, a separate re-request server, SoupBinTCP, full-day real data | — | **Not implemented / not tested** (no such claim is made) |

The CI and Linux results in this table cover R1–R3 at commit `418d247`. The rows marked
*ITCH* and *Sprint* have been verified locally only: macOS, Apple clang 17, release suite,
UBSan, coverage and fuzz runs. GCC, ASan and TSan have not run on them yet
([docs/PROGRESS.md](docs/PROGRESS.md#market-data-sprint-2026-11-10-branch-enginex-quant-sprint-20261109)).

## Architecture

```text
 TradeGrid (club repo, Go) ── public matching spec + fixtures (read-only, attributed) ──┐
                                                                                       ▼
 ┌──────────────────────────────── EngineX (this repo) ────────────────────────────────┐
 │  TCP clients ⇄ enginex_net: acceptor · reader/writer thread per connection (R3)     │
 │   │ length-prefixed JSON         bounded ingress queue      bounded outbound queues │
 │   ▼                                     ▼                          ▲                │
 │  enginex CLI ── JSON lines ──▶ enginex_codec (JSON boundary only)  │ acks + events  │
 │                                         │ typed Command            │ after sync     │
 │                                         ▼                          │                │
 │                SEQUENCER: ExchangeStateMachine (single owner of all state)          │
 │                 prepare() pure validation → commit() sequence + apply               │
 │                     │                                     │                         │
 │   MatchingEngine: per-symbol OrderBook             DurableExchange (R2)             │
 │    bids/asks: pmr::map<price, PriceLevel{FIFO       group commit → EXJ1 journal     │
 │    pmr::list}>, index pmr::unordered_map (pool)     write + fdatasync/F_FULLFSYNC   │
 └─────────────────────────────────────────────────────────────────────────────────────┘

 Separate market-data path (no connection to the exchange above):

 ITCH 5.0 bytes ─┬─ file: StreamFramer (2-byte length prefix) ─┐
                 └─ MoldUDP64 packets: parse → Sequencer ──────┤  (dedup, reorder, gaps,
                                                               │   re-request, fail-stop)
                                                               ▼
                         decode(): 23 message types, bounds-checked big-endian fields
                                                               ▼
       BookBuilder: paged direct order index → chunked node pool → per-locate books,
       sorted-vector price levels with FIFO queues → depth / top / SHA-256 state hash
```

There is no runtime connection to TradeGrid. Details are in
[docs/architecture.md](docs/architecture.md) (why one sequencer owns the book, operation
complexity, iterator safety, threading), [docs/journaling.md](docs/journaling.md)
(durability contract, crash matrix, recovery rules) and [docs/protocol.md](docs/protocol.md)
(wire format, ordering, handoff, backpressure).

## Supported platforms and prerequisites

* macOS (Apple clang 17 tested) and Linux (GCC 13, Clang 18 tested in Ubuntu 24.04).
* CMake ≥ 3.24, Ninja, a C++20 compiler, Python 3 (for tools and smoke tests).
* Network access on first configure: GoogleTest 1.15.2 and nlohmann/json 3.11.3 are fetched
  with pinned URLs and SHA-256 hashes.

## Build, test, run

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug                         # all unit, property, fixture, journal and CLI tests

cmake --preset release && cmake --build --preset release
./build/release/enginex simulate --input examples/demo_orders.jsonl --output demo-results.json
./build/release/enginex benchmark --seed 42 --orders 100000 --scenario mixed --out benchmarks/results.csv

scripts/check.sh all            # debug+release build/test, CLI smoke tests, format check
scripts/linux_check.sh          # Linux GCC / ASan+UBSan / TSan / release jobs in Docker
```

Durable session (R2):

```bash
mkdir -p data
./build/release/enginex run --journal data/demo.exj --durability sync --input examples/demo_orders.jsonl
./build/release/enginex run --journal data/demo.exj --input /dev/null      # restart: replays 7 records
./build/release/enginex replay --journal data/demo.exj --verify            # prints the same state hash
python3 tools/fault_injection.py --enginex build/release/enginex          # kill -9 + corruption checks
```

Networked service (R3), in three terminals:

```bash
./build/release/enginex serve --port 9000 --journal data/demo.exj --durability sync      # A
./build/release/enginex client subscribe --port 9000                                      # B
./build/release/enginex client submit --port 9000 --input examples/demo_orders.jsonl      # C
```

ITCH 5.0 market-data replay (independent of the exchange). `scripts/itch_demo.sh` runs the
whole walkthrough in one command: golden fixture, malformed input, deterministic replay
twice, injected anomalies (lenient vs `--strict`), MoldUDP64 gap recovery on a simulated
lossy channel, and a short benchmark. `scripts/real_data_check.sh` (opt-in; it downloads
256 MiB per day, 768 MiB for all three) repeats the real Nasdaq sample validation. Individually:

```bash
./build/release/enginex itch replay --input tests/testdata/itch/lifecycle.itch --strict
./build/release/enginex itch generate --out data/demo.itch --seed 42 --messages 200000 --symbols 4
./build/release/enginex itch replay --input data/demo.itch --depth 5 --json
./build/release/enginex itch dump --input tests/testdata/itch/lifecycle.itch --limit 5
./build/release/enginex itch mold-sim --input data/demo.itch --loss-percent 3   # simulated channel
scripts/mold_udp_demo.sh                        # MoldUDP64 over real UDP (loopback), two processes
./build/release/enginex_itch_bench --seed 42 --messages 1000000 --trials 7
```

Output of the first command, on the hand-scripted fixture (Python-packed bytes; the
expected book is derived by hand in `tools/itch_fixture.py`). It is abbreviated here: the
decode-error and state-hash lines are left out and the anomaly counters are shortened.

```text
ITCH 5.0 replay of tests/testdata/itch/lifecycle.itch
  bytes 801, frames 26, decoded 26
  messages by type {"S":6,"R":2,"H":1,"A":6,"F":1,"E":1,"C":2,"X":1,"D":1,"U":2,"P":1,"Q":1,"B":1}
  anomalies {"unknown_order_ref":0, ... all 0}
  live orders 5 (peak 6)
  [1] AAPL  levels 1/1
           220 @ 150.0000        |      400 @ 150.0200
  [2] MSFT  levels 0/1
                                 |       20 @ 400.5000
```

`scripts/demo.sh` runs all of the spec's reviewer flows (R1–R3) in one go, including a crash
and restart, corruption handling and a slow consumer.

The spec's equivalent raw commands also work:
`cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENGINEX_ENABLE_TESTS=ON`.

## Example

Input (`examples/demo_orders.jsonl`, first four lines; this is the spec's §7.2 example):

```json
{"type":"new_limit","request_id":"req-001","symbol":"AAPL","side":"BUY","price":10100,"quantity":10}
{"type":"new_limit","request_id":"req-002","symbol":"AAPL","side":"SELL","price":10000,"quantity":4}
{"type":"new_limit","request_id":"req-003","symbol":"AAPL","side":"SELL","price":10100,"quantity":2}
{"type":"snapshot","symbol":"AAPL","depth":10}
```

Actual output (`head -4 examples/demo_orders.jsonl | enginex simulate --input - --compact`,
responses abbreviated to their key fields):

```text
line 1  req-001 accepted  seq 1  ord-1  BUY 10 @ 10100  remaining 10  OPEN    executions []
line 2  req-002 accepted  seq 2  ord-2  SELL 4 @ 10000  remaining 0   FILLED  exe-1: 4 @ 10100 vs ord-1
line 3  req-003 accepted  seq 3  ord-3  SELL 2 @ 10100  remaining 0   FILLED  exe-2: 2 @ 10100 vs ord-1
line 4  snapshot AAPL as_of_command_sequence 3  bids [{price 10100, quantity 4, order_count 1}]  asks []
final_state: last_command_sequence 3, last_execution_sequence 2, open_orders 1,
             state_hash 97fabc20adfd1b8f2d65213486d705f2173d6b7f152c7a2e59ee5ced8c4b5370
```

Both sells execute at **10100**, the resting buy's price, and four shares remain bid at
10100. The full JSON for the demo, which also covers cancels, a duplicate retry, a request-id
conflict and every validation error, is committed as
[`examples/expected_demo_results.json`](examples/expected_demo_results.json). A test checks it
on every run.

## Guarantees and non-goals

Guaranteed by tests in this repository (R1):

* Matching follows price-time priority and executes at the resting price. No book is left
  crossed after a command. Partial fills keep queue position.
* Rejected commands change nothing and consume no sequence number.
* Retrying an accepted `request_id` with the same body returns the original result. A
  different body is `DUPLICATE_REQUEST_ID_CONFLICT`.
* Identical input produces an identical canonical state and SHA-256 hash.

Guaranteed by tests (R2), under the assumptions in [docs/journaling.md](docs/journaling.md) §5:

* With `--durability sync`, an `accepted` reply is sent only after its journal record has
  been fully written and synchronized. A process killed at any point keeps every acknowledged
  command, and the tests check this at four crash boundaries plus random SIGKILLs.
* Restart replays the journal and reproduces the exact canonical state, ids and sequences.
* An incomplete trailing record (never acknowledged) is preserved to a sidecar file and
  truncated. Any corruption of a complete record refuses startup without modifying the file.
* A write or sync failure stops the process from accepting work (`JOURNAL_IO_ERROR`, outcome
  unknown). After restart, retrying with the same `request_id` yields exactly one logical
  mutation. This is idempotent retry, not exactly-once delivery.

Guaranteed by tests (R3):

* Every accepted command gets a unique, contiguous global sequence from one sequencer, no
  matter how many clients submit concurrently. Replaying the acknowledged commands in
  sequence order reproduces the server's exact state.
* A subscriber's snapshot (`event_watermark = W`) is followed by events `W+1, W+2, …` with no
  gap. Applying the events to the snapshot reproduces the server's book exactly.
* A slow or stalled client never blocks order processing. It is disconnected, after a
  `resync_required` notice when that can still be queued, and has to reconnect for a fresh
  snapshot. Full ingress queues answer `SERVER_OVERLOADED` instead of blocking.
* Framing tolerates any TCP segmentation. Oversized or zero-length frames, malformed JSON,
  trickled partial frames and connection floods are rejected without affecting other
  clients.
* Graceful shutdown processes every queued request and syncs the journal. A restart (even
  after `kill -9`) restores the same state, and event numbering continues.

Not guaranteed: survival of real power loss beyond what the OS and device promise for
`fdatasync`/`F_FULLFSYNC`. That has not been tested, and storage that lies about flushes
defeats it.

Not provided: authentication or encryption on the TCP port (the server refuses non-loopback
binds unless `--allow-remote`), lossless replay of missed events, real money, brokerage or
market connectivity, accounts, risk, margin, authentication, market/IOC/FOK/stop orders, amendments, self-trade prevention, regulatory
features, or production readiness of any kind. This is a learning project.

## Benchmarks

The numbers below are in-process core timings from `ExchangeStateMachine::process`
(validation + sequencing + matching; **no I/O, JSON, durability or networking**). They come
from an Apple M3 MacBook Air running macOS 26.6.2, Apple clang 17, Release build, seed 42,
100 000 measured commands per scenario, with medians of 7 trials. Full methodology, workload
definitions, dispersion and caveats (including the 41.7 ns timer tick on Apple Silicon) are
in [docs/benchmarks.md](docs/benchmarks.md). Raw CSVs are in
[`benchmarks/results/`](benchmarks/results).

| scenario | throughput (cmd/s) | p50 ns | p99 ns | p99.9 ns |
|---|---:|---:|---:|---:|
| no-cross | 6.69M | 125 | 416 | 500 |
| high-cross | 8.12M | 84 | 334 | 458 |
| same-price-burst | 9.71M | 83 | 333 | 417 |
| multi-level-sweep | 5.84M | 125 | 917 | 1,584 |
| cancel-heavy | 8.14M | 125 | 333 | 417 |
| mixed | 8.19M | 125 | 333 | 417 |
| mixed (1M commands) | 4.56M | 166 | 666 | 834 |

These rows are the original R1 measurements. An optional R4 change, which moved book nodes
into a per-engine `std::pmr` pool after an allocation probe found 2–3.5 heap calls per
command, measured +14–28 % in the five scenarios that recycle nodes. Examples: `mixed` 8.18 M
→ 9.63 M cmd/s, `cancel-heavy` 8.12 M → 10.41 M. `no-cross` is unchanged, and every pair
ends in the same state hash
([docs/benchmarks.md § R4](docs/benchmarks.md#r4-optional-result-pooled-book-nodes-in-the-matching-core)).

These are not order-entry or end-to-end latencies. Durable acknowledgment latency is measured
separately and is dominated by the flush. Same machine, `mixed`, seed 42, one sync per
command, medians of 5 trials:

| measurement | acks/s | p50 | p99 |
|---|---:|---:|---:|
| durable ack, `--durability sync` (`F_FULLFSYNC`) | 315 | 3.12 ms | 5.89 ms |
| durable ack, `--durability fsync` (not drive-cache safe on macOS) | 39,979 | 20.0 µs | 51.6 µs |
| durable ack, `--durability none` (`write()` only) | 407,955 | 2.0 µs | 8.9 µs |
| recovery: replay 97,998 records (warm cache) | 3.6M records/s | 27.2 ms total | |

Network round trip over loopback TCP (closed-loop clients; includes JSON, sockets, queues and
the group-commit journal), medians of 5 trials:

| measurement | req/s | p50 | p99 |
|---|---:|---:|---:|
| 1 client, in-memory server | 27,915 | 33.1 µs | 53.8 µs |
| 8 clients, in-memory server | 85,749 | 87.1 µs | 159 µs |
| 1 client, `--durability sync` | 380 | 2.96 ms | 4.55 ms |
| 8 clients, `--durability sync` (group commit: 4.0 commands/sync) | 1,417 | 5.98 ms | 8.23 ms |

ITCH 5.0 market data is a separate in-process workload on the same machine. Unless noted,
rows use 1 M synthetic messages over 8 symbols, medians of 7 trials. Rows are in the order the
changes were made, and each was measured at the commit recorded in its CSV, so the last rows
describe the current default. Methodology, the profiling substitute and the tail
investigation are in [docs/benchmarks.md](docs/benchmarks.md#itch-50-decoding-and-reconstruction).

| measurement | ns/msg | msgs/s | heap allocs/msg |
|---|---:|---:|---:|
| framing + decoding only | 14.6 | 68.5 M (≈2.1 GiB/s) | 0 |
| book apply, `std::unordered_map` order index (baseline) | 52.5 | 19.0 M | 0.482 |
| book apply, open-addressing order index | 32.2 | 31.1 M | 0.008 |
| book apply, open addressing + capacity hint (max latency 613 µs → 1.6 µs) | 26.8 | 37.4 M | 0.008 |
| full replay (framing + decoding + book), open addressing + hint | 37.8 | 26.4 M | 0.008 |
| book apply, 5000 symbols / 2 M msgs, `std::allocator` level nodes → free-list arena | 70.6 → 56.1 | 14.2 M → 17.8 M | 0.304 → 0.003 |
| **real Nasdaq sample** (21.26 M msgs, 8 713 symbols): replay, map + hash → sorted-vector levels + paged direct index | 99.3 → 62.1 | 10.1 M → 16.1 M | ≈ 0 |
| same, + near-top level scan (current; 3 sample days: 56.0 / 58.1 / 54.8, reproduced at `562c56d` as 58.0 / 59.3 / 54.9) | **56.0** | **17.9 M** | ≈ 0 |
| book apply, 1 M msgs / 8 symbols, same change (no capacity hint) | 31.7 → 22.4 | 31.5 M → 44.6 M | ≈ 0 |
| current default (+ chunked node pool, which removes the 10 ms stall), `562c56d`: book apply / full replay, 8 symbols | 24.2 / 35.8 | 41.3 M / 27.9 M | ≈ 0 |
| same, 5000 symbols / 2 M msgs | 44.2 / 56.5 | 22.6 M / 17.7 M | 0.03 |

## Testing strategy

* Unit tests cover every matching, cancellation, validation and idempotency rule in spec
  §11.1.
* Property tests run 100 seeds against a naive, independent reference book and check every
  invariant after every command.
* Fixtures: TradeGrid's published matching examples and fixtures, translated with attribution
  (`tests/testdata/tradegrid/`), plus EngineX-specific cases. `tools/compare_go_fixtures.py`
  runs a differential check against a local TradeGrid checkout.
* Journal tests cover format, corruption, tail repair, injected I/O failures and fork-based
  crashes at every acknowledgment boundary. `tools/fault_injection.py` SIGKILLs both
  `enginex run` and `enginex serve` mid-traffic and verifies durability and idempotent retry.
* Network tests run over real sockets: segmentation, concurrency, handoff under load, slow
  consumers, overload, reconnect, shutdown and restart.
* Golden CLI output, benchmark-workload self-validation, ASan/UBSan and TSan on Linux.
* ITCH: hand-written golden bytes for all 23 message types, fixtures packed independently in
  Python, every wrong length and unknown type, framing split at every offset, a message-by-message
  differential test against a naive reference book (mutation-checked), an independent Python decoder
  that agrees field by field with the C++ one on all 23 types and on 63.7 M real messages, and 300 randomly
  corrupted files. MoldUDP64 sequencing is tested end to end over a simulated lossy,
  duplicating and reordering channel, where the rebuilt book must hash-match a direct replay.
  A libFuzzer-compatible harness (`fuzz/`) also runs as a seeded standalone mutation fuzzer.
  See [docs/itch.md § Verification](docs/itch.md#verification).
* `tools/mutation_check.py` injects 52 hand-chosen bugs one at a time (matching priority,
  validation bounds, journal checks, sequencing, book rules, server fail-stop), and the tests
  kill all of them. It found two real test gaps, both now covered. See
  [docs/verification.md](docs/verification.md#do-the-tests-catch-bugs).

See [docs/verification.md](docs/verification.md) for the full test matrix, the environments
where each suite ran, and known limitations. One of those is that macOS 26.6 + Apple clang 17
sanitizer runtimes fail even on an empty program, so ASan/TSan evidence comes from Linux.

## Relationship to TradeGrid and authorship

EngineX was written independently by Christian Cortez as a personal systems-engineering
project. The TradeGrid club project (<https://github.com/co-rtex/tradegrid>) has its own Go
exchange, roadmap and contributors. EngineX does not modify or replace it. It uses TradeGrid's
public [matching-engine specification](https://github.com/co-rtex/tradegrid/blob/main/docs/matching-engine.md)
and fixtures only as behavioral references, re-expressed in EngineX's own fixture format with
attribution. [docs/tradegrid-compatibility.md](docs/tradegrid-compatibility.md) lists exactly
what is shared and what differs.

Engineering contract: [docs/ENGINEERING_SPEC.md](docs/ENGINEERING_SPEC.md). Design decisions:
[docs/DECISIONS.md](docs/DECISIONS.md). Progress log: [docs/PROGRESS.md](docs/PROGRESS.md).
License: MIT.
