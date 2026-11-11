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

## Acceptance gates for performance changes

A performance change is accepted only through these gates. They were adopted after the
measurement laptop's load (average 4–9) made 3-trial medians range from 50 to 106 ns/msg
for one binary.

**1. One workload at a time.** Each family has its own bench, metric and correctness check,
and they are never merged into one latency claim:

| | workload | command (both binaries, `tools/paired_ab.py` appends `--trials 1 --out`) | metric | correctness check |
|---|---|---|---|---|
| a | ITCH parse only, real day | `enginex_itch_bench --input DAY --only itch_decode` | `ns_per_msg` | `state_hash_prefix` (field checksum) |
| b | ITCH book apply only, real day | `enginex_itch_bench --input DAY --only itch_book_apply --no-latency` | `ns_per_msg` | `state_hash_prefix` (book) |
| c | ITCH full replay, real day | `enginex_itch_bench --input DAY --only itch_replay` | `ns_per_msg` | `state_hash_prefix` (book) |
| d | synthetic matching core | `enginex_bench --scenario S --orders N` | `elapsed_ns` | `state_hash_prefix` (exchange state) |
| e | MoldUDP64 sequencer, in process | `enginex_mold_bench --messages N` (`--measurement NAME`) | `ns_per_msg` | `delivered` |
| f | UDP loopback with loss and recovery | `scripts/mold_udp_demo.sh` or `tools/mold_udp_sweep.py` | wall time, exactness | book hash equals the direct replay |

Synthetic generator workloads (d, e, and the synthetic ITCH files) are never reported as
results on a real feed. Real-day numbers name the day.

**2. Paired runs against an unchanged control binary.** The control is built from the parent
commit in a separate worktree. `tools/paired_ab.py --control OLD --candidate NEW --pairs 20
-- ARGS` runs the two back to back, 1 trial each, alternating which goes first. It refuses the
comparison if the correctness column differs in any run.

**3. A claim needs all three of:** at least 10 pairs, an exact two-sided sign test with
p < 0.05, and a median paired ratio at least 2 % from 1. Otherwise the result is reported as
"no measurable difference", with the median ratio, IQR and win count.

The gates were calibrated in both directions:

| run | median ratio | IQR | sign test p | verdict |
|---|---:|---:|---:|---|
| A/A, exchange core, 12 pairs | 0.990 | 0.975–1.004 | 0.15 | no measurable difference |
| A/A, ITCH book apply, 12 pairs | 1.001 | 0.995–1.003 | 1.0 | no measurable difference |
| Debug hardened build vs Release, 10 pairs | 15.9 | 15.81–15.92 | 0.002 | candidate slower (correct) |

The ratio-only rule used before these gates (IQR excluding 1.0) was not safe. An A/A run with
4 pairs gave IQR 1.0023–1.0054, which would have counted as a regression.

**4. Then the correctness gates of the changed path:**
* the Python differential models;
* `scripts/real_data_check.sh` on all three days;
* the mutation harness when its target files changed;
* Release, Debug, UBSan and hardened suites.

Negative results stay in the record, as they are for software prefetch and smaller index
pages below.

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

Peak RSS was 52–53 MiB for the 100k runs (45 MiB for `no-cross`) and 305 MiB for the 1M run. The process holds the
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
`std::pmr::unsynchronized_pool_resource` per engine (DECISIONS.md D26). That left ≈1.0
allocation per command, the permanent idempotency-map entry. D31 later replaced that map
with a record deque and an open-addressing index. At `cb768b3` the probe reports 0.03
allocations per command in all six scenarios (amortized deque blocks and index growth).

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

## Core: the request-id index (idempotency history), measured by history size

*Finding.* The core's per-command cost grew with the number of commands processed. On `mixed`
(seed 42, medians of 3) it was 108 ns at 100 k, 153 at 300 k, 198 at 1 M and 296 at 3 M,
while the book never held more than a few dozen orders. The cost was in the history kept for
idempotent retries. Every accepted command lived in a node-based
`unordered_map<string, CommandRecord>`. Checking a new `request_id` missed DRAM about twice
(bucket, then node). Rehashing was not the cause: the harness already calls `reserve()`
before timing, so the map never grew inside the measured region. (An extra reserve added as an
experiment, 262 → 256 ns at 3 M, was redundant for that reason; an audit pointed this out.) An
isolated probe (console) measured ≈ 75 ns per miss at 3 M entries against ≈ 23 ns for a flat
table.

