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
    # Hostile alpha bytes (>= 0x80, NUL, control): a lenient replay and a dump must still
    # produce valid JSON, not crash on invalid UTF-8 (regression: review 4).
    import struct
    def hdr(t, locate, ns):
        return t + struct.pack(">HH", locate, 0) + ns.to_bytes(6, "big")
    bad_stock = b"\xffAB\x00\x01\x80  "
    msgs = [
        hdr(b"R", 1, 1) + bad_stock + b"\xfe" * 2 + struct.pack(">I", 100) + b"\x81" * 5 +
        b"\x82\x83" + struct.pack(">I", 1) + b"\x84",                     # 39 bytes
        hdr(b"H", 1, 2) + bad_stock + b"\x80\x00" + b"\xff\x00\x01\x02",  # trading state 0x80
        hdr(b"S", 0, 3) + b"\x9f",                                          # system event code
        hdr(b"A", 1, 4) + struct.pack(">Q", 1) + b"B" + struct.pack(">I", 10) + bad_stock +
        struct.pack(">I", 100),
    ]
    hostile = os.path.join(d, "hostile.itch")
    with open(hostile, "wb") as fh:
        for m in msgs:
            fh.write(struct.pack(">H", len(m)) + m)
    h = json.loads(run("itch", "replay", "--input", hostile, "--json"))
    assert h["frames"] == 4 and h["book_stats"]["live_orders"] == 1, h
    assert json.loads(run("itch", "replay", "--input", hostile, "--json", "--metrics-out",
                          os.path.join(d, "hm.json")))["frames"] == 4
    run("itch", "replay", "--input", hostile)  # text mode
    for line in run("itch", "dump", "--input", hostile).splitlines():
        json.loads(line)
    # CLI-level fuzz: mutated fixtures must always yield valid JSON (exit 0 or 1, never a
    # crash). The library fuzzer cannot see JSON rendering, which is where review 4 found a bug.
    import random
    rng = random.Random(20261110)
    base = [open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "testdata", "itch", n),
                 "rb").read() for n in ("lifecycle.itch", "malformed.itch")]
    mutant = os.path.join(d, "mutant.itch")
    for _ in range(150):
        data = bytearray(rng.choice(base))
        for _ in range(rng.randint(1, 12)):
            data[rng.randrange(len(data))] = rng.randrange(256)
        with open(mutant, "wb") as fh:
            fh.write(data)
        for args in (("replay", "--json"), ("dump",)):
            p = subprocess.run([exe, "itch", *args, "--input", mutant], capture_output=True,
                               text=True)
            if p.returncode not in (0, 1) or "fatal" in p.stderr:
                sys.exit(f"{args} crashed on a mutant (exit {p.returncode}): {p.stderr}")
            for line in p.stdout.splitlines():
                json.loads(line)
    # Independent Python decoder + book (tools/itch_pybook.py) must reproduce every level of
    # every final book of the generated file, and the best bid/ask of every book after every
    # 50th message.
    tools = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
    pb = subprocess.run([sys.executable, os.path.join(tools, "itch_pybook.py"), f,
                         "--checkpoint-every", "50", "--enginex", exe], capture_output=True, text=True)
    assert pb.returncode == 0 and "checkpoints" in pb.stdout, (pb.stdout, pb.stderr)
    # Same with the C++ sorted level vector capped at 2 levels, so the deep tier (D32) spills
    # and refills under the generator's churn and is checked mid-stream.
    pb = subprocess.run([sys.executable, os.path.join(tools, "itch_pybook.py"), f,
                         "--checkpoint-every", "50", "--level-vector-limit", "2",
                         "--enginex", exe], capture_output=True, text=True)
    assert pb.returncode == 0 and "checkpoints" in pb.stdout, (pb.stdout, pb.stderr)
    # MoldUDP64 over real UDP (loopback), two processes, with deliberate loss both ways.
    import socket
    import time

    def free_port():
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sk:
            sk.bind(("127.0.0.1", 0))
            return sk.getsockname()[1]

    rport, sport = free_port(), free_port()
    recv = subprocess.Popen([exe, "itch", "mold-recv", "--port", str(rport), "--request-port",
                             str(sport), "--idle-timeout-ms", "3000"],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(0.3)
    sent = json.loads(run("itch", "mold-send", "--input", f, "--port", str(sport), "--dest-port",
                          str(rport), "--drop-percent", "5", "--retransmit-drop-percent", "10",
                          "--linger-ms", "300", "--seed", "4"))
    out, err = recv.communicate(timeout=60)
    assert recv.returncode == 0, (recv.returncode, out, err)
    got = json.loads(out)
    assert got["final_state"] == "ended" and got["in_order_exactly_once"], got
    assert got["delivered"] == sent["messages"] and got["book_hash"] == r1["state_hash"], got
    assert sent["dropped_deliberately"] > 0 and sent["requests_answered"] > 0, sent
    # Usage errors (exit 2): no destination, non-loopback without --allow-remote, --allow-remote
    # without an explicit session, an over-long session, an out-of-range request interval.
    run("itch", "mold-send", "--input", f, expect=2)
    run("itch", "mold-send", "--input", f, "--dest-port", "9", "--dest-host", "10.0.0.1", expect=2)
    run("itch", "mold-send", "--input", f, "--dest-port", "9", "--allow-remote", expect=2)
    run("itch", "mold-send", "--input", f, "--dest-port", "9", "--session", "ELEVENCHARS", expect=2)
    run("itch", "mold-recv", "--port", str(rport), "--request-port", "9",
        "--request-interval-ms", "0", expect=2)
    run("itch", "mold-recv", "--port", str(rport), expect=2)  # recovery needs --request-port
    run("itch", "bogus", expect=2)
print("cli itch smoke: ok")
