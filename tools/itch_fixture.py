#!/usr/bin/env python3
"""Writes the hand-scripted ITCH 5.0 golden fixtures in tests/testdata/itch/.

Independent of the C++ encoder: every message is packed here with `struct` directly from the
offset tables of the Nasdaq TotalView-ITCH 5.0 specification (big-endian, '>' format), and the
expected book after the scenario is written out by hand below, not computed by a simulator.
The C++ test (ItchFixture.*) replays the files and compares against the expectations.

Files use the 2-byte big-endian length-prefixed file convention. These are synthetic
fixtures from the public format, not Nasdaq data.

Usage: python3 tools/itch_fixture.py [--check]   (--check: verify files are up to date)
"""
import json
import pathlib
import struct
import sys

OUT = pathlib.Path(__file__).resolve().parent.parent / "tests" / "testdata" / "itch"


def ts48(ns):
    return ns.to_bytes(6, "big")


def hdr(t, locate, tracking, ns):
    return t.encode() + struct.pack(">HH", locate, tracking) + ts48(ns)


def stock(s):
    return s.ljust(8).encode()


def system_event(ns, code):  # 'S' 12
    return hdr("S", 0, 0, ns) + code.encode()


def directory(ns, locate, sym):  # 'R' 39
    return (hdr("R", locate, 0, ns) + stock(sym) + b"Q" + b"N" + struct.pack(">I", 100) + b"N" +
            b"C" + b"Z " + b"P" + b"N" + b"N" + b"1" + b"N" + struct.pack(">I", 0) + b"N")


def add(ns, locate, ref, side, shares, sym, price, mpid=None):  # 'A' 36 / 'F' 40
    body = struct.pack(">Q", ref) + side.encode() + struct.pack(">I", shares) + stock(sym) + \
        struct.pack(">I", price)
    if mpid is None:
        return hdr("A", locate, 0, ns) + body
    return hdr("F", locate, 0, ns) + body + mpid.encode()


def executed(ns, locate, ref, shares, match):  # 'E' 31
    return hdr("E", locate, 0, ns) + struct.pack(">QIQ", ref, shares, match)


def executed_px(ns, locate, ref, shares, match, printable, price):  # 'C' 36
    return hdr("C", locate, 0, ns) + struct.pack(">QIQ", ref, shares, match) + \
        printable.encode() + struct.pack(">I", price)


def cancel(ns, locate, ref, shares):  # 'X' 23
    return hdr("X", locate, 0, ns) + struct.pack(">QI", ref, shares)


def delete(ns, locate, ref):  # 'D' 19
    return hdr("D", locate, 0, ns) + struct.pack(">Q", ref)


def replace(ns, locate, old, new, shares, price):  # 'U' 35
    return hdr("U", locate, 0, ns) + struct.pack(">QQII", old, new, shares, price)


def trade(ns, locate, shares, sym, price, match):  # 'P' 44
    return hdr("P", locate, 0, ns) + struct.pack(">Q", 0) + b"B" + struct.pack(">I", shares) + \
        stock(sym) + struct.pack(">IQ", price, match)


def cross(ns, locate, shares, sym, price, match, kind):  # 'Q' 40
    return hdr("Q", locate, 0, ns) + struct.pack(">Q", shares) + stock(sym) + \
        struct.pack(">IQ", price, match) + kind.encode()


def broken(ns, locate, match):  # 'B' 19
    return hdr("B", locate, 0, ns) + struct.pack(">Q", match)


def trading_action(ns, locate, sym, state):  # 'H' 25
    return hdr("H", locate, 0, ns) + stock(sym) + state.encode() + b" " + b"    "


EXPECTED_LENGTHS = {"S": 12, "R": 39, "A": 36, "F": 40, "E": 31, "C": 36, "X": 23, "D": 19,
                    "U": 35, "P": 44, "Q": 40, "B": 19, "H": 25}


def frame(msgs):
    out = bytearray()
    for m in msgs:
        t = chr(m[0])
        assert len(m) == EXPECTED_LENGTHS[t], (t, len(m))
        out += struct.pack(">H", len(m)) + m
    return bytes(out)


T0 = 34_200_000_000_000  # 09:30:00.000000000