*Change* (`676e392`, `6f655cd`; D31). Records live in a `std::deque` by sequence. A
`request_id` maps to its record through an open-addressing table of 8-byte slots holding a
32-bit fingerprint and the record index. The fingerprint is SipHash-2-4 with a random key per
state machine, so clients cannot flood one probe cluster. A full string compare confirms
every match. Results are unchanged: identical state hashes in every row below.

Interleaved A/B, control `564a427` vs `6f655cd`
([`core-request-index-ab-macos-m3.csv`](../benchmarks/results/core-request-index-ab-macos-m3.csv)):

| commands (`mixed`) | ns/cmd before → after | p99 before → after | p99.9 before → after | peak RSS |
|---:|---:|---:|---:|---:|
| 100 k | 108 → 94 | 292 → 208 ns | 375 → 250 ns | 34 → 33 MiB |
| 300 k | 153 → 105 | 458 → 250 ns | 584 → 333 ns | 96 → 83 MiB |
| 1 M | 198 → 119 | 584 → 250 ns | 750 → 333 ns | 305 → 261 MiB |
| 3 M | 296 → 196 | 834 → 292 ns | 3.3 → 1.1 µs | 903 → 767 MiB |

The other five R1 scenarios at 100 k commands gain 12–25 % throughput with lower p99
(same CSV). Journal recovery replays through the same state machine. Replaying 1 M records
went from 2.09 to 4.06 M records/s, so a restart takes 465 → 239 ms. At 100 k records it went
from 4.35 to 4.50 M/s (same CSV, `recovery_replay` rows). The cost still grows with history, as DRAM misses on the index and records
must. History is now capped at 2³² − 2 commands, which are rejected as `LIMIT_EXCEEDED` like
sequence exhaustion. The p99.9 and maximum are noisy on this shared laptop (load average
≈ 4 during these runs), and only the medians above are claimed.

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

These commands are as run at `51ea715`. The default build now uses the paged order index
and sorted-vector levels (the bench prints `order index: paged_direct; level nodes:
sorted_vector`). Reproducing the open-addressing rows today needs
`-DENGINEX_ITCH_HASH_INDEX=ON -DENGINEX_ITCH_MAP_LEVELS=ON` on the second build.

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
the `-DENGINEX_ITCH_STD_LEVEL_ALLOC=ON` build of the same commit (today that option takes
effect only together with `-DENGINEX_ITCH_MAP_LEVELS=ON`).

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

*Regression check of later changes.* D32, lazy node construction and the checkpoint hook
came after `562c56d`. The sprint's last `real_data_check.sh` run measured replay at
64.5–68.5 ns/msg, but under load ≈ 5. So `562c56d` was rebuilt and run against the current
code in 12 back-to-back pairs on 2019-03-27 (1 trial each, order alternating; one pair lost
a sample). The median ratio was 1.019 (IQR 0.94–1.05), and the current code was slower in 6 of
11 pairs. In the three quietest pairs it was 2.4–4.8 % slower, and the best runs were 58.5 vs
56.1 ns/msg. A regression of up to ≈ 4 % therefore cannot be excluded on this machine. It has
not been attributed to a specific change. The state hash is identical. Raw pairs:
[`itch-replay-regression-ab-macos-m3.txt`](../benchmarks/results/itch-replay-regression-ab-macos-m3.txt).

Re-checked later with the acceptance gate (`tools/paired_ab.py`, 20 pairs each, `562c56d` as
the control, 2019-03-27, load average ≈ 6–7, identical state hash in every run):

| workload | median ratio (current / `562c56d`) | IQR | current slower in | sign test p | verdict |
|---|---:|---:|---:|---:|---|
| full replay (`itch_replay`) | 1.036 | 0.986–1.108 | 13 of 20 | 0.26 | no measurable difference |
| book apply (`itch_book_apply`) | 0.970 | 0.748–1.200 | 10 of 20 | 1.0 | no measurable difference |

No regression is demonstrated. This host, at this load, cannot resolve one of a few percent:
the apply medians (71–82 ns/msg) are far above the ≈ 43 ns/msg measured on a quieter machine
earlier, and the spread is ±25 %. A quiet machine and the same command would settle it. Raw
pairs: [`itch-replay-regression-gate-macos-m3.csv`](../benchmarks/results/itch-replay-regression-gate-macos-m3.csv),
[`itch-apply-regression-gate-macos-m3.csv`](../benchmarks/results/itch-apply-regression-gate-macos-m3.csv).

