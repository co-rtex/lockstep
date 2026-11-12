#!/usr/bin/env python3
"""Runs two builds of tools/variant/itch_state_hash and requires identical per-workload output
(the first line, which names the layout, must differ)."""
import subprocess
import sys

a, b = (subprocess.run([exe], capture_output=True, text=True, check=True).stdout.splitlines()
        for exe in sys.argv[1:3])
if a[0] == b[0]:
    # The main library was itself configured with the baseline layout: nothing to compare.
    print(f"skipped: both executables use {a[0]!r}")
    sys.exit(77)  # reported by ctest as skipped (SKIP_RETURN_CODE), not as a pass
if a[1:] != b[1:]:
    sys.exit("layouts disagree:\n" + "\n".join(f"  {x}\n  {y}" for x, y in zip(a[1:], b[1:])))
print(f"{a[0]} == {b[0]} on {len(a) - 1} workloads")
