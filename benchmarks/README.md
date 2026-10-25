# benchmarks/

* `scenarios.{hpp,cpp}`: the six seeded workloads from spec §12.2. They are generated
  outside timing with a shadow state machine, so cancels always target open orders.
* `harness.{hpp,cpp}`: warm-up, trials, untimed throughput pass, per-command latency pass,
  determinism self-checks and CSV output.
* `bench_main.cpp`: `enginex_bench`, which runs every scenario with one set of parameters.
* `results/`: committed CSVs from recorded runs (one row per trial plus a `median` row).

Methodology, machine details and interpretation: [`docs/benchmarks.md`](../docs/benchmarks.md).

```bash
./build/release/enginex benchmark --scenario mixed --seed 42 --orders 100000 --out /tmp/r.csv
./build/release/enginex_bench --orders 100000 --trials 7 --out /tmp/all.csv
python3 tools/summarize_benchmarks.py /tmp/all.csv
```