*Current numbers (`b50a73f`, the code after every sprint change including D35, quiet host).*
Measured with `enginex_itch_bench --input DAY --trials 5` on 2026-11-11 at 02:34–02:35 EST,
load average 1.9–2.1. The numbers are ns/msg, as median (best–worst) of the 5 trials; latency
is from the per-message timing pass, timer included:

| day | decode | book apply | replay | apply p50 / p99 / p99.9 |
|---|---:|---:|---:|---:|
| 2019-01-30 | 12.7 (12.6–12.7) | 45.0 (44.9–45.2) | 56.7 (55.5–58.5) | 42 / 291 / 417 |
| 2020-01-30 | 12.6 (12.6–12.7) | 44.8 (44.6–46.2) | 57.2 (56.7–57.4) | 42 / 292 / 459 |
| 2019-03-27 | 12.5 (12.5–12.5) | 41.7 (41.3–42.6) | 55.3 (54.5–56.8) | 41 / 291 / 417 |

The state hashes are identical across trials and match the reference hashes. Unpaired medians
like these still move by a few percent between sessions; for example, 2019-01-30 apply gave
44.8 an hour earlier, before D35. So changes are judged only by the paired gates below. Raw
rows:
[`itch-real-current-quiet-macos-m3.csv`](../benchmarks/results/itch-real-current-quiet-macos-m3.csv).
An earlier run of the same command at `8253653`, at load 3.3–7.4, varied by up to 50 % between
trials: [`itch-real-current-macos-m3.csv`](../benchmarks/results/itch-real-current-macos-m3.csv).

**It was settled in a quieter window, and the regression is real.** At load ≈ 3, the same
20-pair apply gate gave:
* a median ratio of **1.0695** (IQR 1.04–1.13);
* the current code slower in 18 of 20 pairs, sign test p = 0.0004;
* 44.0 → 47.4 ns/msg, with an identical state hash.

That passes the gate: **book apply was about 7 % slower on real data than at `562c56d`**.
A quieter host later put it at ≈ 5 %, attributed it to D32 and D34, and D35 removed D34's
share (below).

Bisecting with the same gate (load ≈ 3–5):

| step | pairs | median ratio | IQR | slower in | p |
|---|---:|---:|---:|---:|---:|
| lazy node construction (`dd78a8d`) | 12 | 0.983 | 0.87–1.17 | 4 of 12 | 0.39 |
| D32 bounded level vector, and commits between (`b7f750e`) | 20 | 1.031 | 1.00–1.08 | 14 of 20 | 0.12 |
| everything after D32, up to `a505d51` | 20 | 1.033 | 0.99–1.08 | 12 of 20 | 0.50 |
| of which D34 keyed fallback index alone (`eb2442f` → `9c8f48a`, load ≈ 3) | 20 | 1.015 | 0.98–1.05 | 13 of 20 | 0.26 |

The cost splits roughly evenly between D32 and the later commits. Neither half is
significant on its own. The two halves multiply to ≈ 1.065, consistent with the end-to-end
1.0695.

One hypothesis was tested and rejected. D32 put a `std::map` header inline in every book
side, growing a `Book` from ≈ 80 to 128 bytes, although the map is empty on real data. Moving
it behind a `unique_ptr` (allocated on the first spill) gave a median ratio of 0.998 against
HEAD (20 pairs, p = 1.0), so it was reverted.

The D32 share is the price of the worst-case bound: the hostile pattern costs 96 ns/msg instead
of 25 µs/msg, and the vector's own worst case is 1.6 µs/msg (churn). That price is accepted
deliberately. At this load the other ≈ 3 % could not be attributed. The quiet-host bisect
below pins it on D34.

Raw pairs:
[`itch-apply-regression-quiet-macos-m3.csv`](../benchmarks/results/itch-apply-regression-quiet-macos-m3.csv),
[`itch-apply-bisect-d32-macos-m3.csv`](../benchmarks/results/itch-apply-bisect-d32-macos-m3.csv),
[`itch-apply-bisect-after-d32-macos-m3.csv`](../benchmarks/results/itch-apply-bisect-after-d32-macos-m3.csv),
[`itch-apply-deep-map-pointer-macos-m3.csv`](../benchmarks/results/itch-apply-deep-map-pointer-macos-m3.csv), [`itch-apply-bisect-d34-macos-m3.csv`](../benchmarks/results/itch-apply-bisect-d34-macos-m3.csv).

