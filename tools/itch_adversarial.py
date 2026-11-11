#!/usr/bin/env python3
"""Generates synthetic ITCH files that attack specific structures of the book. Every file has
one stock directory entry, then N messages; all are valid ITCH (no anomalies), packed with the
independent tables of tools/itch_pydecode.py.

  python3 tools/itch_adversarial.py N MODE OUT.itch
  enginex_itch_bench --input OUT.itch --only itch_book_apply --trials 3

Modes:
  worse      N buy adds 1 tick apart, each a NEW level worse than every existing one (the
             sorted level vector's front: the hostile case D32 bounds)
  better     the same, each better than every existing one (the vector's back: cheap)
  churn      8 191 levels, then N add/delete pairs at a price just below the worst level, so
             every add and delete moves the whole bounded vector (D32's worst case per message)
  hashflood  N adds whose order references are chosen to collide in a multiplicative hash with
             the fixed multiplier 2^64/phi at every table size: (C + i) * phi^-1 mod 2^64.
             They are >= 2^32, so the paged index sends them to its hash fallback.
  hashflood-lifecycle
             the same N adds, then for every order in turn: a partial execution, a partial
             cancel, a replace onto a new crafted reference, or a delete, so lookups, removals
             (backward-shift deletion) and re-inserts all run inside the hash fallback
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from itch_pydecode import STRUCTS  # noqa: E402

PHI = 0x9E3779B97F4A7C15
MODES = ("worse", "better", "churn", "hashflood", "hashflood-lifecycle")


def header(msg_type, locate, ts):
    return msg_type.encode() + locate.to_bytes(2, "big") + (0).to_bytes(2, "big") + ts.to_bytes(6, "big")


def frame(message):
    return len(message).to_bytes(2, "big") + message


def main():
    if len(sys.argv) != 4 or sys.argv[2] not in MODES:
        sys.exit(__doc__)
    n, mode, out = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    stock = b"ADVR    "
    buf = bytearray(frame(header("R", 1, 1) + STRUCTS["R"].pack(
        stock, b"Q", b"N", 100, b"N", b"C", b"Z ", b"P", b"N", b" ", b"1", b"N", 0, b"N")))
    ts = 10

    def add(ref, price):
        nonlocal ts
        ts += 1
        return frame(header("A", 1, ts) + STRUCTS["A"].pack(ref, b"B", 100, stock, price))

    if mode in ("worse", "better"):
        for i in range(n):
            price = 2_000_000 - i if mode == "worse" else 1_000_000 + i  # Price(4), 1 tick apart
            buf += add(i + 1, price)
    elif mode == "churn":
        levels = 8191  # one below the default vector bound (8 192): the deep map stays empty
        for i in range(levels):
            buf += add(i + 1, 2_000_000 - i)
        worst = 2_000_000 - levels + 1
        for j in range(n):
            ref = levels + 1 + j
            buf += add(ref, worst - 1)  # a new worst level: inserted at the vector's front
            ts += 1
            buf += frame(header("D", 1, ts) + STRUCTS["D"].pack(ref))  # and removed again
    else:  # hashflood, hashflood-lifecycle
        inv = pow(PHI, -1, 1 << 64)
        base = 1 << 40

        def crafted(i):
            return ((base + i) * inv) % (1 << 64)
        for i in range(n):
            buf += add(crafted(i), 1_000_000 + (i % 100))
        if mode == "hashflood-lifecycle":
            def msg(t, body):
                nonlocal ts
                ts += 1
                return frame(header(t, 1, ts) + body)
            for i in range(n):  # every order starts with 100 shares (see add())
                ref = crafted(i)
                k = i % 4
                if k == 0:  # partial execution, match number i
                    buf += msg("E", STRUCTS["E"].pack(ref, 40, i + 1))
                elif k == 1:  # partial cancel
                    buf += msg("X", STRUCTS["X"].pack(ref, 30))
                elif k == 2:  # replace onto a new crafted reference, new price and size
                    buf += msg("U", STRUCTS["U"].pack(ref, crafted(n + i), 70, 1_000_200 + (i % 50)))
                else:  # delete
                    buf += msg("D", STRUCTS["D"].pack(ref))
    pathlib.Path(out).write_bytes(bytes(buf))
    return 0


if __name__ == "__main__":
    sys.exit(main())
