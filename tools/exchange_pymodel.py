#!/usr/bin/env python3
"""Independent Python model of the synthetic exchange's matching and command rules, compared
with `enginex simulate` on random workloads: every response line (status, error code, command
sequence, order id, remaining, order status, cancelled quantity, executions) and the final
books must agree.

  python3 tools/exchange_pymodel.py [--seeds N] [--commands N] [--enginex build/release/enginex]

Rules modelled (docs/ENGINEERING_SPEC.md, docs/protocol.md): symbols AAPL and MSFT; price-time
priority (best price first, FIFO within a price); executions at the resting order's price;
an unfilled remainder rests at its limit; command sequence counts accepted mutating commands;
order id "ord-<sequence>", execution id "exe-<n>" (one global counter); request ids are
idempotency keys (identical body -> original result, flagged duplicate; different body ->
DUPLICATE_REQUEST_ID_CONFLICT); rejections consume no sequence. Validation is checked in the
documented order: idempotency, then symbol, side, price [1, 1e8], quantity [1, 1e6]; cancel:
unknown or non-order id -> UNKNOWN_ORDER, not resting -> ORDER_NOT_OPEN.
"""
import json
import pathlib
import random
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SYMBOLS = ("AAPL", "MSFT")


class Model:
    def __init__(self):
        self.seq = 0
        self.exe = 0
        self.records = {}   # request_id -> (body, response)
        self.orders = {}    # seq -> order dict (all accepted new_limit orders)
        self.books = {s: {"BUY": [], "SELL": []} for s in SYMBOLS}  # lists of resting order dicts

    def reject(self, cmd, code):
        return {"request_id": cmd["request_id"], "status": "rejected", "code": code}

    def process(self, cmd):
        body = {k: v for k, v in cmd.items() if k != "request_id"}
        rid = cmd["request_id"]
        if rid in self.records:
            old_body, old = self.records[rid]
            if old_body != body:
                return self.reject(cmd, "DUPLICATE_REQUEST_ID_CONFLICT")
            dup = dict(old)  # the original result (spec: "returns its original result")
            dup["duplicate"] = True
            return dup
        if cmd["type"] == "new_limit":
            if cmd["symbol"] not in SYMBOLS:
                return self.reject(cmd, "INVALID_SYMBOL")
            if cmd["side"] not in ("BUY", "SELL"):
                return self.reject(cmd, "INVALID_SIDE")
            if not 1 <= cmd["price"] <= 100_000_000:
                return self.reject(cmd, "INVALID_PRICE")
            if not 1 <= cmd["quantity"] <= 1_000_000:
                return self.reject(cmd, "INVALID_QUANTITY")
            return self.new_limit(cmd, body)
        target = cmd["order_id"]
        n = int(target[4:]) if target.startswith("ord-") and target[4:].isdigit() else 0
        if n == 0 or n not in self.orders:
            return self.reject(cmd, "UNKNOWN_ORDER")
        o = self.orders[n]
        if o["status"] in ("FILLED", "CANCELLED"):
            return self.reject(cmd, "ORDER_NOT_OPEN")
        self.seq += 1
        book = self.books[o["symbol"]][o["side"]]
        book.remove(o)
        cancelled = o["remaining"]
        o["remaining"] = 0
        o["status"] = "CANCELLED"
        resp = {"request_id": cmd["request_id"], "status": "accepted", "duplicate": False,
                "command_sequence": self.seq, "order_id": target, "cancelled_quantity": cancelled,
                "order_status": "CANCELLED", "executions": []}
        self.records[cmd["request_id"]] = (body, resp)
        return resp

    def new_limit(self, cmd, body):
        self.seq += 1
        seq = self.seq
        side, price, qty = cmd["side"], cmd["price"], cmd["quantity"]
        contra = self.books[cmd["symbol"]]["SELL" if side == "BUY" else "BUY"]
        executions = []
        remaining = qty
        while remaining > 0 and contra:
            # Best contra price: lowest ask for a buy, highest bid for a sell; FIFO = list order.
            best = min(o["price"] for o in contra) if side == "BUY" else max(o["price"] for o in contra)
            if (side == "BUY" and best > price) or (side == "SELL" and best < price):
                break
            resting = next(o for o in contra if o["price"] == best)
            traded = min(remaining, resting["remaining"])
            remaining -= traded
            resting["remaining"] -= traded
            self.exe += 1
            executions.append({"execution_id": f"exe-{self.exe}", "resting_order_id": f"ord-{resting['seq']}",
                               "price": best, "quantity": traded})
            if resting["remaining"] == 0:
                resting["status"] = "FILLED"
                contra.remove(resting)
            else:
                resting["status"] = "PARTIALLY_FILLED"
        status = "FILLED" if remaining == 0 else ("OPEN" if remaining == qty else "PARTIALLY_FILLED")
        order = {"seq": seq, "symbol": cmd["symbol"], "side": side, "price": price,
                 "remaining": remaining, "status": status}
        self.orders[seq] = order
        if remaining > 0:
            self.books[cmd["symbol"]][side].append(order)
        resp = {"request_id": cmd["request_id"], "status": "accepted", "duplicate": False,
                "command_sequence": seq, "order_id": f"ord-{seq}", "remaining": remaining,
                "order_status": status, "executions": executions, "seq_ref": seq}
        self.records[cmd["request_id"]] = (body, resp)
        return resp

    def levels(self, symbol, side):
        agg = {}
        for o in self.books[symbol][side]:
            q, c = agg.get(o["price"], (0, 0))
            agg[o["price"]] = (q + o["remaining"], c + 1)
        prices = sorted(agg, reverse=(side == "BUY"))
        return [{"price": p, "quantity": agg[p][0], "order_count": agg[p][1]} for p in prices]


