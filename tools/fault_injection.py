#!/usr/bin/env python3
"""End-to-end crash and corruption checks for the EngineX journal (spec §9.4, §11.4).

Scenario A, kill -9 rounds. `enginex run --durability sync` is started on one journal and fed
commands one at a time over stdin. In each round the process is killed with SIGKILL at a
seeded random point, sometimes right after a command was written to its stdin without
waiting for the reply (an in-flight request). After every kill:

  * every command whose `accepted` reply was observed must be in the journal
    (no acknowledged-but-unrecorded command);
  * the restarted process retries every request id sent so far with the same body:
    acknowledged ones must come back `duplicate` with the original sequence and executions;
    the in-flight one is either a duplicate (it was committed) or newly accepted (it was not),
    never applied twice.

After all rounds, the final state hash must equal `enginex simulate` over the same command
list. Simulate is in-memory and never crashes, so this proves the crashes and retries changed
nothing.

Scenario B, corruption. A copy of the journal gets one flipped payload byte: `replay
--verify` must exit 1 (corrupt) and `run` must refuse to start. A truncated copy must give
exit 3 (incomplete tail) from verify, and `--repair-tail` must restore the last complete
record and preserve the cut bytes.

Scenario C, network kill -9. `enginex serve --journal --durability sync` gets traffic from 3
concurrent TCP clients and a subscriber, and is killed with SIGKILL mid-stream. After a
restart on the same journal: every acknowledged command is present; each client retries
everything it sent (acknowledged requests return `duplicate` with the original sequence);
the restarted server's final state equals an in-memory `simulate` of the commands ordered by
their assigned command_sequence; and a fresh subscription's watermark equals the last
sequence.

Usage: tools/fault_injection.py --enginex build/release/enginex [--rounds 8] [--seed 1]
"""

from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import signal
import socket
import struct
import subprocess
import threading
import time
import sys
import tempfile
from pathlib import Path


def commands(seed: int, count: int) -> list[dict]:
    # Cancel targets never exceed the number of earlier new orders, so the target sequence
    # already exists when the cancel is first processed. A rejected cancel therefore stays
    # rejected on every retry (closed orders stay closed, cancel sequences are never orders),
    # which keeps retries comparable with a single in-memory pass.
    rng = random.Random(seed)
    out = []
    new_orders = 0
    for i in range(count):
        if new_orders > 5 and rng.random() < 0.2:
            out.append({"type": "cancel", "request_id": f"k{i}",
                        "order_id": f"ord-{rng.randint(1, new_orders)}"})
        else:
            new_orders += 1
            side = rng.choice(["BUY", "SELL"])
            price = 10000 + (rng.randint(-8, 4) if side == "BUY" else rng.randint(-4, 8))
            out.append({"type": "new_limit", "request_id": f"k{i}",
                        "symbol": rng.choice(["AAPL", "MSFT"]), "side": side,
                        "price": price, "quantity": rng.randint(1, 50)})
    return out


