#!/usr/bin/env python3
"""Paired A/B comparison of two benchmark binaries on a noisy host (the method used for every
performance decision in docs/benchmarks.md since the load on the measurement laptop made
3-trial medians unreliable).

Each pair runs the control and the candidate back to back with identical arguments, one trial
each, alternating which goes first so slow drift cancels. The answer is the distribution of
per-pair ratios candidate/control, not the best run of either.

  python3 tools/paired_ab.py --control OLD_BIN --candidate NEW_BIN [--pairs 20]
      [--metric ns_per_msg] [--measurement NAME] [--check state_hash_prefix]
      [--min-effect 0.02] [--raw-out pairs.csv] -- BENCH_ARGS...

BENCH_ARGS go to both binaries; the tool appends `--trials 1 --out <tmp.csv>`. It works with
enginex_itch_bench (--only ... --no-latency; metric ns_per_msg), enginex_bench (metric
elapsed_ns) and enginex_mold_bench (--measurement NAME; --check delivered). Lower metric is
better.

The correctness column (`--check`, default state_hash_prefix; a missing column is an error)
must be identical in every run of both binaries, or the comparison is refused: a faster wrong
answer is not a result. Exit 0 with a JSON summary on stdout; exit 1 on a failed run or a
correctness mismatch.

Verdict (docs/benchmarks.md, "Acceptance gates"). A change is claimed only if all three hold:
  * at least 10 pairs;
  * the exact two-sided sign test on the pairs gives p < 0.05;
  * the median ratio differs from 1 by at least --min-effect (default 2 %).
Otherwise the verdict is "no measurable difference" (or "inconclusive" below 10 pairs).
An interquartile range that excludes 1.0 is not enough on its own: an A/A run (the same binary
on both sides, 4 pairs) gave IQR 1.0023-1.0054 on the exchange bench.
"""
import csv
import json
import math
import os
import statistics
import subprocess
import sys
import tempfile


def sign_test_p(better, worse):
    """Exact two-sided binomial sign test (ties dropped): the probability, if neither binary
    were faster, of a split at least this lopsided."""
    n = better + worse
    if n == 0:
        return 1.0
    k = min(better, worse)
    tail = sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n
    return min(1.0, 2 * tail)


def run_once(binary, args, measurement, metric, check):
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "row.csv")
        p = subprocess.run([binary] + args + ["--trials", "1", "--out", out],
                           capture_output=True, text=True)
        if p.returncode != 0 or not os.path.exists(out):
            sys.exit(f"{binary} failed (exit {p.returncode}): {p.stderr.strip()[-300:]}")
        rows = [r for r in csv.DictReader(open(out)) if r.get("trial") == "1"]
    if measurement:
        rows = [r for r in rows if r.get("measurement") == measurement]
    if len(rows) != 1:
        names = sorted({r.get("measurement") for r in rows})
        sys.exit(f"expected one trial-1 row, got {len(rows)} (measurements {names}); "
                 f"use --measurement or narrow the bench with --only")
    row = rows[0]
    if metric not in row:
        sys.exit(f"no column {metric!r}; columns: {sorted(row)}")
    if check not in row:
        sys.exit(f"no correctness column {check!r}; choose one with --check from {sorted(row)}")
    return float(row[metric]), row[check], row.get("git_sha", "")


def main():
    argv = sys.argv[1:]
    if "--" not in argv:
        sys.exit(__doc__)
    split = argv.index("--")
    opts, bench_args = argv[:split], argv[split + 1:]
    conf = {"--control": None, "--candidate": None, "--pairs": "20", "--metric": "ns_per_msg",
            "--measurement": "", "--check": None, "--raw-out": "", "--min-effect": "0.02"}
    i = 0
    while i < len(opts):
        if opts[i] not in conf or i + 1 >= len(opts):
            sys.exit(__doc__)
        conf[opts[i]] = opts[i + 1]
        i += 2
    if not conf["--control"] or not conf["--candidate"]:
        sys.exit(__doc__)
    pairs = int(conf["--pairs"])
    if pairs < 2:
        sys.exit("--pairs must be at least 2")
    check = conf["--check"] or "state_hash_prefix"  # a missing column is an error, never a pass
    rows, checks = [], set()
    for k in range(pairs):
        order = [("control", conf["--control"]), ("candidate", conf["--candidate"])]
        if k % 2 == 1:
            order.reverse()
        result = {}
        for label, binary in order:
            value, check_value, sha = run_once(binary, bench_args, conf["--measurement"],
                                               conf["--metric"], check)
            result[label] = value
            checks.add((check_value,))
            rows.append({"pair": k + 1, "binary": label, "git_sha": sha,
                         conf["--metric"]: value, "check": check_value})
        rows[-1]["ratio_candidate_over_control"] = result["candidate"] / result["control"]
    if len(checks) != 1:
        sys.exit(f"correctness column {check!r} differs between runs: {sorted(checks)}")
    if conf["--raw-out"]:
        with open(conf["--raw-out"], "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=["pair", "binary", "git_sha", conf["--metric"],
                                              "check", "ratio_candidate_over_control"])
            w.writeheader()
            w.writerows(rows)
    ratios = sorted(r["ratio_candidate_over_control"] for r in rows
                    if "ratio_candidate_over_control" in r)
    control = [r[conf["--metric"]] for r in rows if r["binary"] == "control"]
    candidate = [r[conf["--metric"]] for r in rows if r["binary"] == "candidate"]
    q1, q3 = ratios[len(ratios) // 4], ratios[(3 * len(ratios)) // 4]
    better = sum(1 for r in ratios if r < 1.0)
    worse = sum(1 for r in ratios if r > 1.0)
    p = sign_test_p(better, worse)
    median = statistics.median(ratios)
    min_effect = float(conf["--min-effect"])
    if pairs < 10:
        verdict = "inconclusive (fewer than 10 pairs)"
    elif p < 0.05 and median <= 1.0 - min_effect:
        verdict = "candidate faster"
    elif p < 0.05 and median >= 1.0 + min_effect:
        verdict = "candidate slower"
    else:
        verdict = "no measurable difference"
    summary = {
        "pairs": pairs, "metric": conf["--metric"], "check": check,
        "check_value": next(iter(checks))[0],
        "median_ratio": round(median, 4),
        "iqr": [round(q1, 4), round(q3, 4)],
        "candidate_better_in": better, "candidate_worse_in": worse,
        "sign_test_p": round(p, 4), "min_effect": min_effect,
        "control_median": statistics.median(control), "control_min": min(control),
        "candidate_median": statistics.median(candidate), "candidate_min": min(candidate),
        "verdict": verdict,
    }
    print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