def lifecycle():
    """Two symbols; every order-lifecycle rule exercised once. Comments give the book after."""
    t = iter(range(T0, T0 + 10_000_000, 1000))
    m = [
        system_event(next(t), "O"),
        directory(next(t), 1, "AAPL"),
        directory(next(t), 2, "MSFT"),
        system_event(next(t), "S"),
        system_event(next(t), "Q"),
        trading_action(next(t), 1, "AAPL", "T"),
        # AAPL bids: 1 @ 150.0000 x100, 2 @ 150.0000 x200 (FIFO 1,2), 3 @ 149.9900 x300
        add(next(t), 1, 1, "B", 100, "AAPL", 1_500_000),
        add(next(t), 1, 2, "B", 200, "AAPL", 1_500_000, mpid="GSCO"),
        add(next(t), 1, 3, "B", 300, "AAPL", 1_499_900),
        # AAPL asks: 4 @ 150.0100 x50, 5 @ 150.0200 x500
        add(next(t), 1, 4, "S", 50, "AAPL", 1_500_100),
        add(next(t), 1, 5, "S", 500, "AAPL", 1_500_200),
        # 1 executes 40 (keeps priority): bids 150.00 = [1 x60, 2 x200]
        executed(next(t), 1, 1, 40, 1),
        # 4 fully executed by a non-printable C at 150.0050: ask level 150.01 removed
        executed_px(next(t), 1, 4, 50, 2, "N", 1_500_050),
        # 2 partially canceled by 50 (keeps priority): 150.00 = [1 x60, 2 x150]
        cancel(next(t), 1, 2, 50),
        # 1 replaced as 6 at the same price with 60: loses priority → 150.00 = [2 x150, 6 x60]
        replace(next(t), 1, 1, 6, 60, 1_500_000),
        # 3 replaced as 7 at 150.0000 x10: 149.99 level removed; 150.00 = [2, 6, 7]
        replace(next(t), 1, 3, 7, 10, 1_500_000),
        # trades/crosses/breaks: no book effect
        trade(next(t), 1, 1000, "AAPL", 1_500_050, 3),
        cross(next(t), 1, 25_000, "AAPL", 1_500_000, 4, "O"),
        broken(next(t), 1, 3),
        # MSFT: 10 @ 400.0000 x10 bid, 11 @ 400.5000 x20 ask, then 10 deleted
        add(next(t), 2, 10, "B", 10, "MSFT", 4_000_000),
        add(next(t), 2, 11, "S", 20, "MSFT", 4_005_000),
        delete(next(t), 2, 10),
        # 5 executed 100 printable with price: asks 150.02 x400
        executed_px(next(t), 1, 5, 100, 5, "Y", 1_500_200),
        system_event(next(t), "M"),
        system_event(next(t), "E"),
        system_event(next(t), "C"),
    ]
    expected = {
        "frames": len(m),
        "anomalies": 0,
        "messages_by_type": {"S": 6, "R": 2, "H": 1, "A": 6, "F": 1, "E": 1, "C": 2, "X": 1,
                             "D": 1, "U": 2, "P": 1, "Q": 1, "B": 1},
        "books": {
            "AAPL": {
                "bids": [[1_500_000, 220, 3]],
                "asks": [[1_500_200, 400, 1]],
                "bid_orders": [[2, 150], [6, 60], [7, 10]],
                "ask_orders": [[5, 400]],
            },
            "MSFT": {"bids": [], "asks": [[4_005_000, 20, 1]], "bid_orders": [],
                     "ask_orders": [[11, 20]]},
        },
        "executed_shares": 190,            # 40 + 50 + 100
        "printable_executed_shares": 140,  # E 40 + printable C 100 (non-printable C excluded)
        "canceled_shares": 50,
        "non_displayed_trade_shares": 1000,
        "cross_shares": 25_000,
        "broken_trades": 1,
        "live_orders": 5,
        "last_system_event": "C",
    }
    return frame(m), expected