class Session:
    """A running `enginex run` process with line-oriented request/response."""

    def __init__(self, enginex: str, journal: Path):
        self.proc = subprocess.Popen(
            [enginex, "run", "--journal", str(journal), "--durability", "sync"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, bufsize=1)
        self.recovery = json.loads(self.proc.stdout.readline())

    def send(self, cmd: dict) -> None:
        self.proc.stdin.write(json.dumps(cmd) + "\n")
        self.proc.stdin.flush()

    def reply(self) -> dict:
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("enginex run exited unexpectedly")
        return json.loads(line)

    def request(self, cmd: dict) -> dict:
        self.send(cmd)
        return self.reply()

    def kill(self) -> None:
        self.proc.send_signal(signal.SIGKILL)
        self.proc.wait()

    def close(self) -> dict:
        self.proc.stdin.close()
        last = None
        for line in self.proc.stdout:
            last = json.loads(line)
        self.proc.wait()
        return last


def check(cond: bool, message: str) -> None:
    if not cond:
        raise AssertionError(message)


def scenario_kill(enginex: str, workdir: Path, rounds: int, seed: int) -> Path:
    rng = random.Random(seed)
    journal = workdir / "kill.exj"
    cmds = commands(seed, rounds * 40)
    acked: dict[str, dict] = {}  # request_id -> accepted reply observed by the client
    sent = 0
    for rnd in range(rounds):
        s = Session(enginex, journal)
        check(s.recovery["type"] == "recovery", "missing recovery line")
        # Retry everything sent so far: committed work must never be re-applied.
        for cmd in cmds[:sent]:
            r = s.request(cmd)
            rid = cmd["request_id"]
            if rid in acked:
                check(r["status"] == "accepted" and r["duplicate"],
                      f"round {rnd}: acknowledged {rid} not returned as duplicate: {r}")
                check(r["command_sequence"] == acked[rid]["command_sequence"],
                      f"round {rnd}: {rid} sequence changed")
                check(r["executions"] == acked[rid]["executions"],
                      f"round {rnd}: {rid} executions changed")
            elif r["status"] == "accepted":
                acked[rid] = r
        # New work, then a kill at a random point.
        burst = rng.randint(5, 35)
        in_flight = rng.random() < 0.5
        for _ in range(burst):
            if sent >= len(cmds):
                break
            cmd = cmds[sent]
            sent += 1
            if in_flight and _ == burst - 1:
                s.send(cmd)  # do not wait for the reply
                break
            r = s.request(cmd)
            if r["status"] == "accepted":
                acked[cmd["request_id"]] = r
        s.kill()
        # Every acknowledged command must be durable.
        verify = subprocess.run([enginex, "replay", "--journal", str(journal), "--verify"],
                                capture_output=True, text=True)
        check(verify.returncode == 0, f"round {rnd}: replay failed: {verify.stdout}")
        report = json.loads(verify.stdout)
        max_acked = max((r["command_sequence"] for r in acked.values()), default=0)
        check(report["last_command_sequence"] >= max_acked,
              f"round {rnd}: acknowledged sequence {max_acked} missing after kill "
              f"(journal has {report['last_command_sequence']})")
        print(f"  round {rnd}: sent {sent}, acknowledged {len(acked)}, journal records "
              f"{report['records']}, in-flight={'yes' if in_flight else 'no'}: OK")

    # Final pass: complete every command idempotently and compare with an in-memory run.
    s = Session(enginex, journal)
    for cmd in cmds[:sent]:
        s.request(cmd)
    final = s.close()
    with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
        for cmd in cmds[:sent]:
            f.write(json.dumps(cmd) + "\n")
        sim_input = f.name
    sim = subprocess.run([enginex, "simulate", "--input", sim_input, "--compact"],
                         capture_output=True, text=True, check=True)
    os.unlink(sim_input)
    expected = json.loads(sim.stdout)["final_state"]["state_hash"]
    check(final["state_hash"] == expected,
          f"state after crashes {final['state_hash']} != in-memory {expected}")
    print(f"  final state hash {expected[:16]} matches in-memory simulate over {sent} commands")
    return journal


def scenario_corruption(enginex: str, workdir: Path, journal: Path) -> None:
    data = journal.read_bytes()
    check(len(data) > 100, "journal too small for corruption test")

    corrupt = workdir / "corrupt.exj"
    flipped = bytearray(data)
    flipped[len(data) // 2] ^= 0x10
    corrupt.write_bytes(bytes(flipped))
    r = subprocess.run([enginex, "replay", "--journal", str(corrupt), "--verify"],
                       capture_output=True, text=True)
    check(r.returncode == 1, f"flipped byte: expected exit 1, got {r.returncode}")
    check(json.loads(r.stdout)["status"] == "corrupt", "flipped byte not reported corrupt")
    r = subprocess.run([enginex, "run", "--journal", str(corrupt)], input="",
                       capture_output=True, text=True)
    check(r.returncode == 1, "run started on a corrupt journal")
    check(corrupt.read_bytes() == bytes(flipped), "corrupt journal was modified")
    print("  flipped byte: replay exit 1 (corrupt), run refused, file untouched: OK")

    truncated = workdir / "truncated.exj"
    truncated.write_bytes(data[:-5])
    r = subprocess.run([enginex, "replay", "--journal", str(truncated), "--verify"],
                       capture_output=True, text=True)
    check(r.returncode == 3, f"truncated: expected exit 3, got {r.returncode}")
    full = json.loads(subprocess.run([enginex, "replay", "--journal", str(journal), "--verify"],
                                     capture_output=True, text=True).stdout)
    r = subprocess.run([enginex, "replay", "--journal", str(truncated), "--repair-tail"],
                       capture_output=True, text=True)
    check(r.returncode == 0, f"repair failed: {r.stdout}")
    rep = json.loads(r.stdout)
    check(rep["records"] == full["records"] - 1, "repair did not keep all complete records")
    check(Path(rep["preserved_tail_path"]).exists(), "cut bytes were not preserved")
    r = subprocess.run([enginex, "replay", "--journal", str(truncated), "--verify"],
                       capture_output=True, text=True)
    check(r.returncode == 0, "journal not clean after repair")
    print(f"  truncated tail: verify exit 3, repair kept {rep['records']} records and preserved "
          f"{Path(rep['preserved_tail_path']).name}: OK")


class Wire:
    """Minimal EngineX protocol client (length-prefixed JSON)."""

    def __init__(self, port: int):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.buf = b""

    def send(self, msg: dict) -> None:
        msg = {"protocol_version": 1, **msg}
        data = json.dumps(msg).encode()
        self.sock.sendall(struct.pack(">I", len(data)) + data)

    def recv(self) -> dict | None:
        while True:
            if len(self.buf) >= 4:
                (n,) = struct.unpack(">I", self.buf[:4])
                if len(self.buf) >= 4 + n:
                    payload, self.buf = self.buf[4:4 + n], self.buf[4 + n:]
                    return json.loads(payload)
            try:
                chunk = self.sock.recv(65536)
            except (ConnectionResetError, socket.timeout, OSError):
                return None
            if not chunk:
                return None
            self.buf += chunk

    def request(self, msg: dict) -> dict | None:
        self.send(msg)
        while True:
            r = self.recv()
            if r is None or r.get("type") != "book_update":
                return r

    def close(self) -> None:
        self.sock.close()


def start_server(enginex: str, journal: Path) -> tuple[subprocess.Popen, int, dict]:
    proc = subprocess.Popen([enginex, "serve", "--port", "0", "--journal", str(journal),
                             "--durability", "sync"],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    ready = json.loads(proc.stdout.readline())
    check(ready["type"] == "listening", f"server did not start: {ready}")
    return proc, ready["port"], ready


def scenario_network_kill(enginex: str, workdir: Path, seed: int) -> None:
    journal = workdir / "net.exj"
    proc, port, _ = start_server(enginex, journal)
    sent: dict[int, list[dict]] = {c: [] for c in range(3)}
    acked: dict[str, dict] = {}
    lock = threading.Lock()
    stop = threading.Event()

    def trader(c: int) -> None:
        rng = random.Random(seed * 100 + c)
        try:
            w = Wire(port)
        except OSError:
            return
        i = 0
        while not stop.is_set():
            side = rng.choice(["BUY", "SELL"])
            cmd = {"type": "new_limit", "request_id": f"net{c}-{i}",
                   "symbol": rng.choice(["AAPL", "MSFT"]), "side": side,
                   "price": 10000 + (rng.randint(-6, 3) if side == "BUY" else rng.randint(-3, 6)),
                   "quantity": rng.randint(1, 40)}
            i += 1
            with lock:
                sent[c].append(cmd)
            r = w.request(cmd)
            if r is None:
                return  # server died; this request's outcome is unknown
            if r.get("type") == "ack":
                with lock:
                    acked[cmd["request_id"]] = r

    sub = Wire(port)
    sub.send({"type": "subscribe"})
    check(sub.recv()["type"] == "snapshot_response", "no subscribe snapshot")
    threads = [threading.Thread(target=trader, args=(c,)) for c in range(3)]
    for t in threads:
        t.start()
    deadline = time.time() + 20
    while len(acked) < 60 and time.time() < deadline:
        time.sleep(0.01)
    proc.send_signal(signal.SIGKILL)
    proc.wait()
    stop.set()
    for t in threads:
        t.join()
    sub.close()
    check(len(acked) >= 60, f"too few acknowledgments before the kill ({len(acked)})")
    max_acked = max(r["command_sequence"] for r in acked.values())

    proc, port, ready = start_server(enginex, journal)
    check(ready["records_replayed"] >= max_acked,
          f"acknowledged sequence {max_acked} lost: only {ready['records_replayed']} replayed")
    sub = Wire(port)
    sub.send({"type": "subscribe"})
    snap = sub.recv()
    check(snap["event_watermark"] == ready["records_replayed"],
          "subscribe watermark does not match the recovered sequence")
    sub.close()
    by_seq: dict[int, dict] = {}
    for c, cmds in sent.items():
        w = Wire(port)
        for cmd in cmds:
            r = w.request(cmd)
            check(r is not None and r["type"] == "ack", f"retry of {cmd['request_id']} failed: {r}")
            rid = cmd["request_id"]
            if rid in acked:
                check(r["duplicate"] and r["command_sequence"] == acked[rid]["command_sequence"],
                      f"acknowledged {rid} was not returned as its original result")
            by_seq[r["command_sequence"]] = cmd
        w.close()
    proc.send_signal(signal.SIGTERM)
    out, _ = proc.communicate(timeout=30)
    final = json.loads(out.strip().splitlines()[-1])
    check(final["type"] == "shutdown", "no shutdown summary")
    with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
        for seq in sorted(by_seq):
            f.write(json.dumps(by_seq[seq]) + "\n")
        sim_input = f.name
    sim = subprocess.run([enginex, "simulate", "--input", sim_input, "--compact"],
                         capture_output=True, text=True, check=True)
    os.unlink(sim_input)
    expected = json.loads(sim.stdout)["final_state"]["state_hash"]
    check(sorted(by_seq) == list(range(1, len(by_seq) + 1)), "sequences are not contiguous")
    check(final["state_hash"] == expected, "network state after crash differs from simulate")
    print(f"  {len(acked)} acked before kill (max seq {max_acked}); {ready['records_replayed']} "
          f"replayed; {len(by_seq)} commands after retries; state hash {expected[:16]} matches "
          f"simulate in sequence order: OK")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--enginex", default="build/release/enginex")
    p.add_argument("--rounds", type=int, default=8)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--keep", action="store_true", help="keep the temporary directory")
    args = p.parse_args()
    workdir = Path(tempfile.mkdtemp(prefix="enginex-fault-"))
    try:
        print(f"kill -9 scenario ({args.rounds} rounds, seed {args.seed}):")
        journal = scenario_kill(args.enginex, workdir, args.rounds, args.seed)
        print("corruption scenario:")
        scenario_corruption(args.enginex, workdir, journal)
        print("network kill -9 scenario:")
        scenario_network_kill(args.enginex, workdir, args.seed)
        print("fault injection: ALL CHECKS PASSED")
        return 0
    except AssertionError as e:
        print(f"FAILED: {e}", file=sys.stderr)
        return 1
    finally:
        if args.keep:
            print(f"(kept {workdir})")
        else:
            shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
