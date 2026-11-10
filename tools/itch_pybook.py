#!/usr/bin/env python3
"""Independent end-to-end check of ITCH book reconstruction: the Python decoder tables from
tools/itch_pydecode.py plus a book written here from the specification's order rules, compared
with the C++ replay's final books (every level of every book: price, shares, order count) and
its live-order count.

  python3 tools/itch_pybook.py FILE [--enginex build/release/enginex] [--checkpoint-every N]
                                   [--level-vector-limit N]

With --checkpoint-every N it also compares, after every N-th message, the live-order count and
each book's best bid and ask (price, total shares) with the C++ replay's --checkpoint-out
lines, so a divergence that later cancels out is still caught.

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
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from itch_pydecode import HEADER, STRUCTS, frames  # noqa: E402  (shared spec tables only)

ROOT = pathlib.Path(__file__).resolve().parent.parent


def build(path, every=0):
    """Final books plus, when every > 0, a checkpoint after each every-th message: live orders
    and each book's best bid and ask (price, total shares) - the C++ --checkpoint-out format."""
    orders = {}  # reference -> [locate, side, price, shares]
    levels = {}  # (locate, side) -> {price: [shares, orders]}
    a, f, e, c, x, u = (STRUCTS[t] for t in "AFECXU")

    def level_add(locate, side, price, shares, count):
        lv = levels.setdefault((locate, side), {}).setdefault(price, [0, 0])
        lv[0] += shares
        lv[1] += count
        if lv[1] == 0:
            if lv[0] != 0:
                sys.exit(f"level {locate} {side} {price}: {lv[0]} shares and no orders")
            del levels[(locate, side)][price]

    checkpoints = []
    n = 0
    for p in frames(path, 0):
        n += 1
        t = p[0]
        locate = int.from_bytes(p[1:3], "big")
        if t == 0x41 or t == 0x46:  # A, F
            ref, side, shares, _stock, price = (a if t == 0x41 else f).unpack_from(p, HEADER)[:5]
            if ref in orders or shares == 0:
                sys.exit(f"anomaly: add {ref}")
            orders[ref] = [locate, side, price, shares]
            level_add(locate, side, price, shares, 1)
        elif t in (0x45, 0x43, 0x58):  # E, C, X: reduce
            st = e if t == 0x45 else (c if t == 0x43 else x)
            ref, k = st.unpack_from(p, HEADER)[:2]
            o = orders.get(ref)
            if o is None or o[0] != locate or k == 0 or k > o[3]:
                sys.exit(f"anomaly: reduce {ref} by {k}")
            o[3] -= k
            level_add(locate, o[1], o[2], -k, -1 if o[3] == 0 else 0)
            if o[3] == 0:
                del orders[ref]
        elif t == 0x44:  # D
            ref = int.from_bytes(p[HEADER:HEADER + 8], "big")
            o = orders.pop(ref, None)
            if o is None or o[0] != locate:
                sys.exit(f"anomaly: delete {ref}")
            level_add(locate, o[1], o[2], -o[3], -1)
        elif t == 0x55:  # U
            old, new, shares, price = u.unpack_from(p, HEADER)
            o = orders.pop(old, None)
            if o is None or o[0] != locate or new in orders or shares == 0:
                sys.exit(f"anomaly: replace {old} -> {new}")
            level_add(locate, o[1], o[2], -o[3], -1)
            orders[new] = [locate, o[1], price, shares]
            level_add(locate, o[1], price, shares, 1)
        if every and n % every == 0:
            checkpoints.append({"message": n, "live_orders": len(orders), "tops": tops(levels)})
    out = {}
    for (locate, side), book in levels.items():
        if book:
            entry = out.setdefault(locate, ([], []))
            ladder = entry[0 if side == b"B" else 1]
            ladder.extend([p, *book[p]] for p in sorted(book, reverse=side == b"B"))
    if sum(lv[1] for book in levels.values() for lv in book.values()) != len(orders):
        sys.exit("level order counts disagree with the order table")
    return out, len(orders), checkpoints


def tops(levels):
    best = {}
    for (locate, side), book in levels.items():
        if book:
            price = max(book) if side == b"B" else min(book)
            row = best.setdefault(locate, [locate, 0, 0, 0, 0])
            row[1 if side == b"B" else 3:3 if side == b"B" else 5] = [price, book[price][0]]
    return [best[k] for k in sorted(best)]


def main():
    args = sys.argv[1:]
    exe = str(ROOT / "build" / "release" / "enginex")
    if "--enginex" in args:
        i = args.index("--enginex"); exe = args[i + 1]; del args[i:i + 2]
    extra = []
    if "--level-vector-limit" in args:  # C++ testing aid: forces the deep-level tier (D32)
        i = args.index("--level-vector-limit"); extra = args[i:i + 2]; del args[i:i + 2]
    every = 0
    if "--checkpoint-every" in args:
        i = args.index("--checkpoint-every"); every = int(args[i + 1]); del args[i:i + 2]
        if every <= 0:
            sys.exit("--checkpoint-every must be positive")
    if len(args) != 1:
        sys.exit(__doc__)
    py_books, py_live, py_checks = build(args[0], every)
    with tempfile.TemporaryDirectory() as tmp:
        cmd = [exe, "itch", "replay", "--input", args[0], "--json", "--depth", "10000000"] + extra
        out = pathlib.Path(tmp) / "checkpoints.jsonl"
        if every:
            cmd += ["--checkpoint-every", str(every), "--checkpoint-out", str(out)]
        run = subprocess.run(cmd, capture_output=True, text=True)
        try:
            cpp = json.loads(run.stdout)
        except json.JSONDecodeError:
            sys.exit(f"C++ replay failed ({run.returncode}): {run.stderr.strip()}")
        # A file cut mid-frame (a prefix of a day) stops at the truncated tail with exit 1; the
        # Python framer stops at the same place. Any other failure is out of scope.
        if run.returncode != 0 and cpp.get("frame_error") != "truncated_tail":
            sys.exit(f"C++ replay failed ({run.returncode}): {cpp.get('stopped')}")
        cpp_checks = [json.loads(line) for line in out.read_text().splitlines()] if every else []
    if sum(cpp["anomalies"].values()) != 0:
        sys.exit("the C++ replay reports anomalies: out of scope for this check")
    if cpp["frames"] != cpp["decoded"]:
        sys.exit("the C++ replay reports decode errors: out of scope for this check")
    if len(cpp_checks) != len(py_checks):
        sys.exit(f"checkpoints: python {len(py_checks)}, C++ {len(cpp_checks)}")
    for py, cc in zip(py_checks, cpp_checks):
        cc["tops"].sort()
        if py != cc:
            diff = [k for k in py if py[k] != cc.get(k)]
            sys.exit(f"checkpoint at message {py['message']}: {diff} differ")
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
          f"identical to the C++ replay (every level: price, shares, orders)"
          + (f"; {len(py_checks)} mid-stream checkpoints (every {every} messages: live orders, "
             f"best bid and ask of every book) identical" if every else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