def workload(rng, n):
    cmds, used = [], []
    for i in range(n):
        r = rng.random()
        if r < 0.05 and used:  # retry: identical body, or a conflicting one
            old = dict(rng.choice(used))
            if rng.random() < 0.3 and old["type"] == "new_limit":
                old["quantity"] = old["quantity"] % 1_000_000 + 1
            cmds.append(old)
            continue
        rid = f"r{i}"
        if r < 0.30:  # cancel: mostly recent orders, sometimes nonsense
            k = rng.randint(max(1, i - 60), i + 1) if i else 1
            oid = rng.choice([f"ord-{k}", f"ord-{k}", f"ord-{k}", "ord-0", "bogus", f"ord-{10 * n}"])
            cmd = {"type": "cancel", "request_id": rid, "order_id": oid}
        else:
            cmd = {"type": "new_limit", "request_id": rid,
                   "symbol": rng.choice(SYMBOLS) if rng.random() > 0.01 else "XXXX",
                   "side": rng.choice(["BUY", "SELL"]),
                   "price": rng.randint(9_950, 10_050) if rng.random() > 0.01 else rng.choice([0, 100_000_001]),
                   "quantity": rng.randint(1, 100) if rng.random() > 0.01 else rng.choice([0, 1_000_001])}
        cmds.append(cmd)
        used.append(cmd)
    return cmds


def compare(exe, seed, n):
    rng = random.Random(seed)
    cmds = workload(rng, n)
    model = Model()
    expected = [model.process(c) for c in cmds]
    with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
        for c in cmds:
            f.write(json.dumps(c) + "\n")
        path = f.name
    try:
        out = subprocess.run([exe, "simulate", "--input", path, "--output", "-", "--compact"],
                             capture_output=True, text=True)
    finally:
        pathlib.Path(path).unlink()
    if out.returncode != 0:
        sys.exit(f"seed {seed}: simulate exit {out.returncode}: {out.stderr[:300]}")
    got = json.loads(out.stdout)
    for line, (want, have) in enumerate(zip(expected, got["responses"]), start=1):
        h = {"request_id": have.get("request_id"), "status": have["status"]}
        if have["status"] == "rejected":
            h["code"] = have["error"]["code"]
        else:
            h.update({k: have[k] for k in ("duplicate", "command_sequence", "order_id", "order_status")})
            if "remaining" in have:
                h["remaining"] = have["remaining"]
            if "cancelled_quantity" in have:
                h["cancelled_quantity"] = have["cancelled_quantity"]
            h["executions"] = [{k: e[k] for k in ("execution_id", "resting_order_id", "price", "quantity")}
                               for e in have["executions"]]
        w = {k: v for k, v in want.items() if k != "seq_ref"}
        if h != w:
            sys.exit(f"seed {seed}, line {line}: command {json.dumps(cmds[line - 1])}\n"
                     f"  python: {json.dumps(w)}\n  C++:    {json.dumps(h)}")
    final = got["final_state"]
    for b in final["books"]:
        for side, key in (("BUY", "bids"), ("SELL", "asks")):
            if b[key] != model.levels(b["symbol"], side):
                sys.exit(f"seed {seed}: final {b['symbol']} {key} differ")
    if final["last_command_sequence"] != model.seq or final["last_execution_sequence"] != model.exe:
        sys.exit(f"seed {seed}: watermarks differ")
    stats = {"accepted": sum(1 for e in expected if e["status"] == "accepted"),
             "rejected": sum(1 for e in expected if e["status"] == "rejected"),
             "duplicates": sum(1 for e in expected if e.get("duplicate")),
             "executions": model.exe}
    return stats


