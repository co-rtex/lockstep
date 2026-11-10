#!/usr/bin/env python3
"""Loopback envelope of the MoldUDP64 UDP transport: for each (live drop %, retransmit drop %,
packet rate), run `enginex itch mold-recv` and `itch mold-send` as two processes and record
whether the receiver rebuilt the exact book (hash == direct replay), how long it took and how
much recovery traffic it needed.

  python3 tools/mold_udp_sweep.py ITCH_FILE OUT.csv [BUILD_DIR] [--configs D:R:PPS[:MTU],...]

MTU is the largest packet in bytes (default 1400). Both processes are stopped on every path.
"""
import csv
import json
import os
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT = "0:0:20000,0:0:50000,0:0:70000,0:0:100000,5:10:20000,5:10:50000,5:10:70000,5:10:100000"


def main():
    args = sys.argv[1:]
    configs = DEFAULT
    if "--configs" in args:
        i = args.index("--configs")
        configs = args[i + 1]
        del args[i:i + 2]
    if len(args) < 2:
        sys.exit(__doc__)
    itch, out = args[0], args[1]
    build = pathlib.Path(args[2] if len(args) > 2 else ROOT / "build" / "release")
    exe = str(build / "enginex")
    version = subprocess.run([exe, "version"], capture_output=True, text=True).stdout.split()
    sha = version[3].rstrip(",") if len(version) > 3 else "unknown"
    direct = subprocess.run([exe, "itch", "replay", "--input", itch, "--json", "--depth", "1"],
                            capture_output=True, text=True)
    direct_hash = json.loads(direct.stdout)["state_hash"]
    fresh = not os.path.exists(out)
    with open(out, "a", newline="") as f:
        w = csv.writer(f)
        if fresh:
            w.writerow(["started_utc", "git_sha", "input", "drop_percent", "retransmit_drop_percent",
                        "rate_pps", "packets", "final_state", "delivered", "exact_book", "receiver_seconds",
                        "gaps_opened", "requests_sent", "retransmitted", "answers_rate_limited",
                        "datagrams_received", "mtu"])
        for k, cfg in enumerate(configs.split(",")):
            fields = cfg.split(":")
            if len(fields) not in (3, 4):
                sys.exit(f"bad config {cfg!r}: expected D:R:PPS or D:R:PPS:MTU")
            drop, rdrop, pps = fields[:3]
            mtu = fields[3] if len(fields) == 4 else "1400"
            rport, sport = 47200 + 2 * k, 47201 + 2 * k
            started = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            recv = subprocess.Popen([exe, "itch", "mold-recv", "--port", str(rport),
                                     "--request-port", str(sport)],
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
            try:
                time.sleep(0.3)
                send = subprocess.run([exe, "itch", "mold-send", "--input", itch, "--port",
                                       str(sport), "--dest-port", str(rport), "--drop-percent", drop,
                                       "--retransmit-drop-percent", rdrop, "--rate-pps", pps,
                                       "--mtu", mtu, "--linger-ms", "500"],
                                      capture_output=True, text=True, timeout=900)
                r = json.loads(recv.communicate(timeout=900)[0])
            finally:
                if recv.poll() is None:
                    recv.kill()
                    recv.wait()
            s = json.loads(send.stdout)
            exact = r["book_hash"] == direct_hash and r["final_state"] == "ended"
            row = [started, sha, os.path.basename(itch), drop, rdrop, pps, s["packets"],
                   r["final_state"], r["delivered"], exact, round(r["seconds"], 2), r["gaps_opened"],
                   r["requests_sent"], s["retransmitted_packets"], s["answers_rate_limited"],
                   r["datagrams"], mtu]
            w.writerow(row)
            f.flush()
            print(f"drop {drop}% rdrop {rdrop}% {pps} pps mtu {mtu}:{r['final_state']}, exact={exact}, "
                  f"{r['seconds']:.1f} s, {r['requests_sent']} requests", flush=True)


if __name__ == "__main__":
    main()