def malformed():
    """Valid messages around one unknown type and one wrong length, then a truncated tail."""
    t = iter(range(T0, T0 + 1_000_000, 1000))
    good = frame([add(next(t), 1, 1, "B", 100, "AAPL", 1_000_000)])
    unknown = struct.pack(">H", 5) + b"z" + b"\x00" * 4
    bad_len = struct.pack(">H", 20) + b"A" + b"\x00" * 19
    good2 = frame([add(next(t), 1, 2, "S", 100, "AAPL", 1_000_100)])
    truncated = struct.pack(">H", 19) + delete(next(t), 1, 1)[:10]
    data = good + unknown + bad_len + good2 + truncated
    expected = {"frames": 4, "decoded": 2, "unknown_type": 1, "length_mismatch": 1,
                "frame_error": "truncated_tail",
                "frame_error_offset": len(good) + len(unknown) + len(bad_len) + len(good2),
                "live_orders": 2}
    return data, expected


def mold_packets():
    """Fuzz seed: the lifecycle messages as MoldUDP64 packets (session "FIXTURE", 3 messages per
    packet, then end of session), each packet preceded by a 2-byte length for the fuzz harness."""
    data, _ = lifecycle()
    msgs, pos = [], 0
    while pos < len(data):
        n = struct.unpack(">H", data[pos:pos + 2])[0]
        msgs.append(data[pos + 2:pos + 2 + n])
        pos += 2 + n
    session = b"FIXTURE".ljust(10)
    out, seq = bytearray(), 1
    for i in range(0, len(msgs), 3):
        batch = msgs[i:i + 3]
        pkt = session + struct.pack(">QH", seq, len(batch))
        for m in batch:
            pkt += struct.pack(">H", len(m)) + m
        out += struct.pack(">H", len(pkt)) + pkt
        seq += len(batch)
    end = session + struct.pack(">QH", seq, 0xFFFF)
    out += struct.pack(">H", len(end)) + end
    return bytes(out)


def mold_gaps():
    """Fuzz seed for the MoldUDP64 reorder buffer: packets out of order, a heartbeat that
    reveals a hole, a later packet that opens an adjacent hole (merge), fills in the middle
    of holes, overlapping retransmissions and an end of session (2-byte length before each)."""
    data, _ = lifecycle()
    msgs, pos = [], 0
    while pos < len(data):
        n = struct.unpack(">H", data[pos:pos + 2])[0]
        msgs.append(data[pos + 2:pos + 2 + n])
        pos += 2 + n
    session = b"FIXTURE".ljust(10)

    def packet(first, count):
        pkt = session + struct.pack(">QH", first, count)
        for s in range(first, first + count):
            m = msgs[(s - 1) % len(msgs)]
            pkt += struct.pack(">H", len(m)) + m
        return pkt

    def control(next_seq, code):
        return session + struct.pack(">QH", next_seq, code)

    order = [packet(1, 3), control(13, 0), packet(16, 3), packet(7, 3), packet(2, 4),
             packet(4, 3), packet(10, 6), control(19, 0xFFFF), packet(19, 2), control(21, 0xFFFF)]
    return b"".join(struct.pack(">H", len(p)) + p for p in order)


def main():
    check = "--check" in sys.argv[1:]
    stale = []
    for name, (data, expected) in {"lifecycle": lifecycle(), "malformed": malformed()}.items():
        files = {OUT / f"{name}.itch": data,
                 OUT / f"{name}.expected.json": (json.dumps(expected, indent=2) + "\n").encode()}
        for path, content in files.items():
            if check:
                if not path.exists() or path.read_bytes() != content:
                    stale.append(str(path))
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
                print(f"wrote {path} ({len(content)} bytes)")
    corpus_dir = pathlib.Path(__file__).resolve().parent.parent / "fuzz" / "corpus"
    for corpus, content in ((corpus_dir / "mold_packets.bin", mold_packets()),
                            (corpus_dir / "mold_gaps.bin", mold_gaps())):
        if check:
            if not corpus.exists() or corpus.read_bytes() != content:
                stale.append(str(corpus))
        else:
            corpus.parent.mkdir(parents=True, exist_ok=True)
            corpus.write_bytes(content)
            print(f"wrote {corpus} ({len(content)} bytes)")
    if stale:
        print("stale fixtures (run tools/itch_fixture.py):", *stale, sep="\n  ")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
