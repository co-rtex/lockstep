#!/usr/bin/env python3
"""Independent ITCH 5.0 decoder (Python `struct`, written from the specification's message
tables) used as a differential check of the C++ decoder: every frame of a length-prefixed file
is decoded here and compared field by field with `enginex itch dump --all-fields`.

  python3 tools/itch_pydecode.py FILE [--limit N] [--enginex build/release/enginex]
  python3 tools/itch_pydecode.py --random N [--seed S]   (N random messages of each of the
      23 types, every field random, written to a temp file and compared the same way)

It shares no code with the C++ decoder: field order, widths and offsets come from the tables
below, each total size is asserted against the specification's message length, and frames
are validated independently. Exit 0 only if every frame agrees, including which frames are
rejected and why (wrong length, unknown type, invalid enumerated field).
"""
import json
import pathlib
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Header: message type (1), stock locate (2), tracking number (2), timestamp (6 bytes, ns).
HEADER = 11
# Per type: (total length from the spec, [(field name, struct code)]) after the header.
# Codes: c = 1 alpha byte, Ns = N alpha bytes, H/I/Q = 2/4/8-byte unsigned big-endian.
SPEC = {
    "S": (12, [("event_code", "c")]),
    "R": (39, [("stock", "8s"), ("market_category", "c"), ("financial_status", "c"),
               ("round_lot_size", "I"), ("round_lots_only", "c"), ("issue_classification", "c"),
               ("issue_subtype", "2s"), ("authenticity", "c"), ("short_sale_threshold", "c"),
               ("ipo_flag", "c"), ("luld_tier", "c"), ("etp_flag", "c"),
               ("etp_leverage_factor", "I"), ("inverse_indicator", "c")]),
    "H": (25, [("stock", "8s"), ("trading_state", "c"), ("reserved", "c"), ("reason", "4s")]),
    "Y": (20, [("stock", "8s"), ("reg_sho_action", "c")]),
    "L": (26, [("mpid", "4s"), ("stock", "8s"), ("primary_market_maker", "c"),
               ("market_maker_mode", "c"), ("participant_state", "c")]),
    "V": (35, [("level1", "Q"), ("level2", "Q"), ("level3", "Q")]),
    "W": (12, [("breached_level", "c")]),
    "K": (28, [("stock", "8s"), ("release_time", "I"), ("release_qualifier", "c"),
               ("ipo_price", "I")]),
    "J": (35, [("stock", "8s"), ("reference_price", "I"), ("upper_collar", "I"),
               ("lower_collar", "I"), ("extension", "I")]),
    "h": (21, [("stock", "8s"), ("market_code", "c"), ("halt_action", "c")]),
    "A": (36, [("order_ref", "Q"), ("side", "c"), ("shares", "I"), ("stock", "8s"),
               ("price", "I")]),
    "F": (40, [("order_ref", "Q"), ("side", "c"), ("shares", "I"), ("stock", "8s"),
               ("price", "I"), ("attribution", "4s")]),
    "E": (31, [("order_ref", "Q"), ("executed_shares", "I"), ("match_number", "Q")]),
    "C": (36, [("order_ref", "Q"), ("executed_shares", "I"), ("match_number", "Q"),
               ("printable", "c"), ("execution_price", "I")]),
    "X": (23, [("order_ref", "Q"), ("canceled_shares", "I")]),
    "D": (19, [("order_ref", "Q")]),
    "U": (35, [("original_ref", "Q"), ("new_ref", "Q"), ("shares", "I"), ("price", "I")]),
    "P": (44, [("order_ref", "Q"), ("side", "c"), ("shares", "I"), ("stock", "8s"),
               ("price", "I"), ("match_number", "Q")]),
    "Q": (40, [("shares", "Q"), ("stock", "8s"), ("cross_price", "I"), ("match_number", "Q"),
               ("cross_type", "c")]),
    "B": (19, [("match_number", "Q")]),
    "I": (50, [("paired_shares", "Q"), ("imbalance_shares", "Q"), ("imbalance_direction", "c"),
               ("stock", "8s"), ("far_price", "I"), ("near_price", "I"),
               ("current_reference_price", "I"), ("cross_type", "c"),
               ("price_variation_indicator", "c")]),
    "N": (20, [("stock", "8s"), ("interest_flag", "c")]),
    "O": (48, [("stock", "8s"), ("open_eligibility_status", "c"),
               ("minimum_allowable_price", "I"), ("maximum_allowable_price", "I"),
               ("near_execution_price", "I"), ("near_execution_time", "Q"),
               ("lower_price_range_collar", "I"), ("upper_price_range_collar", "I")]),
}
STRUCTS = {}
for t, (length, fields) in SPEC.items():
    st = struct.Struct(">" + "".join(code for _, code in fields))
    assert HEADER + st.size == length, (t, HEADER + st.size, length)
    STRUCTS[t] = st


def printable(raw: bytes) -> str:
    """Same escaping contract as the CLI: 0x20..0x7E except backslash as is, else \\xNN."""
    return "".join(chr(b) if 0x20 <= b <= 0x7E and b != 0x5C else f"\\x{b:02X}" for b in raw)