def reply_view(have):
    """A server reply (ack/error) in the model's response format."""
    if have["type"] == "error":
        return {"request_id": have.get("request_id"), "status": "rejected", "code": have["code"]}
    h = {"request_id": have["request_id"], "status": "accepted"}
    h.update({k: have[k] for k in ("duplicate", "command_sequence", "order_id", "order_status")})
    for k in ("remaining", "cancelled_quantity"):
        if k in have:
            h[k] = have[k]
    h["executions"] = [{k: e[k] for k in ("execution_id", "resting_order_id", "price", "quantity")}
                       for e in have["executions"]]
    return h


def compare_network(exe, seed, n, journal_dir, restart=False):
    """The same workload over TCP: `enginex serve` (journaled when journal_dir is set) and
    `enginex client submit`; every reply and the server's final books must match the model.
    With restart, the server is restarted on its journal and the whole workload is sent
    again: the model processes it twice (accepted ids now replay their original results)."""
    rng = random.Random(seed)
    cmds = workload(rng, n)
    model = Model()
    expected = [model.process(c) for c in cmds]
    stats = one_pass(exe, seed, cmds, expected, journal_dir, model)
    if restart:  # same journal, new server process, same commands again
        expected = [model.process(c) for c in cmds]
        stats = one_pass(exe, seed, cmds, expected, journal_dir, model)
    return stats


def one_pass(exe, seed, cmds, expected, journal_dir, model):
    serve = [exe, "serve", "--port", "0"]
    if journal_dir:
        serve += ["--journal", str(pathlib.Path(journal_dir) / f"seed{seed}.exj"), "--durability", "none"]
    srv = subprocess.Popen(serve, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    try:
        port = json.loads(srv.stdout.readline())["port"]
        with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
            for c in cmds:
                f.write(json.dumps(c) + "\n")
            path = f.name
        try:
            out = subprocess.run([exe, "client", "submit", "--port", str(port), "--input", path],
                                 capture_output=True, text=True, timeout=300)
        finally:
            pathlib.Path(path).unlink()
    finally:
        srv.terminate()
        rest, _ = srv.communicate(timeout=60)
    replies = [json.loads(l) for l in out.stdout.splitlines() if l.strip()]
    if len(replies) != len(cmds):
        sys.exit(f"network seed {seed}: {len(replies)} replies for {len(cmds)} commands")
    for line, (want, have) in enumerate(zip(expected, replies), start=1):
        w = {k: v for k, v in want.items() if k != "seq_ref"}
        if reply_view(have) != w:
            sys.exit(f"network seed {seed}, line {line}: command {json.dumps(cmds[line - 1])}\n"
                     f"  python: {json.dumps(w)}\n  server: {json.dumps(reply_view(have))}")
    final = [json.loads(l) for l in rest.splitlines() if l.startswith("{")][-1]
    if final.get("type") != "shutdown":
        sys.exit(f"network seed {seed}: no shutdown summary")
    for b in final["books"]:
        for side, key in (("BUY", "bids"), ("SELL", "asks")):
            if b[key] != model.levels(b["symbol"], side):
                sys.exit(f"network seed {seed}: final {b['symbol']} {key} differ")
    if final["last_command_sequence"] != model.seq or final["last_execution_sequence"] != model.exe:
        sys.exit(f"network seed {seed}: watermarks differ")
    return {"accepted": sum(1 for e in expected if e["status"] == "accepted"),
            "rejected": sum(1 for e in expected if e["status"] == "rejected"),
            "duplicates": sum(1 for e in expected if e.get("duplicate")), "executions": model.exe}


def main():
    args = sys.argv[1:]
    restart = "--restart" in args  # implies --network --journal
    network = "--network" in args or restart
    journal = "--journal" in args or restart
    args = [a for a in args if a not in ("--network", "--journal", "--restart")]
    opts = {"--seeds": "20", "--commands": "3000", "--enginex": str(ROOT / "build" / "release" / "enginex")}
    for k in list(opts):
        if k in args:
            i = args.index(k); opts[k] = args[i + 1]; del args[i:i + 2]
    total = {"accepted": 0, "rejected": 0, "duplicates": 0, "executions": 0}
    with tempfile.TemporaryDirectory() as jdir:
        for seed in range(1, int(opts["--seeds"]) + 1):
            if network:
                stats = compare_network(opts["--enginex"], seed, int(opts["--commands"]),
                                        jdir if journal else None, restart)
            else:
                stats = compare(opts["--enginex"], seed, int(opts["--commands"]))
            for k, v in stats.items():
                total[k] += v
    mode = ("over TCP, journaled, restarted and resent" if restart else
            "over TCP (journaled)" if network and journal else
            "over TCP" if network else "via simulate")
    print(f"exchange_pymodel: {opts['--seeds']} seeds x {opts['--commands']} commands {mode} agree "
          f"line by line and in final books ({json.dumps(total)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
