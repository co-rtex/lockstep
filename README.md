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
| Linux builds: GCC, Clang ASan/UBSan, TSan | R1 | **Passing in a local Ubuntu 24.04 container** |
| GitHub Actions CI | R1 | Workflow written; **not yet run** (repository not pushed) |
| Append-only journal, replay, crash/corruption recovery | R2 | Planned |
| TCP order entry, sequenced market-data feed, backpressure, resync | R3 | Planned |

## Architecture

```text
 TradeGrid (club repo, Go) ── public matching spec + fixtures (read-only, attributed) ──┐
                                                                                       ▼
 ┌──────────────────────────────── EngineX (this repo) ────────────────────────────────┐
 │  enginex CLI  ──JSON lines──▶  enginex_codec (JSON boundary only)                    │
 │                                     │ typed Command                                 │
 │                                     ▼                                               │
 │                  ExchangeStateMachine  (single authoritative sequencer)              │
 │                   prepare(): pure validation → commit(): sequence + apply           │
 │                                     │                                               │
 │                  MatchingEngine: per-symbol OrderBook                                │
 │                   bids/asks: std::map<price, PriceLevel{ FIFO std::list }>           │
 │                   order index: unordered_map<id, {level it, list it}>                │
 └─────────────────────────────────────────────────────────────────────────────────────┘
```

There is no runtime connection to TradeGrid. Details are in
[docs/architecture.md](docs/architecture.md): why one sequencer owns the book, the complexity
of each operation, and why cancels can never hold a dangling iterator.

## Supported platforms and prerequisites

* macOS (Apple clang 17 tested) and Linux (GCC 13, Clang 18 tested in Ubuntu 24.04).
* CMake ≥ 3.24, Ninja, a C++20 compiler, Python 3 (for tools and smoke tests).
* Network access on first configure: GoogleTest 1.15.2 and nlohmann/json 3.11.3 are fetched
  with pinned URLs and SHA-256 hashes.

## Build, test, run

```bash
cmake --preset debug && cmake --build --preset debug
ctest --preset debug                         # 298 tests

cmake --preset release && cmake --build --preset release
./build/release/enginex simulate --input examples/demo_orders.jsonl --output demo-results.json
./build/release/enginex benchmark --seed 42 --orders 100000 --scenario mixed --out benchmarks/results.csv

scripts/check.sh all            # debug+release build/test, CLI smoke tests, format check
scripts/linux_check.sh          # Linux GCC / ASan+UBSan / TSan / release jobs in Docker
```

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

Not provided: real money, brokerage or market connectivity, accounts, risk, margin,
authentication, market/IOC/FOK/stop orders, amendments, self-trade prevention, regulatory
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

These are not order-entry or end-to-end latencies. Durable and network latencies are
measured separately in later releases.

## Testing strategy

* Unit tests cover every matching, cancellation, validation and idempotency rule in spec
  §11.1.
* Property tests run 100 seeds against a naive, independent reference book and check every
  invariant after every command.
* Fixtures: TradeGrid's published matching examples and fixtures, translated with attribution
  (`tests/testdata/tradegrid/`), plus EngineX-specific cases. `tools/compare_go_fixtures.py`
  runs a differential check against a local TradeGrid checkout.
* Golden CLI output, benchmark-workload self-validation, ASan/UBSan and TSan on Linux.

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
