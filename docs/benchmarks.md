# Benchmarks

All numbers on this page were produced by the commands shown, on the machine described,
from the commit recorded in each CSV row. Raw rows (every trial plus a median row) are in
[`benchmarks/results/`](../benchmarks/results). Nothing here is extrapolated.

**Caveat on the `git_sha` column.** Until the build-time fix (see below), the revision was
captured when CMake *configured* a build directory, not when it was built. A row's `git_sha`
is therefore the commit at that directory's last configure. CMake reconfigures whenever a
`CMakeLists.txt` changes, and A/B builds were usually configured fresh, but a reused
directory can name an earlier commit than the code it measured. Those rows were not
re-labelled. When the exact code matters, the prose names the commit and how the comparison
was built. The revision is now generated at build time (`cmake/git_info.cmake`), and a tree
with modified tracked files is labelled `<sha>-dirty`. This was found when a fresh benchmark
at `f365234` reported `776cdec`.

The spec (§12.1) separates four kinds of measurement for the exchange. They are never
combined into one number. The ITCH market-data measurements are a separate fifth family
([§ ITCH](#itch-50-decoding-and-reconstruction)):

| # | Measurement | What is timed | Where |
|---|---|---|---|
| 1 | **Core in-process** (`core_in_process`) | `ExchangeStateMachine::process`: validation, sequencing, idempotency map, matching | this page, R1 |
| 2 | **Durable accepted-command** (`durable_ack_<mode>`) | validation + apply + journal write + `fdatasync`/`F_FULLFSYNC`/`fsync` + response | R2 section |
| 3 | **Network round trip** (`network_rtt_<durability>_c<clients>`) | TCP client → framing → queue → sequencer (+group-commit journal) → ack | R3 section |
| 4 | **Recovery time** (`recovery_replay`) | open + validate + replay N journal records (warm cache) | R2 section |

## Machine and build (R1 run)

| Item | Value |
|---|---|
| Machine | MacBook Air, Apple M3 (4 performance + 4 efficiency cores), 8 GiB RAM, fanless |
| OS | macOS 26.6.2 (Darwin 25.6.0), on AC power, Low Power Mode off |
| Compiler | Apple clang 17.0.0 (clang-1700.4.4.1), `-O3 -DNDEBUG` (CMake Release), no `-march=native` |
| Commit | `862753a7cd74` (recorded in every CSV row). The matching core (`src/engine.cpp`, `state.cpp`, `order_book.cpp`, `canonical.cpp`, the workload generator) is unchanged from this commit to the final R3 commit, checked with `git diff --stat 862753a HEAD -- <those files>` |
| Command | `./build/release/enginex_bench --seed 42 --orders 100000 --trials 7 --out benchmarks/results/r1-core-macos-m3.csv` |
| 1M run | `./build/release/enginex benchmark --scenario mixed --seed 42 --orders 1000000 --trials 5 --out benchmarks/results/r1-core-macos-m3-1m.csv` |

Thread pinning and CPU-frequency control are not available on macOS, so the OS may schedule
the benchmark on an efficiency core or throttle the fanless chassis. Dispersion is reported
for that reason, and medians are not best runs.

## Procedure (what the harness does)

1. **Generate** the workload outside timing. A shadow `ExchangeStateMachine` runs during
   generation so cancels target orders that are actually open. The generator uses a portable
   xoshiro256** RNG, so a seed produces the same commands on every platform and standard
   library.
2. **Prefill** (untimed): the scenario's initial book population is applied to a fresh state.
3. **Warm-up**: one full untimed pass, discarded.
4. **Trials** (7 here). Each trial uses a fresh state and the identical commands:
   * *Throughput pass*: one `steady_clock` read before and after the whole measured loop.
     No per-command timers, logging or allocation for results (the response object is
     reused).
   * *Latency pass*: `steady_clock::now()` before and after every command; samples are stored
     in a pre-reserved vector and summarized afterwards (nearest-rank percentiles).
5. **Integrity checks** after timing: both passes and all trials must end with the same
   canonical state hash, and the accepted/execution counts must equal the generator's shadow
   run. Any divergence aborts the benchmark.
6. The **median across trials** is reported for each column, with throughput min/max.

### Timer caveat (important for reading the latency columns)

On Apple Silicon, `steady_clock` ticks at 24 MHz: **one tick = 41.7 ns**. Per-command latency
samples are therefore quantized (83 ns = 2 ticks, 125 ns = 3 ticks, …), and anything below
~42 ns reads as 0 or 41. A `now()` call costs about 15–19 ns (measured each run and recorded in
the CSV as `timer_call_ns`); the latency pass includes one timer call per sample. Treat
p50/p90 as ±1 tick, and prefer the throughput pass for average cost (1e9 / throughput = mean
ns per command without timer overhead). Linux `steady_clock` usually has much finer
resolution.

## Workloads (spec §12.2)

All prices are around a mid of 10 000 cents. Quantities are uniform in the stated ranges. The
"observed" columns come from the harness's own validation output for seed 42 with 100 000
measured commands, so the hit rates are measured, not assumed.

| Scenario | Prefill | Measured mix | Observed: % commands with fills / executions / max levels swept |
|---|---|---|---|
| `no-cross` | none | 100% passive orders 1–500 ticks from mid, qty 1–100, 2 symbols; book grows to 100k | 0% / 0 / 0 |
| `high-cross` | 500 passive per symbol within 10 ticks | 100% aggressive orders 1–10 ticks through mid, qty 1–100 | 50.6% / 99 356 / 7 |
| `same-price-burst` | 1 000 sells at one price | 60% sells and 25% buys at that one price, 15% cancels of random open orders | 40.2% / 68 885 / 1 |
| `multi-level-sweep` | 100 levels × 5 orders per side per symbol | 90% passive replenishment, 10% sweeps sized to consume 3–15 levels exactly | 9.8% / 99 744 / 15 |
| `cancel-heavy` | 2 000 passive per symbol | 50% passive inserts, 50% cancels of uniformly chosen open orders | 0% / 0 / 0 (49.8% cancels) |
| `mixed` | 500 passive per symbol | 62% limit orders mid−15…+10 (buy) / mid−10…+15 (sell), qty 1–200; 35% cancels; 3% exact retries | 19.2% / 28 010 / 5 |

## R1 results: core in-process (measurement 1)

Seed 42, 7 trials + 1 warm-up, medians across trials. The latency columns come from the
latency pass and carry the timer caveat above.

| scenario | commands | throughput (cmd/s, median) | min – max across trials | p50 ns | p90 ns | p99 ns | p99.9 ns | max ns |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| no-cross | 100,000 | 6,691,257 | 6.46M – 6.90M | 125 | 250 | 416 | 500 | 8,750 |
| high-cross | 100,000 | 8,122,405 | 7.62M – 8.18M | 84 | 208 | 334 | 458 | 3,334 |
| same-price-burst | 100,000 | 9,709,563 | 8.27M – 9.83M | 83 | 167 | 333 | 417 | 5,042 |
| multi-level-sweep | 100,000 | 5,842,728 | 5.54M – 5.98M | 125 | 292 | 917 | 1,584 | 5,291 |
| cancel-heavy | 100,000 | 8,141,195 | 6.64M – 8.55M | 125 | 167 | 333 | 417 | 5,417 |
| mixed | 100,000 | 8,193,531 | 7.64M – 8.34M | 125 | 208 | 333 | 417 | 5,250 |
| mixed | 1,000,000 | 4,560,561 | 4.23M – 4.73M | 166 | 458 | 666 | 834 | 8,625 |

Peak RSS was 53 MiB for the 100k runs and 305 MiB for the 1M run. The process holds the
workload, two trial states and the latency samples.

**Reproducibility re-run (2026-11-10).** The same command
(`enginex_bench --seed 42 --orders 100000 --trials 7`) was re-run with the matching core
unchanged. All six scenarios ended with the same state hashes as the recorded run, and every
median throughput was within +0.3 % to +3.6 % of the table above (for example mixed
8.49 M vs 8.19 M cmd/s). Raw rows:
[`r1-core-macos-m3-rerun-20261110.csv`](../benchmarks/results/r1-core-macos-m3-rerun-20261110.csv).
The table keeps the original run.

### Interpretation

* `multi-level-sweep` has the worst tail (p99 917 ns). Sweep commands touch up to 15 levels
  and ~75 resting orders, each one an execution record plus a list-node and index-node free.
  Matching cost scales with the number of orders and levels consumed, not with book size.
* `no-cross` is slower per command than `cancel-heavy` even though neither trades. Its book
  grows to 100k orders over up to 1 000 distinct prices, so the index and the per-order
  allocations stop fitting in cache.
* The 1M-command `mixed` run is about 44% slower than the 100k run. The idempotency map (one
  entry per accepted request, kept for the life of the process), the execution history and
  the order index all grow linearly and fall out of cache. This is an expected cost of
  retaining full replay and idempotency state in memory, not a matching-loop cost.
* Run-to-run variation: an earlier run at commit `3f37833` measured `high-cross` at a median of
  5.26M cmd/s with one trial at 0.83M. That supports reporting medians with min/max and keeping
  raw rows instead of quoting a single best number.

These are in-process core numbers only. They contain no I/O, JSON parsing, journaling or
networking, and they must not be presented as order-entry or end-to-end latency.

## R4 (optional) result: pooled book nodes in the matching core

Evidence first. `enginex_core_alloc_probe` (counts `operator new` around `process()`, same
seeded workloads) showed **2.0–3.5 heap allocations per command**. Each resting order cost
3.0–3.8: a `std::list` node, an order-index node and, sometimes, a new `std::map` level, all
freed again on fill or cancel. The book containers now use `std::pmr` types backed by one
`std::pmr::unsynchronized_pool_resource` per engine (DECISIONS.md D26). That leaves ≈1.0
allocation per command, the permanent idempotency-map entry.

Same machine, commit `c9b300aea8e2`, both builds from that commit. `std` is
`-DENGINEX_CORE_STD_ALLOC=ON`: identical container types on `new_delete_resource`. 100 000
commands × 7 trials, and 1 M × 5 for `mixed`, medians. Raw rows:
[`r4-core-pool-macos-m3.csv`](../benchmarks/results/r4-core-pool-macos-m3.csv).

| scenario | commands | std cmd/s | pool cmd/s | change | p99 ns | p99.9 ns | peak RSS MiB |
|---|---:|---:|---:|---:|---:|---:|---:|
| no-cross | 100 k | 6.97 M | 6.98 M | +0.2 % | 375 → 375 | 500 → 541 | 43 → 49 |
| high-cross | 100 k | 8.13 M | 9.31 M | +14.5 % | 375 → 333 | 500 → 417 | 49 → 55 |
| same-price-burst | 100 k | 9.80 M | 11.32 M | +15.4 % | 333 → 292 | 458 → 417 | 49 → 55 |
| multi-level-sweep | 100 k | 5.78 M | 7.02 M | +21.5 % | 1000 → 625 | 1750 → 1000 | 49 → 55 |
| cancel-heavy | 100 k | 8.12 M | 10.41 M | +28.3 % | 292 → 292 | 417 → 375 | 49 → 56 |
| mixed | 100 k | 8.18 M | 9.63 M | +17.8 % | 292 → 292 | 417 → 375 | 49 → 56 |
| mixed | 1 M | 4.61 M | 5.05 M | +9.6 % | 625 → 584 | 875 → 750 | 305 → 305 |

Every pair ends in the same state hash. The core suite (100-seed property tests against the
reference model, TradeGrid fixtures, journal recovery) passes on both builds. `no-cross`
only adds resting orders and never frees, so there is nothing for the pool to reuse, and it
does not change. The cost is memory: the pool keeps freed blocks for reuse and returns
nothing to the OS until the engine is destroyed (+5–6 MiB at 100 k commands). The R1 table
above still shows the original measurements. A spot check of the network round trip with the pooled core
(1 client, in-memory, 20 000 requests, 5 trials, `c9b300a`) gave 27 632 req/s with p50
33.4 µs, against the recorded 27 915 req/s and 33.1 µs. That is unchanged within noise, as
expected for a socket- and JSON-dominated path. Journal recovery replays through the same
engine. On the same commit, `--mode recovery --scenario mixed --orders 100000 --trials 5` went
from 3.93 M records/s (std build) to 4.27 M (pool build), +8.5 %, with the same state hash
(console values; not added to a CSV).

## R2 results: durable acknowledgment latency (measurement 2)

Same machine, Release build at commit `ceefa9f037e4`, scenario `mixed`, seed 42, medians of 5
trials after 1 warm-up. Raw rows: [`benchmarks/results/r2-durable-macos-m3.csv`](../benchmarks/results/r2-durable-macos-m3.csv).

Each sample times one `DurableExchange::process` call: validation, apply in memory, encode,
`write()` of the record, the synchronization for that mode, and building the response. Every
command waits for its own sync, with no group commit. There is no JSON and no networking. The
journal lives in a private temp directory on the internal SSD (APFS).

| `--durability` | flush used | commands | acks/s (median) | min – max | p50 | p90 | p99 | p99.9 | max |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `sync` | `fcntl(F_FULLFSYNC)` | 1,000 | 315 | 262 – 331 | 3.12 ms | 4.14 ms | 5.89 ms | 10.85 ms | 22.85 ms |
| `fsync` | `fsync()` (no drive-cache flush on macOS) | 20,000 | 39,979 | 31.4k – 49.5k | 20.0 µs | 26.3 µs | 51.6 µs | 816 µs | 7.91 ms |
| `none` | none (`write()` only) | 20,000 | 407,955 | 368k – 464k | 2.0 µs | 2.4 µs | 8.9 µs | 13.8 µs | 2.24 ms |

Commands:

```bash
./build/release/enginex benchmark --scenario mixed --mode durable --durability sync  --orders 1000  --trials 5 --out r2.csv
./build/release/enginex benchmark --scenario mixed --mode durable --durability fsync --orders 20000 --trials 5 --out r2.csv
./build/release/enginex benchmark --scenario mixed --mode durable --durability none  --orders 20000 --trials 5 --out r2.csv
```

*Reproduction after the sprint* (`94ee2d0`, which includes the R4 pool;
[`r2-durable-macos-m3-rerun-20261110.csv`](../benchmarks/results/r2-durable-macos-m3-rerun-20261110.csv)):

| mode | throughput | p50 ack |
|---|---:|---:|
| sync (`F_FULLFSYNC`) | 364 cmd/s (was 315) | 2.99 ms (was 3.12) |
| fsync | 49.4 k cmd/s (was 40.0 k) | 20.7 µs (was 20.0) |
| none | 461 k cmd/s (was 408 k) | 1.9 µs (was 2.0) |
| recovery replay, 100 k records | 4.26 M records/s (was 3.60 M) | |

The sync-mode ack is still dominated by the device flush (≈ 3 ms). The faster rows are
consistent with the cheaper matching core (R4), but this rerun alone does not isolate the
cause. Device flush times also vary between runs on this laptop.

Interpretation:

* **The flush dominates.** A real durable acknowledgment (`sync`, which flushes the drive
  cache with `F_FULLFSYNC`) costs about 3 ms here. That is roughly 25 000× the in-process
  matching cost (~125 ns p50 in R1). The matching engine is irrelevant to durable latency at
  one sync per command.
* `fsync` is ~150× faster than `sync` on macOS precisely because it does *not* flush the
  drive's write cache, so it is not power-loss safe on this platform. The two must not be
  conflated. EngineX defaults to `sync`.
* `none` measures the user-space cost plus one `write()` syscall (~2 µs). It survives a
  process crash, not an OS crash.
* Group commit (one sync per batch of commands) is how real systems amortize this. The R3
  server does it; its numbers are reported in the R3 section, not here.

## R2 results: recovery time (measurement 4)

Time for `DurableExchange::open` on an existing journal: read the file, validate the header
and every record's length/CRC/sequence, decode, and replay through the state machine, ending
in the same canonical state hash as the in-memory run. The journal is written just before the
trials, so it is read from the **OS page cache** (warm cache). Cold-start disk reads are not
measured.

| journal | records | median | records/s (median) | min – max records/s |
|---|---:|---:|---:|---:|
| `mixed`, 100k measured commands | 97,998 | 27.2 ms | 3,603,403 | 3.18M – 4.06M |
| `mixed`, 1M measured commands | 971,156 | 538.6 ms | 1,803,279 | 0.69M – 1.98M |

```bash
./build/release/enginex benchmark --scenario mixed --mode recovery --orders 100000  --trials 5 --out r2.csv
./build/release/enginex benchmark --scenario mixed --mode recovery --orders 1000000 --trials 5 --out r2.csv
```

The record count is lower than prefill + measured commands because rejected commands and
duplicate retries are not journaled. Per-record replay cost rises at 1M for the same reason
as the R1 1M run: the idempotency map, execution history and order index outgrow the caches.
One 1M trial took about 1.4 s (the 0.69M records/s minimum), so recovery time has run-to-run
outliers on this machine.

## R3 results: network round trip (measurement 3)

Same machine, Release build at commit `e8b10e2a82d7`, medians of 5 trials after 1 warm-up.
Raw rows: [`benchmarks/results/r3-network-macos-m3.csv`](../benchmarks/results/r3-network-macos-m3.csv).

**What is timed:** `enginex benchmark --mode network` starts an in-process `Server` on
`127.0.0.1` (ephemeral port) and N client threads. Each client keeps **one request
outstanding at a time** (closed loop). A sample runs from just before the client sends a
request frame to the moment its correlated reply has been received and parsed. It therefore
includes client JSON encoding and decoding, loopback TCP, the server reader thread, the
ingress queue, the sequencer (validation, matching, group commit = journal `write` + sync for
the batch), the reply queue and the writer thread. Clients send 70% limit orders around mid
(some crossing) and 30% cancels of their own open orders. Some of those cancels lose the race
to a fill and get `ORDER_NOT_OPEN` (about 10% of requests); errors are timed like acks.
Interleaving across connections is not deterministic, so trials end in different (valid)
states.

| durability | clients | requests | req/s (median) | min – max | p50 | p90 | p99 | p99.9 | mean batch |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| memory (no journal) | 1 | 20,000 | 27,915 | 27.7k – 27.9k | 33.1 µs | 42.3 µs | 53.8 µs | 62.7 µs | 1.00 |
| memory (no journal) | 8 | 20,000 | 85,749 | 84.4k – 86.3k | 87.1 µs | 122 µs | 159 µs | 204 µs | 2.22 |
| `none` (write, no sync) | 1 | 20,000 | 25,470 | 24.2k – 25.8k | 36.6 µs | 45.7 µs | 57.3 µs | 69.2 µs | 1.00 |
| `none` (write, no sync) | 8 | 20,000 | 84,153 | 83.8k – 84.7k | 89.4 µs | 122 µs | 162 µs | 227 µs | 2.45 |
| `sync` (`F_FULLFSYNC`) | 1 | 600 | 380 | 220 – 385 | 2.96 ms | 3.82 ms | 4.55 ms | 8.89 ms | 1.00 |
| `sync` (`F_FULLFSYNC`) | 8 | 4,000 | 1,417 | 1,385 – 1,426 | 5.98 ms | 7.04 ms | 8.23 ms | 13.6 ms | 4.00 |

```bash
./build/release/enginex benchmark --mode network --durability memory --clients 1 --orders 20000 --trials 5 --out r3.csv
./build/release/enginex benchmark --mode network --durability sync   --clients 8 --orders 4000  --trials 5 --out r3.csv
```

*Reproduction after the sprint* (`94ee2d0`, which includes the P0 server fixes;
[`r3-network-macos-m3-rerun-20261110.csv`](../benchmarks/results/r3-network-macos-m3-rerun-20261110.csv)):
memory, 1 client 27.5 k cmd/s, p50 33.5 µs, p99 54.0 µs; memory, 8 clients 85.8 k cmd/s,
p50 86.6 µs, p99 157 µs; sync, 8 clients 1 411 cmd/s, p50 5.99 ms. All are within 1.5 % of
the R3 medians above (p99 of the sync row −3 %).

Interpretation:

* **The three latencies are different things.** The in-process core needs ~125 ns per command
  (R1). A local TCP round trip without durability is ~33 µs p50, about 250× more, and that
  cost is socket I/O, thread hand-offs and JSON, not matching. A durable acknowledgment over
  TCP is ~3 ms p50 on this machine, dominated by `F_FULLFSYNC`. None of these may be quoted in
  place of another.
* **Group commit works.** With `sync` durability, 8 concurrent clients averaged 4.0 commands
  per journal sync. Throughput rose from 380 to 1,417 acknowledged requests/s (3.7×) at the
  cost of higher per-request latency (p50 2.96 → 5.98 ms). A request that arrives during a sync
  waits for that sync and then for its own batch's sync.
* With no sync cost, 8 clients reach ~85k req/s. The single sequencer is the serialization
  point by design (DECISIONS.md D1, D13). Per-request latency rises because requests queue
  behind each other on one core.
* Loopback only: no real network, NIC or kernel-bypass is involved, and nothing here supports
  a "low-latency" claim. Client and server share the same 8-core laptop.

## ITCH 5.0 decoding and reconstruction

These measure the separate market-data path ([itch.md](itch.md)), not the exchange, and are
not comparable with the R1–R3 rows: different input, different work.

Same machine as above (the hardware model could not be re-read in this session because
`sysctl` is blocked by the sandbox). Release build at commit `51ea715bf7d6`, Apple clang
17.0.0 (clang-1700.6.4.2), `-O3 -DNDEBUG`. 7 trials after 1 untimed warm-up ("first pass",
reported separately), medians. Raw rows:
[`benchmarks/results/itch-macos-m3.csv`](../benchmarks/results/itch-macos-m3.csv) and
console output [`itch-macos-m3.txt`](../benchmarks/results/itch-macos-m3.txt).

**Workload.** A synthetic ITCH stream from `generate()` with seed 42, 1 000 000 lifecycle
messages (1 000 014 in total with start/end-of-day messages), 8 symbols, 32.7 MB in the
length-prefixed file format. The mix is in `include/enginex/itch/synthetic.hpp`. The book
peaks at 88 792 live orders. The stream is generated and fully in memory before timing. Every
trial uses a fresh `BookBuilder`. All trials, and all three builds below, end with the same
state hash `3380cb95a962ddfa`.

**What is timed.**

| Measurement | Work inside the timer |
|---|---|
| `itch_decode` | `StreamFramer::feed` over the whole buffer + `decode()` of every payload (no book) |
| `itch_book_apply` | `BookBuilder::apply` over pre-decoded `Message`s (no framing or decoding) |
| `itch_replay` | `replay()`: framing + decoding + book, from the in-memory buffer |

The binary replaces global `operator new` with a counter, so the heap allocations inside each
timed region are exact. `itch_book_apply` also has a separate latency pass, with a
`steady_clock` read around every message. As in R1, these per-message samples are quantized
to 41.7 ns ticks and include one ~15 ns timer call, so p50/p90 mean "one or two ticks". Use the
throughput pass for average cost.

| build | measurement | msgs/s (median) | ns/msg | min – max (M/s) | allocs / msg | p99 | p99.9 | max |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `std::unordered_map` index | `itch_decode` | 68.5 M | 14.6 | 68.3 – 68.7 | 0 | | | |
| `std::unordered_map` index | `itch_book_apply` | 19.0 M | 52.5 | 18.8 – 19.1 | 0.482 | 125 ns | 167 ns | 288 µs |
| `std::unordered_map` index | `itch_replay` | 14.8 M | 67.4 | 14.3 – 15.3 | 0.482 | | | |
| open addressing | `itch_book_apply` | 31.1 M | 32.2 | 30.7 – 31.2 | 0.008 | 84 ns | 125 ns | 613 µs |
| open addressing | `itch_replay` | 22.8 M | 43.9 | 22.7 – 22.9 | 0.008 | | | |
| open addressing, `--reserve 262144` | `itch_book_apply` | 37.4 M | 26.8 | 36.7 – 37.7 | 0.008 | 83 ns | 125 ns | 1.6 µs |
| open addressing, `--reserve 262144` | `itch_replay` | 26.4 M | 37.8 | 26.4 – 26.5 | 0.008 | | | |

```bash
cmake -S . -B build/rel-stdidx -G Ninja -DCMAKE_BUILD_TYPE=Release -DENGINEX_ITCH_STD_INDEX=ON
cmake --build build/rel-stdidx && cmake --build --preset release
./build/rel-stdidx/enginex_itch_bench --seed 42 --messages 1000000 --symbols 8 --trials 7 --out itch.csv
./build/release/enginex_itch_bench    --seed 42 --messages 1000000 --symbols 8 --trials 7 --out itch.csv
./build/release/enginex_itch_bench    --seed 42 --messages 1000000 --symbols 8 --trials 7 --reserve 262144 --out itch.csv
```

**How the bottleneck was found.** No sampling profiler can attach in this environment:
`sample` reports the target as not running, and `pgrep` is denied. Two substitutes were used
instead: allocation counting and a per-message-type cost breakdown taken from the latency
pass (`mean ns by type` in the console output). With `std::unordered_map`, the order index
made 0.48 heap allocations per message (one node per add/replace, freed on every
delete/execute-to-zero). The breakdown (timer included, ≈15 ns floor) showed adds at ≈50 ns,
deletes ≈60 ns and replaces ≈99 ns, the message types that insert into or erase from the
index. After the index change these fell to 33 / 29 / 55 ns. The remaining 0.008
allocations per message are `std::map` nodes for new price levels.

**The tail.** Open addressing made the *maximum* worse (288 µs → 613 µs) while improving
every percentile through p99.9. The hypothesis was a single table doubling: rehash all live
orders into a newly allocated, never-touched array. Pre-sizing with `--reserve` (a
`BookBuilder(expected_orders)` hint) removed it: max 1.6 µs. It also improved mean cost to
26.8 ns, because growth and its page faults no longer happen during the run. Without a
hint, a production consumer would see that spike once per doubling. `std::unordered_map`
has the same issue: its 288 µs max is consistent with its own rehash, though that was not
isolated.

### Scaling with symbol count, and the price-level node arena

Commit `a7492a0784f6`, same machine and compiler. 2 000 000 lifecycle messages, seed 42,
`--reserve 262144`, 5 trials, medians. The live-order count is similar in every row (178–197 k
at the end), so the rows differ mainly in how many independent books share that work. Raw
rows: [`itch-scaling-macos-m3.csv`](../benchmarks/results/itch-scaling-macos-m3.csv)
([console](../benchmarks/results/itch-scaling-macos-m3.txt)). The `std_allocator` rows are
the `-DENGINEX_ITCH_STD_LEVEL_ALLOC=ON` build of the same commit.

| symbols | level nodes | book apply ns/msg | allocs/msg | apply p99.9 | replay ns/msg |
|---:|---|---:|---:|---:|---:|
| 8 | `std::allocator` | 29.2 | 0.0044 | 167 ns | 39.6 |
| 8 | node arena | 28.8 | 0.0000 | 125 ns | 39.7 |
| 100 | `std::allocator` | 35.1 | 0.0291 | 167 ns | 46.6 |
| 100 | node arena | 34.4 | 0.0001 | 167 ns | 45.9 |
| 1000 | `std::allocator` | 49.2 | 0.1277 | 209 ns | 66.5 |
| 1000 | node arena | 44.0 | 0.0005 | 209 ns | 58.0 |
| 5000 | `std::allocator` | 70.6 | 0.3043 | 334 ns | 90.7 |
| 5000 | node arena | 56.1 | 0.0025 | 292 ns | 74.6 |

Decoding stays at ≈14.6 ns/msg for every symbol count, as expected, since it does not touch a
book. Cost per message grows with the number of books because each message touches a colder
tree, level and order. With few symbols, `std::map` nodes for new price levels are rare.
With 5000 sparse books, levels appear and vanish constantly, and `std::allocator` makes 0.30
heap calls per message. A per-builder free-list arena for those nodes removes the calls and
cuts book-apply cost by 11 % at 1000 and 21 % at 5000 symbols, with no change at 8. The remaining growth (28.8 → 56.1 ns)
is cache-miss cost, which the allocator does not address. A flatter level structure would be
the next experiment, and would need measurements on real symbol distributions to justify it.

**Index sizing matters too.** With the same 2 M-message, 8-symbol workload, the capacity hint
gives 33.7 ns/msg with no hint (growth during the run), 28.4 with `--reserve 262144`
(index of 512 Ki slots, 8 MiB) and **46.7** with `--reserve 1048576` (2 Mi slots, 32 MiB).
The oversized table exceeds the caches, and Fibonacci hashing spreads consecutive order
references across it. A hint should be close to the expected peak live orders, not
"as large as possible". (Single runs of 5 trials each, `--only itch_book_apply --no-latency`,
from the console; not in the CSV.)

### Warm vs CPU-cache-cold passes

Commit `fd24aeceb3e4`, `--reserve 262144`, 7 trials. `--flush-cache-mib 64` streams a 64 MiB
buffer before every timed pass, so the workload and the book's memory start outside the CPU
caches. The page cache stays warm, since dropping it needs root on macOS. Raw rows:
[`itch-cache-macos-m3.csv`](../benchmarks/results/itch-cache-macos-m3.csv)
([console](../benchmarks/results/itch-cache-macos-m3.txt)).

| workload | flush | apply ns/msg (median) | apply max | replay ns/msg (median) | replay min–max M msg/s |
|---|---|---:|---:|---:|---:|
| 1 M msgs, 8 symbols | none | 27.1 | 2.2 µs | 38.2 | 26.1 – 26.2 |
| 1 M msgs, 8 symbols | 64 MiB | 27.3 | 2.6 µs | 37.9 | 25.7 – 26.5 |
| 2 M msgs, 5000 symbols | none | 56.7 | 3.3 µs | 74.9 | 13.2 – 13.4 |
| 2 M msgs, 5000 symbols | 64 MiB | 57.6 | 7.2 µs | 80.4 | 10.9 – 13.3 |

Whole-run medians are nearly insensitive to the starting cache state. Each trial builds its
book from empty and processes 1–2 M messages, so the cold start is amortized over the run.
The cold start does show up in the worst-case message (max) and in run-to-run spread. The
cache effect that matters for these workloads is the *working-set size*: the symbol-count
sweep above (29 → 71 ns/msg) and the oversized index (46.7 ns/msg).

**A large book that goes cold.** The case that does matter is a book already built when its
memory leaves the caches, for example after an idle period mid-session. `--cold-probe 2000`
builds the book from all but the last 2 000 messages (untimed, ≈ 89 k live orders). It then
times those 2 000 one by one, once directly (warm) and once after a 64 MiB flush (cold), 7
repetitions each, medians. Commit `f516b0516c93`; console:
[`itch-cold-probe-macos-m3.txt`](../benchmarks/results/itch-cold-probe-macos-m3.txt).
Samples include one timer call and are quantized to 41.7 ns, so use the means and the tails.

| symbols | warm mean | cold mean | warm p99 | cold p99 | warm p99.9 | cold p99.9 |
|---:|---:|---:|---:|---:|---:|---:|
| 8 | 25.9 ns | 67.0 ns | 83 ns | 250 ns | 84 ns | 459 ns |
| 5000 | 65.4 ns | 188.4 ns | 209 ns | 625 ns | 292 ns | 875 ns |

The first messages after the book goes cold cost about 2.6–2.9× more on average and 3× more
at p99. Each one misses on the index slot, the order node and the level node. This is the
measurable reason latency-sensitive systems keep hot data warm (for example by touching it
during idle time). Nothing like that is implemented here; this only quantifies the cost on
this machine.

### Real Nasdaq sample, and the layout changes it drove

Input: the first 256 MiB (compressed) of Nasdaq's public sample `01302019.NASDAQ_ITCH50.gz`,
decompressed to 676 MB. That is 21 255 695 messages, 03:03 to 09:37 ET, 8 713 symbols and
1.54 M live orders at the end; see [itch.md § Real Nasdaq sample](itch.md#real-nasdaq-sample-local-validation)
for provenance. The file is held in memory, and every trial replays it into a fresh book.
Commit `bbd4ff64a0aa`, three builds of that commit, 5 trials, medians, no capacity hint.
Raw rows (timings only, no market data):
[`itch-real-sample-macos-m3.csv`](../benchmarks/results/itch-real-sample-macos-m3.csv).

| build | price levels | order index | replay ns/msg | msgs/s | peak RSS |
|---|---|---|---:|---:|---:|
| `-DENGINEX_ITCH_MAP_LEVELS=ON -DENGINEX_ITCH_HASH_INDEX=ON` | `std::map` + arena | hash | 99.3 | 10.1 M | 1.0 GiB |
| `-DENGINEX_ITCH_HASH_INDEX=ON` | sorted vector | hash | 91.4 | 10.9 M | 1.2 GiB |
| default at `bbd4ff6` | sorted vector | paged direct | **62.1** | **16.1 M** | 1.2 GiB |
| default at `0ad62a6` | sorted vector + near-top scan | paged direct | **56.0** | **17.9 M** | (not recorded) |

A later row for commit `8a40818` (chunked node pool, see below) is in the same CSV.
The `0ad62a6` row adds one more data-driven change. A temporary histogram on two sample days
showed that ≈ 70 % of level lookups land within 4 entries of the best price and ≈ 80 % within
8. Lookups therefore scan those 8 linearly before binary searching. An interleaved A/B against
the previous commit measured 59.1 → 54.2 ns/msg (2019-03-27) and 63.3 → 57.9 (2020-01-30).
Replay on the three days: 56.0, 58.1 and 54.8 ns/msg at `0ad62a6`, and 58.0, 59.3 and 54.9 in
a reproduction at `562c56d` (5 trials each, same CSV).

**Memory.** `enginex itch replay --metrics-out` streams the file in 1 MiB chunks and now
records peak RSS. With `--depth 1`, so the JSON stays small, and `--strict`, the whole
process peaked at 296 / 327–332 / 282–302 MiB (2019-01-30 / 2020-01-30 / 2019-03-27, two
runs each). That is 1.54 / 1.65 / 1.58 M live orders at peak, or **≈ 188–211 bytes per live
order** for everything
([`itch-real-memory-macos-m3.csv`](../benchmarks/results/itch-real-memory-macos-m3.csv),
which also records 0 decode errors, 0 anomalies and the state hash of each day). RSS varies
by up to 7 % between identical runs. A rough breakdown: ≈ 48 B per order
node, up to ≈ 80 MB of index pages for the ≈ 20 M reference range, and ≈ 650 k live levels
at ≈ 48 B each (vector entry + `Level`), plus allocator and chunk slack (commit `37e41f2`
plus the RSS field).

*Index pages are mostly empty.* The new `peak_index_pages` statistic shows 4 899 / 5 896 /
4 992 pages (77–92 MiB) at only ≈ 7 % slot occupancy. A few old resting orders keep each
page alive. Smaller pages were tried (2¹⁰ and 2⁸ references per page; console runs, not in a
CSV). 2¹⁰ saved only ≈ 3 % of RSS (293 → 284 MiB on 2019-01-30) and was slightly slower
(57.4 → 59.9 ns/msg). Both smaller sizes also reached the page-count budget (16 384 pages)
and spilled references into the hash fallback, and at 2⁸ replay slowed to 82–85 ns/msg.
4 096-reference pages stay. A budget in bytes rather than in pages would be the precondition
for revisiting this.

*Rejected: software prefetch.* While applying message *i*, the index slot of message *i + 8*
was prefetched (book apply on pre-decoded real messages, 2019-03-27, 3 interleaved rounds
of 5 trials). The result was 42.6–43.6 ns/msg without and 40.4–43.3 ns/msg with: at most a
5 % gain, inconsistent between rounds. One earlier noisy round had suggested 12 %, which is
why rounds are interleaved and repeated. It was not adopted: an API that only hints is not
worth an unreliable few percent. The remaining misses are on order nodes and levels, which a
one-stage index prefetch cannot reach.

Decoding alone runs at 13.0 ns/msg (77 M msgs/s, 2.3 GiB/s) on this data. RSS includes the
676 MB file held in memory.

Book apply alone (pre-decoded messages, 3 trials; console values, not in the CSV) went from
75.4 to 48.1 ns/msg. Mean cost by type, timer included: adds 97 → 50 ns, deletes 79 → 59,
replaces 182 → 117, executions 109 → 79. The latency pass recorded maxima of 1.8–10 ms. The bench
then reported *where* the max occurred: at the same message (11 999 113) in repeated trials,
so it was structural, not noise. The cause was the node pool, a `std::vector<Node>` that
doubled past ≈ 1 M live orders and copied ≈ 50 MB in one message. A chunked pool (64 Ki
nodes per chunk, never moved) removed it. The worst sample is now 97–168 µs at a different
message in each trial (consistent with OS noise) and apply runs at 50.4 ns/msg (commit after
`55b199d`).

**Per-message latency on real data** (`dd78a8d`,
[`itch-real-latency-macos-m3.csv`](../benchmarks/results/itch-real-latency-macos-m3.csv); book
apply over pre-decoded messages; each sample includes one ≈ 15 ns `now()` call; medians of 5
trials):

| day | ns/msg | p50 | p90 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|---:|
| 2019-01-30 | 45.1 | 42 ns | 84 ns | 292 ns | 458 ns | 35 µs |
| 2019-03-27 | 43.3 | 41 ns | 84 ns | 333 ns | 541 ns | 36 µs |
| 2020-01-30 | 46.8 | 42 ns | 84 ns | 292 ns | 500 ns | 39 µs |

The maximum is more sensitive than the percentiles: single trials still show 0.2–2 ms samples
at varying messages (OS noise on a shared laptop; load average ≈ 4.5 during these runs).

*A second structural sample.* After the chunked pool, the largest sample on 2019-03-27 still
fell on the same message (index 219 328) in most trials: the day's first add order. Allocating
a 64 Ki-node chunk with `make_unique<Node[]>` value-initialized ≈ 3 MB inside that one add
(and in the one that opens each later chunk). Chunks are now raw storage, and each node is
constructed when handed out. Three interleaved rounds of 5 trials against a control binary
([`itch-node-chunk-ab-macos-m3.txt`](../benchmarks/results/itch-node-chunk-ab-macos-m3.txt) and
`.csv`) show:
* That sample was 91.8–102.4 µs before and is 30.2–38.5 µs after. The remainder is first-use
  allocation of the other structures. Using 4 Ki-node chunks did not change it.
* The control also showed 138–325 µs at message 20 386 914 in each process's first trial.
  This never appeared with the change.
* Mean, p99 and p99.9 are unchanged.

**How the data drove the changes.** The real per-type costs (adds ≈ 97 ns, deletes ≈ 79 ns,
replaces ≈ 182 ns, timer included) were 3–4× the synthetic ones. New level counters showed
43 % of adds and replaces opening a level, against 1.6 % in the synthetic stream, with books
thousands of levels deep. That motivated the sorted vector (−8 %). An experiment then showed
where the rest of the time went: a flat array indexed by order reference, valid only for
this file, replaced the hash index and cut replay to 59.9 ns/msg. With 1.5 M live orders the
hash table needs 64 MiB, and every lookup is a DRAM miss. Nasdaq references are dense and
mostly increasing, so the production *paged* direct index (bounded memory, hash fallback)
keeps nearly all of that gain.

The same three builds on the synthetic workloads (commit `bbd4ff6`, no hint; raw rows:
[`itch-structures-macos-m3.csv`](../benchmarks/results/itch-structures-macos-m3.csv)):

| workload | measurement | map + hash | vector + hash | vector + paged | max latency map+hash → paged |
|---|---|---:|---:|---:|---:|
| 1 M msgs, 8 symbols | book apply | 31.7 | 28.0 | **22.4** ns/msg | 609 µs → 30 µs |
| 1 M msgs, 8 symbols | replay | 43.8 | 40.1 | **35.0** ns/msg | |
| 2 M msgs, 5000 symbols | book apply | 60.4 | 53.8 | **44.8** ns/msg | 1.25 ms → 79 µs |
| 2 M msgs, 5000 symbols | replay | 78.2 | 77.0 | **59.6** ns/msg | |

The paged index has no rehash, so the one-off growth spike disappears without a capacity
hint. State hashes are identical across builds for each workload.

**Workload caveat.** The generator's book grows over the session (≈ 89 k live orders after
1 M messages, ≈ 178 k after 2 M). Compare per-message costs only at equal message counts.

**Not shown / not claimed.** File I/O and cold page cache (a cache cannot be dropped without
root on macOS; the "first pass" column in the CSV is only the first touch of freshly generated
data in a warm process), full-day real Nasdaq files, and any network delivery. Decoding
at ≈2.1 GiB/s on one core is an in-memory number and says nothing about a feed handler's
wire-to-book latency.

## Reproducing

```bash
cmake --preset release && cmake --build --preset release
./build/release/enginex benchmark --seed 42 --orders 100000 --scenario mixed --out benchmarks/results.csv
./build/release/enginex_bench --seed 42 --orders 100000 --trials 7 --out /tmp/all.csv
python3 tools/summarize_benchmarks.py /tmp/all.csv
```

CI runs a short seeded benchmark for every scenario on Linux and uploads the CSV as an
artifact. It has no performance thresholds, so noisy shared runners cannot fail the build.
