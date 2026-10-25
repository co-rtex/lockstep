#!/usr/bin/env python3
"""Print a Markdown table of the `median` rows of one or more EngineX benchmark CSV files.

Usage: tools/summarize_benchmarks.py benchmarks/results/*.csv
"""

import csv
import sys


def fmt_int(v: str) -> str:
    return f"{int(float(v)):,}"


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    rows = []
    for path in sys.argv[1:]:
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                if r["trial"] == "median":
                    rows.append(r)
    if not rows:
        print("no median rows found", file=sys.stderr)
        return 1
    measurement = rows[0]["measurement"]
    print(f"| scenario | measurement | commands | throughput (cmd/s, median) | p50 ns | p90 ns "
          f"| p99 ns | p99.9 ns | max ns | executions |")
    print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for r in rows:
        print(f"| {r['scenario']} | {r['measurement']} | {fmt_int(r['orders'])} | "
              f"{fmt_int(r['throughput_cmd_per_s'])} | {r['lat_p50_ns']} | {r['lat_p90_ns']} | "
              f"{r['lat_p99_ns']} | {r['lat_p999_ns']} | {r['lat_max_ns']} | "
              f"{fmt_int(r['executions'])} |")
    first = rows[0]
    print(f"\nBuild: {first['compiler']}, {first['build_type']}, {first['system']}, "
          f"git {first['git_sha']}; started {first['started_utc']}; measurement `{measurement}`.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