**Settled on a quiet host (2026-11-11, load ≈ 1.7–2.5), then mostly removed (D35).** Every
binary was built the same way, from a `git archive` export under `$TMPDIR` with the already
fetched dependencies. Such binaries report `git_sha` as `unknown`, so each CSV row names its
commit in the `step` or `workload` column. Each gate was 20 pairs on 2019-03-27, and every
run had the same state hash.
* Current code (`12c661a`) against `562c56d`: apply **1.053** (IQR 1.050–1.057, 19/20 slower),
  replay **1.047** (1.039–1.057, 20/20). The ≈ 7 % above was measured at higher load; ≈ 5 % is
  the better estimate.
* Bisect: lazy nodes 1.001 (no difference); **D32 1.021** (p = 0.003); the bounds commits
  0.997 (no difference); **D34 1.028, and 1.027 on a repeat** (p = 0.04, 0.012); memory
  accounting and later 1.006 (no difference). The steps multiply to ≈ 1.05.
* D34's share was unexpected, because its SipHash never runs on real data. It had been
  inlined into the paged index's hot paths. **D35** moves the three fallback calls out of
  line. Against `12c661a`: apply 0.969 and 0.964 (repeat) on 2019-03-27 and 0.970 on
  2020-01-30, and replay 0.969, each faster in 17–20 of 20. The hash-flood attack input, which
  does use the fallback, is 4.5 % slower (69.6 → 72.7 ns/msg, still O(1); accepted, D35).
* **Net, `b50a73f` against `562c56d`: apply 1.012** (IQR 0.991–1.026, 14/20, p = 0.12). That
  is no measurable difference. What remains is D32's deliberate ≈ 2 %, at the edge of what
  the gate resolves.
* Moving D32's deep-tier code out of line in the same way changed nothing (1.006 and 0.998,
  p ≥ 0.5). It was reverted; patch and pairs are kept (D35, *tried and rejected*).

Raw pairs:
[`itch-apply-gate-quiet2-macos-m3.csv`](../benchmarks/results/itch-apply-gate-quiet2-macos-m3.csv),
[`itch-replay-gate-quiet2-macos-m3.csv`](../benchmarks/results/itch-replay-gate-quiet2-macos-m3.csv),
[`itch-apply-bisect-quiet-macos-m3.csv`](../benchmarks/results/itch-apply-bisect-quiet-macos-m3.csv),
[`itch-cold-fallback-d35-macos-m3.csv`](../benchmarks/results/itch-cold-fallback-d35-macos-m3.csv).

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

*Measured breakdown.* `BookBuilder::memory_usage()` reports the bytes each structure has
allocated. It is computed from capacities, so it is deterministic and is not an RSS
reading. `itch replay --json` prints it as `book_memory_bytes`. At the end of each real day,
per live order:

| structure | 2019-01-30 | 2020-01-30 | 2019-03-27 |
|---|---:|---:|---:|
| index pages (16 KiB each, ≈ 7 % occupied) | 52.2 B | 58.6 B | 51.8 B |
| order nodes (40 B each, 64 Ki-node chunks) | 40.9 B | 41.3 B | 41.5 B |
| `Level` objects (24 B, pooled) | 10.2 B | 10.4 B | 10.2 B |
| sorted level vectors (16 B entries, capacity) | 10.3 B | 10.5 B | 10.3 B |
| per-locate tables, books, everything else | 1.5 B | 1.4 B | 1.5 B |
| **structures total** | **115.1 B** | **122.3 B** | **115.3 B** |
| share of peak RSS | 57 % | 58 % | 61 % |

The deep-map tier and the hash fallback hold nothing on real data. The other ≈ 40 % of RSS
is allocator overhead, memory freed but retained by the allocator, transient copies during
vector growth, and the process itself; this accounting cannot attribute it. Index pages are
the largest single item, and their occupancy is the lever for memory. They are kept for
speed: direct page indexing avoids a hash. Raw rows:
[`itch-real-memory-breakdown-macos-m3.csv`](../benchmarks/results/itch-real-memory-breakdown-macos-m3.csv).

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
nodes per chunk, never moved) removed it. The worst sample was then 97–168 µs and apply ran at
50.4 ns/msg (commit after `55b199d`). That remaining sample was later traced to chunk
value-initialization on the day's first add (below).

