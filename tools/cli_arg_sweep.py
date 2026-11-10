#!/usr/bin/env python3
"""CLI robustness sweep: hostile values (empty, non-numeric, negative, overflowing, huge,
non-ASCII, 5 000 bytes) for every value flag of every offline subcommand, plus the client
commands against a closed port. Fails if any run dies on a signal (including an OS kill
after unbounded allocation), aborts, prints an uncaught-exception banner, or exits with a
code outside 0-4. Runs that legitimately take long (billions of trials or generated messages)
are stopped after 20 s and listed, not failed. Every command runs inside a temporary
directory, because some flags take the hostile value as an output file name.

  python3 tools/cli_arg_sweep.py [--enginex build/release/enginex]   (about 2-3 minutes)
"""
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = str(pathlib.Path(__file__).resolve().parent.parent)
EXE = os.path.join(ROOT, "build/release/enginex")
if "--enginex" in sys.argv:
    EXE = os.path.abspath(sys.argv[sys.argv.index("--enginex") + 1])
TMP = tempfile.mkdtemp(prefix="enginex-cli-sweep-")
LIFE = os.path.join(ROOT, "tests/testdata/itch/lifecycle.itch")
DEMO = os.path.join(ROOT, "examples/demo_orders.jsonl")
J = os.path.join(TMP, "sweep.exj")
subprocess.run([EXE, "run", "--journal", J, "--input", DEMO, "--output", "/dev/null"], check=True)

VALUES = ["", "abc", "-1", "0", "1", "18446744073709551615", "18446744073709551616",
          "99999999999999999999999", "1e9", "0x10", " 5", "5 ", "4294967296", "65536", "2.5",
          "-0", "+3", "é", "AAPL,,MSFT", ",", "x" * 5000]

# (argv with required args, flags that take a value)
CMDS = [
    (["simulate", "--input", DEMO, "--output", "/dev/null"], ["--symbols", "--input", "--output"]),
    (["benchmark", "--scenario", "mixed", "--orders", "200", "--trials", "1", "--warmup", "0",
      "--out", os.path.join(TMP, "sweep_b.csv")],
     ["--scenario", "--seed", "--orders", "--trials", "--warmup", "--symbols", "--mode",
      "--clients", "--durability"]),
    (["run", "--journal", os.path.join(TMP, "sweep_run.exj"), "--input", "/dev/null",
      "--output", "/dev/null"], ["--durability", "--symbols", "--journal"]),
    (["replay", "--journal", J, "--verify"], ["--symbols", "--journal"]),
    (["itch", "generate", "--out", os.path.join(TMP, "sweep_g.itch"), "--messages", "100"],
     ["--seed", "--messages", "--symbols", "--anomalies-per-million"]),
    (["itch", "replay", "--input", LIFE], ["--depth", "--symbol", "--expected-orders",
                                           "--level-vector-limit", "--checkpoint-every",
                                           "--input"]),
    (["itch", "dump", "--input", LIFE], ["--limit", "--input"]),
    (["itch", "mold-sim", "--input", LIFE], ["--seed", "--loss-percent", "--duplicate-percent",
                                             "--reorder-window", "--mtu"]),
    (["client", "snapshot", "--port", "1"], ["--port", "--host", "--depth", "--symbol"]),
    (["client", "submit", "--port", "1", "--input", "/dev/null"],
     ["--port", "--host", "--timeout-ms"]),
]
OK_CODES = {0, 1, 2, 3, 4}
bad, timeouts, runs = [], [], 0
for base, flags in CMDS:
    for flag in flags:
        for v in VALUES:
            argv = list(base)
            if flag in argv:  # replace the base value
                argv[argv.index(flag) + 1] = v
            else:
                argv += [flag, v]
            if flag == "--checkpoint-every":
                argv += ["--checkpoint-out", os.path.join(TMP, "sweep_c.jsonl")]
            runs += 1
            try:
                p = subprocess.run([EXE] + argv, capture_output=True, text=True, timeout=20,
                                   errors="replace", cwd=TMP)
            except subprocess.TimeoutExpired:
                timeouts.append(" ".join(argv[:2] + [flag, repr(v)[:30]]))
                continue
            err = p.stderr
            if (p.returncode not in OK_CODES or "terminate" in err or "libc++abi" in err
                    or "Assertion" in err):
                bad.append((p.returncode, argv[:2], flag, repr(v)[:30], err.strip()[:160]))
print(f"{runs} runs, {len(bad)} suspicious, {len(timeouts)} timeouts")
for b in bad:
    print("BAD", b)
for t in timeouts:
    print("TIMEOUT (stopped, not a failure)", t)
shutil.rmtree(TMP, ignore_errors=True)
sys.exit(1 if bad else 0)
