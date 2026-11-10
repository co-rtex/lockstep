#!/usr/bin/env python3
"""Generates a synthetic ITCH file that stresses the price-level structure (D32): one stock
directory entry, then N buy adds 1 tick apart, each a NEW level that is either worse than
every existing one ('worse': the sorted vector's front, the hostile case) or better ('better':
its back, the cheap case). Packed with the independent tables of tools/itch_pydecode.py.

  python3 tools/itch_adversarial.py N worse|better OUT.itch
  enginex_itch_bench --input OUT.itch --only itch_book_apply --trials 3
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from itch_pydecode import STRUCTS  # noqa: E402


def header(msg_type, locate, ts):
    return msg_type.encode() + locate.to_bytes(2, "big") + (0).to_bytes(2, "big") + ts.to_bytes(6, "big")


def frame(message):
    return len(message).to_bytes(2, "big") + message


def main():
    if len(sys.argv) != 4 or sys.argv[2] not in ("worse", "better"):
        sys.exit(__doc__)
    n, mode, out = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    stock = b"ADVR    "
    buf = bytearray(frame(header("R", 1, 1) + STRUCTS["R"].pack(
        stock, b"Q", b"N", 100, b"N", b"C", b"Z ", b"P", b"N", b" ", b"1", b"N", 0, b"N")))
    for i in range(n):
        price = 2_000_000 - i if mode == "worse" else 1_000_000 + i  # Price(4), 1 tick apart
        buf += frame(header("A", 1, 10 + i) + STRUCTS["A"].pack(i + 1, b"B", 100, stock, price))
    pathlib.Path(out).write_bytes(bytes(buf))
    return 0


if __name__ == "__main__":
    sys.exit(main())