**Per-message latency on real data** (`dd78a8d`,
[`itch-real-latency-macos-m3.csv`](../benchmarks/results/itch-real-latency-macos-m3.csv); book
apply over pre-decoded messages; each sample includes one ≈ 15 ns `now()` call; medians of 5
trials):

| day | ns/msg | p50 | p90 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|---:|
| 2019-01-30 | 45.1 | 42 ns | 84 ns | 292 ns | 458 ns | 35 µs |
| 2019-03-27 | 43.3 | 41 ns | 84 ns | 333 ns | 541 ns | 36 µs |
| 2020-01-30 | 46.8 | 42 ns | 125 ns | 292 ns | 500 ns | 39 µs |

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
* In two of the three recorded rounds, the control's first trial peaked at message
  20 386 914 (139 and 185 µs). This never appeared with the change. (Earlier console rounds
  showed up to 324 µs there; they are not in the file.)
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

**Adversarial level pattern (D32).** `tools/itch_adversarial.py 200000 worse|better OUT.itch` builds
one 200 k-level side. With `worse`, every add opens a level worse than all others (the sorted
vector's front); with `better`, the back. Book apply, 3 trials, medians
([`itch-level-bound-macos-m3.csv`](../benchmarks/results/itch-level-bound-macos-m3.csv); the
`level_alloc` column tells the layouts apart):

| layout | `worse` ns/msg | `better` ns/msg |
|---|---:|---:|
| sorted vector, unbounded (before D32) | 25 130 | 17.5 |
| `std::map` levels (`-DENGINEX_ITCH_MAP_LEVELS=ON`) | 141 | 121 |
| sorted vector bounded at 8 192 + deep map (`b7f750e`) | 96 | 54 |

*The bound's real worst case.* The `worse` pattern now sends each new level to the map, so
it no longer exercises the vector. The complexity audit pointed at the pattern that does:
`itch_adversarial.py N churn` keeps 8 191 levels with the map empty, then alternates adding
and deleting a level just below the worst one. Each message then moves the whole vector.
200 000 such pairs (408 192 messages) averaged **1 583 ns/msg** (3 trials, `--no-latency`).
The cost is linear in the limit and still bounded, about 16× the `worse` case. A smaller
limit would lower it, but real books reach 3 831 levels, so 8 192 was kept.

Real data stays in the vector (deepest side 3 831 levels). In interleaved runs on 2019-03-27
under load (≈ 4), the best-trial throughput was 23.25–23.94 M msg/s before and 21.75–23.56
after, so there is no measurable change (console). The control binary was built from the
pre-D32 code during the node-chunk experiment, so its label reads `6d1546a…-dirty`.

**Rejected: software prefetch in book apply.** The flat-array experiment above suggested that
index and node lookups miss the cache, so the book got two hints. `prefetch_index` touched
the index slot of a message 2D ahead, and `prefetch_order` touched the order node of the
message D ahead (by then its slot is cached). The bench applied them in its book-apply loop.
State hashes were unchanged. Under load ≈ 5, single 3-trial runs ranged from 50 to 106
ns/msg in *both* arms, so they could not resolve the effect. An opt-in
`QOS_CLASS_USER_INTERACTIVE` request (performance cores; macOS has no pinning) did not narrow
that spread either. Twenty back-to-back pairs on 2019-03-27 (1 trial each, D = 16 vs off)
gave a median ratio of 0.990 (IQR 0.85–1.08). D = 16 was faster in 10 of 20 pairs, and the
best runs were 49.4 vs 50.0 ns/msg. So there is no measurable gain, and the code was not kept.
A plausible reason, not tested: Nasdaq references mostly increase, so index pages and recently
allocated nodes are already close together and cached. Raw pairs:
[`itch-prefetch-ab-macos-m3.txt`](../benchmarks/results/itch-prefetch-ab-macos-m3.txt). The
experiment's code is kept as a patch against commit `1cb8455`:
[`itch-prefetch-experiment.patch`](../benchmarks/results/itch-prefetch-experiment.patch).

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
