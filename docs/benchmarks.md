# Benchmarks

All numbers on this page were produced by the commands shown, on the machine described,
from the commit recorded in each CSV row. Raw rows (every trial plus a median row) are in
[`benchmarks/results/`](../benchmarks/results). Nothing here is extrapolated.

The spec (§12.1) separates four kinds of measurement. They are never combined into one number:

| # | Measurement | What is timed | Where |
|---|---|---|---|
| 1 | **Core in-process** (`core_in_process`) | `ExchangeStateMachine::process`: validation, sequencing, idempotency map, matching | this page, R1 |
| 2 | **Durable accepted-command** | validation + journal write + `fsync`/`F_FULLFSYNC` + apply + response | R2 section |
| 3 | **Network round trip** | TCP client → framing → queue → sequencer (+journal) → ack | R3 section |
| 4 | **Recovery time** | open + validate + replay N journal records | R2 section |

## Machine and build (R1 run)

| Item | Value |
|---|---|
| Machine | MacBook Air, Apple M3 (4 performance + 4 efficiency cores), 8 GiB RAM, fanless |
| OS | macOS 26.6.2 (Darwin 25.6.0), on AC power, Low Power Mode off |
| Compiler | Apple clang 17.0.0 (clang-1700.4.4.1), `-O3 -DNDEBUG` (CMake Release), no `-march=native` |
| Commit | `862753a7cd74` (recorded in every CSV row) |
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

## Reproducing

```bash
cmake --preset release && cmake --build --preset release
./build/release/enginex benchmark --seed 42 --orders 100000 --scenario mixed --out benchmarks/results.csv
./build/release/enginex_bench --seed 42 --orders 100000 --trials 7 --out /tmp/all.csv
python3 tools/summarize_benchmarks.py /tmp/all.csv
```

CI runs a short seeded benchmark for every scenario on Linux and uploads the CSV as an
artifact. It has no performance thresholds, so noisy shared runners cannot fail the build.
