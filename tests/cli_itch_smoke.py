#!/usr/bin/env python3
"""CLI smoke test: enginex itch generate -> replay (twice, same hash) -> dump -> errors."""
import json
import os
import subprocess
import sys
import tempfile

exe = sys.argv[1]


def run(*args, expect=0):
    p = subprocess.run([exe, *args], capture_output=True, text=True)
    if p.returncode != expect:
        sys.exit(f"{args}: exit {p.returncode} (wanted {expect})\n{p.stdout}\n{p.stderr}")
    return p.stdout


with tempfile.TemporaryDirectory() as d:
    f = os.path.join(d, "s.itch")
    g = json.loads(run("itch", "generate", "--out", f, "--seed", "9", "--messages", "5000",
                       "--symbols", "2"))
    assert g["bytes"] == os.path.getsize(f), g
    r1 = json.loads(run("itch", "replay", "--input", f, "--json", "--strict"))
    r2 = json.loads(run("itch", "replay", "--input", f, "--json", "--depth", "1",
                        "--symbol", "SYM1", "--metrics-out", os.path.join(d, "m.json")))
    assert r1["state_hash"] == r2["state_hash"], "replay is not deterministic"
    assert r1["stopped"] is None and sum(r1["anomalies"].values()) == 0, r1
    assert [b["symbol"] for b in r2["books"]] == ["SYM1"] and len(r2["books"][0]["bids"]) <= 1
    m = json.load(open(os.path.join(d, "m.json")))
    assert m["timing"]["wall_seconds"] > 0
    r3 = json.loads(run("itch", "replay", "--input", f, "--json", "--expected-orders", "50000"))
    assert r3["state_hash"] == r1["state_hash"], "capacity hint changed the result"
    text = run("itch", "replay", "--input", f)
    assert "state hash " + r1["state_hash"] in text
    lines = run("itch", "dump", "--input", f, "--limit", "3").splitlines()
    assert len(lines) == 3 and json.loads(lines[0])["name"] == "system_event", lines
    # Errors: truncated file (exit 1 for replay and dump), bad usage (exit 2).
    t = os.path.join(d, "t.itch")
    with open(f, "rb") as src, open(t, "wb") as dst:
        dst.write(src.read()[:-3])
    bad = json.loads(run("itch", "replay", "--input", t, "--json", expect=1))
    assert bad["frame_error"] == "truncated_tail", bad
    assert "frame_error" in run("itch", "dump", "--input", t, expect=1)
    run("itch", "replay", "--input", f, "--symbol", "NOPE", expect=2)
    run("itch", "generate", "--out", f, "--anomalies-per-million", "2000000", expect=2)
    sim = json.loads(run("itch", "mold-sim", "--input", f, "--loss-percent", "5", "--seed", "3"))
    assert sim["final_state"] == "ended" and sim["book_matches_full_replay"], sim
    assert sim["packets_lost"] > 0 and sim["gaps_opened"] > 0, sim
    lossy = json.loads(run("itch", "mold-sim", "--input", f, "--loss-percent", "5",
                           "--no-recovery"))  # exit 0: stopped at the hole, not wrong
    assert lossy["book_matches_delivered_prefix"] and not lossy["book_matches_full_replay"], lossy
    run("itch", "bogus", expect=2)
print("cli itch smoke: ok")
