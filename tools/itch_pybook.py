#!/usr/bin/env python3
"""Independent end-to-end check of ITCH book reconstruction: the Python decoder tables from
tools/itch_pydecode.py plus a book written here from the specification's order rules, compared
with the C++ replay's final books (every level of every book: price, shares, order count) and
its live-order count.

  python3 tools/itch_pybook.py FILE [--enginex build/release/enginex]

Rules (ITCH 5.0): A/F add a displayed order; E and C execute shares from it (C carries a
price, but the order keeps its own); X cancels shares; an order reaching zero shares leaves
the book; D deletes it; U replaces it: the old order leaves, and a new one with a new
reference, shares and price, on the same side and stock, is added at the back. P, Q and B do
not touch displayed orders. Inputs with book anomalies (unknown references, over-execution)
are out of scope: the check refuses them rather than guessing a policy.
"""
import json
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from itch_pydecode import HEADER, STRUCTS, frames  # noqa: E402  (shared spec tables only)

ROOT = pathlib.Path(__file__).resolve().parent.parent


def build(path):
    orders = {}  # reference -> [locate, side, price, shares]
    a, f, e, c, x, u = (STRUCTS[t] for t in "AFECXU")
    for p in frames(path, 0):
        t = p[0]
        locate = int.from_bytes(p[1:3], "big")
        if t == 0x41 or t == 0x46:  # A, F
            ref, side, shares, _stock, price = (a if t == 0x41 else f).unpack_from(p, HEADER)[:5]
            if ref in orders or shares == 0:
                sys.exit(f"anomaly: add {ref}")
            orders[ref] = [locate, side, price, shares]
        elif t in (0x45, 0x43, 0x58):  # E, C, X: reduce
            st = e if t == 0x45 else (c if t == 0x43 else x)
            ref, n = st.unpack_from(p, HEADER)[:2]
            o = orders.get(ref)
            if o is None or o[0] != locate or n == 0 or n > o[3]:
                sys.exit(f"anomaly: reduce {ref} by {n}")
            o[3] -= n
            if o[3] == 0:
                del orders[ref]
        elif t == 0x44:  # D
            ref = int.from_bytes(p[HEADER:HEADER + 8], "big")
            o = orders.pop(ref, None)
            if o is None or o[0] != locate:
                sys.exit(f"anomaly: delete {ref}")
        elif t == 0x55:  # U
            old, new, shares, price = u.unpack_from(p, HEADER)
            o = orders.pop(old, None)
            if o is None or o[0] != locate or new in orders or shares == 0:
                sys.exit(f"anomaly: replace {old} -> {new}")
            orders[new] = [locate, o[1], price, shares]
    books = {}
    for locate, side, price, shares in orders.values():
        levels = books.setdefault(locate, ({}, {}))[0 if side == b"B" else 1]
        lv = levels.setdefault(price, [0, 0])
        lv[0] += shares
        lv[1] += 1
    out = {}
    for locate, (bids, asks) in books.items():
        out[locate] = ([[p, *bids[p]] for p in sorted(bids, reverse=True)],
                       [[p, *asks[p]] for p in sorted(asks)])
    return out, len(orders)


def main():
    args = sys.argv[1:]
    exe = str(ROOT / "build" / "release" / "enginex")
    if "--enginex" in args:
        i = args.index("--enginex"); exe = args[i + 1]; del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    py_books, py_live = build(args[0])
    run = subprocess.run([exe, "itch", "replay", "--input", args[0], "--json", "--depth",
                          "10000000"], capture_output=True, text=True)
    cpp = json.loads(run.stdout)
    if sum(cpp["anomalies"].values()) != 0:
        sys.exit("the C++ replay reports anomalies: out of scope for this check")
    cpp_books = {}
    for b in cpp["books"]:
        bids = [[l["price"], l["shares"], l["orders"]] for l in b["bids"]]
        asks = [[l["price"], l["shares"], l["orders"]] for l in b["asks"]]
        if len(bids) != b["bid_levels"] or len(asks) != b["ask_levels"]:
            sys.exit(f"depth truncated for locate {b['locate']}")
        if bids or asks:
            cpp_books[b["locate"]] = (bids, asks)
    if cpp["book_stats"]["live_orders"] != py_live:
        sys.exit(f"live orders: python {py_live}, C++ {cpp['book_stats']['live_orders']}")
    if set(py_books) != set(cpp_books):
        sys.exit(f"books with orders differ: {sorted(set(py_books) ^ set(cpp_books))[:10]}")
    for locate in py_books:
        if py_books[locate] != cpp_books[locate]:
            sys.exit(f"locate {locate}: books differ")
    levels = sum(len(b) + len(a) for b, a in py_books.values())
    print(f"itch_pybook: {py_live} live orders, {len(py_books)} books, {levels} price levels "
          f"identical to the C++ replay (every level: price, shares, orders)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
