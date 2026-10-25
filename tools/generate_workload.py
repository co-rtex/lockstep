#!/usr/bin/env python3
"""Generate a seeded JSON-lines command file for `enginex simulate` (or `enginex run`).

The file mixes resting and crossing limit orders around a mid price, cancels of earlier
orders, occasional exact retries and a final snapshot per symbol. Cancel targets are guesses:
they predict EngineX's `ord-<command_sequence>` ids from the new orders alone, but accepted
cancels also consume sequence numbers, so some cancels name filled orders or non-order
sequences and are (correctly) rejected with ORDER_NOT_OPEN / UNKNOWN_ORDER. That exercises
the rejection path; use `enginex benchmark` for workloads with exact cancel targeting.

Example:
  tools/generate_workload.py --seed 7 --count 10000 > /tmp/w.jsonl
  build/release/enginex simulate --input /tmp/w.jsonl --output /tmp/out.json
"""

import argparse
import json
import random
import sys


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--count", type=int, default=1000, help="number of commands")
    p.add_argument("--symbols", default="AAPL,MSFT")
    p.add_argument("--mid", type=int, default=10000, help="mid price in cents")
    p.add_argument("--cancel-pct", type=int, default=25)
    p.add_argument("--retry-pct", type=int, default=3)
    args = p.parse_args()

    rng = random.Random(args.seed)
    symbols = args.symbols.split(",")
    emitted = []
    next_seq = 0  # predicted accepted sequence (cancels of closed orders do not consume one)
    order_seqs = []
    out = sys.stdout
    for i in range(args.count):
        roll = rng.randrange(100)
        if roll < args.retry_pct and emitted:
            cmd = rng.choice(emitted)
        elif roll < args.retry_pct + args.cancel_pct and order_seqs:
            cmd = {"type": "cancel", "request_id": f"g{args.seed}-{i}",
                   "order_id": f"ord-{rng.choice(order_seqs)}"}
        else:
            side = rng.choice(["BUY", "SELL"])
            offset = rng.randint(-15, 10) if side == "BUY" else rng.randint(-10, 15)
            cmd = {"type": "new_limit", "request_id": f"g{args.seed}-{i}",
                   "symbol": rng.choice(symbols), "side": side,
                   "price": args.mid + offset, "quantity": rng.randint(1, 200)}
            next_seq += 1  # valid new orders are always accepted
            order_seqs.append(next_seq)
            emitted.append(cmd)
        out.write(json.dumps(cmd, separators=(",", ":")) + "\n")
    for s in symbols:
        out.write(json.dumps({"type": "snapshot", "symbol": s, "depth": 10}) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
