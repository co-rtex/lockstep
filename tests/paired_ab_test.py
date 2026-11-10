#!/usr/bin/env python3
"""Tests for tools/paired_ab.py with fake bench executables that write a fixed CSV row, so the
verdict logic is checked deterministically (no timing involved)."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

TOOL = pathlib.Path(__file__).resolve().parent.parent / "tools" / "paired_ab.py"

FAKE = """#!{python}
import sys
args = sys.argv[1:]
out = args[args.index("--out") + 1]
with open(out, "w") as f:
    f.write("measurement,trial,ns_per_msg,state_hash_prefix\\n")
    f.write("m,first_pass,{value},{hash}\\n")
    f.write("m,1,{value},{hash}\\n")
    f.write("m,median,{value},{hash}\\n")
"""


def fake(directory, name, value, hash_="abc"):
    path = pathlib.Path(directory) / name
    path.write_text(FAKE.format(python=sys.executable, value=value, hash=hash_))
    path.chmod(0o755)
    return str(path)


def run(control, candidate, pairs, *extra):
    p = subprocess.run([sys.executable, str(TOOL), "--control", control, "--candidate", candidate,
                        "--pairs", str(pairs), *extra, "--"], capture_output=True, text=True)
    return p.returncode, (json.loads(p.stdout) if p.returncode == 0 else p.stderr + p.stdout)


def main():
    with tempfile.TemporaryDirectory() as d:
        slow = fake(d, "slow", 100.0)
        fast = fake(d, "fast", 90.0)
        same = fake(d, "same", 100.0)
        nearly = fake(d, "nearly", 99.0)  # 1 % faster: below the 2 % minimum effect
        wrong = fake(d, "wrong", 50.0, hash_="different")

        rc, s = run(slow, fast, 12)
        assert rc == 0 and s["verdict"] == "candidate faster", s
        assert s["candidate_better_in"] == 12 and s["sign_test_p"] < 0.001, s
        assert abs(s["median_ratio"] - 0.9) < 1e-9, s

        rc, s = run(fast, slow, 12)
        assert rc == 0 and s["verdict"] == "candidate slower", s

        rc, s = run(slow, same, 12)  # ties are dropped from the sign test
        assert rc == 0 and s["verdict"] == "no measurable difference", s
        assert s["candidate_better_in"] == 0 and s["candidate_worse_in"] == 0, s

        rc, s = run(slow, nearly, 12)  # consistent but tiny: not claimed
        assert rc == 0 and s["verdict"] == "no measurable difference", s
        assert s["sign_test_p"] < 0.001, s

        rc, s = run(slow, fast, 6)  # too few pairs for any verdict
        assert rc == 0 and s["verdict"].startswith("inconclusive"), s

        rc, msg = run(slow, wrong, 4)  # a faster wrong answer is refused
        assert rc != 0 and "differs between runs" in msg, msg

        rc, msg = run(slow, fast, 4, "--check", "no_such_column")
        assert rc != 0 and "no correctness column" in msg, msg
    print("paired_ab_test: verdicts, sign test, minimum effect, pair minimum and correctness "
          "refusal behave as documented")
    return 0


if __name__ == "__main__":
    sys.exit(main())