def decode(payload: bytes):
    """Returns (record dict, None) or (None, error name)."""
    if not payload:
        return None, "empty"
    t = chr(payload[0]) if payload[0] < 0x80 else None
    if t not in SPEC:
        return None, "unknown_type"
    length, fields = SPEC[t]
    if len(payload) != length:
        return None, "length_mismatch"
    rec = {"type": t,
           "stock_locate": int.from_bytes(payload[1:3], "big"),
           "tracking_number": int.from_bytes(payload[3:5], "big"),
           "timestamp_ns": int.from_bytes(payload[5:11], "big")}
    for (name, _), value in zip(fields, STRUCTS[t].unpack_from(payload, HEADER)):
        rec[name] = printable(value) if isinstance(value, bytes) else value
    # Enumerated fields the C++ decoder validates (it rejects other values as invalid_field).
    if t in "AF" and rec["side"] not in ("B", "S"):
        return None, "invalid_field"
    if t == "C" and rec["printable"] not in ("Y", "N"):
        return None, "invalid_field"
    return rec, None


def frames(path, limit):
    data = pathlib.Path(path).read_bytes()
    pos, n = 0, 0
    while len(data) - pos >= 2 and (limit == 0 or n < limit):
        length = int.from_bytes(data[pos:pos + 2], "big")
        if length == 0 or len(data) - pos - 2 < length:
            return  # zero length or cut-off tail: the C++ side reports a frame error here
        yield data[pos + 2:pos + 2 + length]
        pos += 2 + length
        n += 1


def random_file(path, per_type, seed):
    """Every type, every field random (enumerated fields valid, alpha bytes arbitrary)."""
    import random
    rng = random.Random(seed)
    out = bytearray()
    for _ in range(per_type):
        for t, (length, fields) in SPEC.items():
            values = []
            for name, code in fields:
                if name == "side" and t in "AF":
                    values.append(rng.choice([b"B", b"S"]))
                elif name == "printable":
                    values.append(rng.choice([b"Y", b"N"]))
                elif code == "c":
                    values.append(bytes([rng.randrange(256)]))
                elif code.endswith("s"):
                    values.append(bytes(rng.randrange(256) for _ in range(int(code[:-1]))))
                else:
                    values.append(rng.randrange(1 << (8 * struct.calcsize(code))))
            msg = (t.encode() + rng.randrange(1 << 16).to_bytes(2, "big")
                   + rng.randrange(1 << 16).to_bytes(2, "big")
                   + rng.randrange(1 << 48).to_bytes(6, "big") + STRUCTS[t].pack(*values))
            assert len(msg) == length
            out += len(msg).to_bytes(2, "big") + msg
    pathlib.Path(path).write_bytes(bytes(out))


def main():
    args = sys.argv[1:]
    if "--random" in args:
        import tempfile
        i = args.index("--random"); per_type = int(args[i + 1]); del args[i:i + 2]
        seed = 1
        if "--seed" in args:
            j = args.index("--seed"); seed = int(args[j + 1]); del args[j:j + 2]
        tmp = tempfile.NamedTemporaryFile(suffix=".itch", delete=False)
        tmp.close()
        random_file(tmp.name, per_type, seed)
        try:
            return compare(tmp.name, 0, exe_from(args))
        finally:
            pathlib.Path(tmp.name).unlink()
    exe = exe_from(args)
    limit = 0
    if "--limit" in args:
        i = args.index("--limit"); limit = int(args[i + 1]); del args[i:i + 2]
    if len(args) != 1:
        sys.exit(__doc__)
    return compare(args[0], limit, exe)


def exe_from(args):
    """Removes and returns --enginex PATH (default build/release/enginex)."""
    if "--enginex" in args:
        i = args.index("--enginex"); exe = args[i + 1]; del args[i:i + 2]
        return exe
    return str(ROOT / "build" / "release" / "enginex")


def compare(path, limit, exe):
    cmd = [exe, "itch", "dump", "--input", path, "--all-fields"]
    if limit:
        cmd += ["--limit", str(limit)]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, bufsize=1 << 20)
    counts, errors, n = {}, {}, 0
    for payload in frames(path, limit):
        line = proc.stdout.readline()
        if not line:
            sys.exit(f"C++ dump ended early at frame {n + 1}")
        got = json.loads(line)
        n += 1
        want, err = decode(payload)
        if err is not None:
            if got.get("decode_error") != err:
                sys.exit(f"frame {n}: python says {err}, C++ says {got}")
            errors[err] = errors.get(err, 0) + 1
            continue
        got.pop("frame", None)
        if got != want:
            diff = {k: (want.get(k), got.get(k)) for k in set(want) | set(got)
                    if want.get(k) != got.get(k)}
            sys.exit(f"frame {n} ({want['type']}): fields differ (python, C++): {diff}")
        counts[want["type"]] = counts.get(want["type"], 0) + 1
    rest = proc.stdout.read()
    proc.wait()
    extra = [json.loads(l) for l in rest.splitlines() if l]
    if any("frame_error" not in e for e in extra):
        sys.exit(f"C++ dump has {len(extra)} more lines than python frames: {extra[:2]}")
    print(f"itch_pydecode: {n} frames agree field by field "
          f"({sum(counts.values())} messages, {sum(errors.values())} rejected identically)")
    print("  by type:", json.dumps(dict(sorted(counts.items()))))
    if errors:
        print("  rejected:", json.dumps(errors))
    return 0


if __name__ == "__main__":
    sys.exit(main())
