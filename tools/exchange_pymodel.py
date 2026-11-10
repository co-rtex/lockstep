#!/usr/bin/env python3
"""Independent Python model of the synthetic exchange's matching and command rules, compared
with `enginex simulate` on random workloads: every response line (status, error code, command
sequence, order id, remaining, order status, cancelled quantity, executions) and the final
books must agree.

  python3 tools/exchange_pymodel.py [--seeds N] [--commands N] [--enginex build/release/enginex]
      [--price-band LO:HI] [--max-qty N]
      [--network [--journal] | --restart | --clients K [--journal] [--chaos M]]

--network sends the workload over TCP with a subscriber that checks every book_update;
--restart also restarts the server on its journal and resends everything; --clients K splits
the workload over K concurrent connections and checks that the server's interleaving is
linearizable against the model (compare_concurrent); --chaos M adds M threads of hostile
connections alongside (see chaos()); with --journal the journal must replay to the server's
final state hash.

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
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
SYMBOLS = ("AAPL", "MSFT")


class Model:
    def __init__(self):
        self.seq = 0
        self.exe = 0
        self.records = {}   # request_id -> (body, response)
        self.orders = {}    # seq -> order dict (all accepted new_limit orders)
        self.books = {s: {"BUY": [], "SELL": []} for s in SYMBOLS}  # lists of resting order dicts
        self.events = []    # one per accepted, non-duplicate command (the book_update feed)

    def publish(self, cmd_type, symbol, order_id, executions, touched):
        """The book_update for an accepted command: touched levels with their new totals."""
        levels = []
        for side, price in sorted(touched):
            q = sum(o["remaining"] for o in self.books[symbol][side] if o["price"] == price)
            c = sum(1 for o in self.books[symbol][side] if o["price"] == price)
            levels.append({"side": side, "price": price, "quantity": q, "order_count": c})

        def best(side):
            lv = self.levels(symbol, side)
            return {"price": lv[0]["price"], "quantity": lv[0]["quantity"]} if lv else None

        self.events.append({"event_sequence": len(self.events) + 1, "command_sequence": self.seq,
                            "command_type": cmd_type, "symbol": symbol, "order_id": order_id,
                            "executions": executions, "levels": levels,
                            "best_bid": best("BUY"), "best_ask": best("SELL")})

    def reject(self, cmd, code):
        return {"request_id": cmd["request_id"], "status": "rejected", "code": code}

    def process(self, cmd):
        r = self.precheck(cmd)
        if r is not None:
            return r
        body = {k: v for k, v in cmd.items() if k != "request_id"}
        if cmd["type"] == "new_limit":
            return self.new_limit(cmd, body)
        target = cmd["order_id"]
        o = self.orders[int(target[4:])]
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
        self.publish("cancel", o["symbol"], target, [], {(o["side"], o["price"])})
        return resp

    def precheck(self, cmd):
        """The reply for a command that is rejected or replayed as a duplicate in the current
        state, or None if it would be accepted. Never changes the model."""
        body = {k: v for k, v in cmd.items() if k != "request_id"}
        rid = cmd["request_id"]
        # Stage 1, the parser: values that cannot be represented as a command are rejected
        # first, ahead of idempotency (docs/protocol.md, "Which code ...").
        if cmd["type"] == "new_limit" and cmd["side"] not in ("BUY", "SELL"):
            return self.reject(cmd, "INVALID_SIDE")
        # Stage 2, the state machine: request_id form, idempotency, then the fields.
        if rid == "" or len(rid.encode()) > 64:
            return self.reject(cmd, "INVALID_REQUEST")
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
            if not 1 <= cmd["price"] <= 100_000_000:
                return self.reject(cmd, "INVALID_PRICE")
            if not 1 <= cmd["quantity"] <= 1_000_000:
                return self.reject(cmd, "INVALID_QUANTITY")
            return None
        target = cmd["order_id"]
        n = int(target[4:]) if target.startswith("ord-") and target[4:].isdigit() else 0
        if n == 0 or n not in self.orders:
            return self.reject(cmd, "UNKNOWN_ORDER")
        if self.orders[n]["status"] in ("FILLED", "CANCELLED"):
            return self.reject(cmd, "ORDER_NOT_OPEN")
        return None

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
        contra_side = "SELL" if side == "BUY" else "BUY"
        touched = {(contra_side, e["price"]) for e in executions}
        if remaining > 0:
            touched.add((side, price))
        self.publish("new_limit", cmd["symbol"], f"ord-{seq}", executions, touched)
        return resp

    def levels(self, symbol, side):
        agg = {}
        for o in self.books[symbol][side]:
            q, c = agg.get(o["price"], (0, 0))
            agg[o["price"]] = (q + o["remaining"], c + 1)
        prices = sorted(agg, reverse=(side == "BUY"))
        return [{"price": p, "quantity": agg[p][0], "order_count": agg[p][1]} for p in prices]


# Workload regime (set from --price-band LO:HI and --max-qty): a narrow band crosses often; a
# wide band builds deep books; large quantities make orders sweep many levels.
PRICE_BAND = (9_950, 10_050)
MAX_QTY = 100


def workload(rng, n):
    cmds, used = [], []
    for i in range(n):
        r = rng.random()
        if r < 0.05 and used:  # retry: identical body, a conflicting one, or an unparseable one
            old = dict(rng.choice(used))
            if rng.random() < 0.3 and old["type"] == "new_limit":
                old["quantity"] = old["quantity"] % 1_000_000 + 1
            elif rng.random() < 0.1 and old["type"] == "new_limit":
                old["side"] = "HOLD"  # parser rejects it before the idempotency check
            cmds.append(old)
            continue
        rid = f"r{i}"
        if rng.random() < 0.03:  # malformed request ids: empty or over 64 bytes
            rid = rng.choice(["", "x" * 65, "é" * 33])
        if r < 0.30:  # cancel: mostly recent orders, sometimes nonsense
            k = rng.randint(max(1, i - 60), i + 1) if i else 1
            oid = rng.choice([f"ord-{k}", f"ord-{k}", f"ord-{k}", "ord-0", "bogus", f"ord-{10 * n}"])
            cmd = {"type": "cancel", "request_id": rid, "order_id": oid}
        else:
            cmd = {"type": "new_limit", "request_id": rid,
                   "symbol": rng.choice(SYMBOLS) if rng.random() > 0.01 else "XXXX",
                   "side": rng.choice(["BUY", "SELL"]) if rng.random() > 0.01 else "HOLD",
                   "price": rng.randint(*PRICE_BAND) if rng.random() > 0.01 else rng.choice([0, 100_000_001]),
                   "quantity": rng.randint(1, MAX_QTY) if rng.random() > 0.01 else rng.choice([0, 1_000_001])}
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
    before = len(model.events)
    expected = [model.process(c) for c in cmds]
    stats = one_pass(exe, seed, cmds, expected, journal_dir, model, model.events[before:], before)
    if restart:  # same journal, new server process, same commands again
        before = len(model.events)
        expected = [model.process(c) for c in cmds]
        stats = one_pass(exe, seed, cmds, expected, journal_dir, model, model.events[before:], before)
    return stats


def event_view(e):
    """A book_update in the model's event format."""
    return {"event_sequence": e["event_sequence"], "command_sequence": e["command_sequence"],
            "command_type": e["command_type"], "symbol": e["symbol"], "order_id": e["order_id"],
            "executions": [{k: x[k] for k in ("execution_id", "resting_order_id", "price", "quantity")}
                           for x in e["executions"]],
            "levels": sorted(e["levels"], key=lambda l: (l["side"], l["price"])),
            "best_bid": e["best_bid"], "best_ask": e["best_ask"]}


def stop(proc, read=True, timeout=60):
    """Stops a child for good: SIGTERM, then SIGKILL if it has not exited within `timeout`.
    Returns its remaining stdout when `read` (pass read=False if a thread owns the pipe)."""
    if proc is None:
        return ""
    if proc.poll() is None:
        proc.terminate()
    try:
        if read:
            try:
                return proc.communicate(timeout=timeout)[0] or ""
            except ValueError:  # pipes already consumed by an earlier communicate()
                proc.wait(timeout=timeout)
                return ""
        proc.wait(timeout=timeout)
        return ""
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        return ""


def replies_of(out, label):
    """Parsed reply lines of `client submit`; a client_error line (the client gave up on a
    line, timed out, or lost the server) is a failure with its message, not a KeyError."""
    lines = [json.loads(l) for l in out.splitlines() if l.strip()]
    for line in lines:
        if line.get("type") == "client_error":
            sys.exit(f"{label}: client_error: {line.get('message')}")
    return lines


def one_pass(exe, seed, cmds, expected, journal_dir, model, events, watermark):
    serve = [exe, "serve", "--port", "0"]
    if journal_dir:
        serve += ["--journal", str(pathlib.Path(journal_dir) / f"seed{seed}.exj"), "--durability", "none"]
    srv = subprocess.Popen(serve, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    sub = None
    try:
        port = json.loads(srv.stdout.readline())["port"]
        if events:  # subscribe before the first command: snapshot, then every book_update
            sub = subprocess.Popen([exe, "client", "subscribe", "--port", str(port), "--count",
                                    str(len(events))], stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL, text=True)
            snap = json.loads(sub.stdout.readline())
            if snap.get("type") != "snapshot_response" or not snap.get("subscribed") or \
                    snap["event_watermark"] != watermark:
                sys.exit(f"network seed {seed}: bad subscribe snapshot {json.dumps(snap)[:200]}")
        with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
            for c in cmds:
                f.write(json.dumps(c) + "\n")
            path = f.name
        try:
            out = subprocess.run([exe, "client", "submit", "--port", str(port), "--input", path],
                                 capture_output=True, text=True, timeout=300)
        finally:
            pathlib.Path(path).unlink()
        feed = []
        if sub:
            feed_out, _ = sub.communicate(timeout=120)
            feed = [json.loads(l) for l in feed_out.splitlines() if l.strip()]
    finally:
        stop(sub, read=False)
        rest = stop(srv)
    updates = [u for u in feed if u.get("type") == "book_update"]
    if len(updates) != len(events):
        sys.exit(f"network seed {seed}: {len(updates)} book_updates, model has {len(events)}")
    for want, have in zip(events, updates):
        w = dict(want, levels=sorted(want["levels"], key=lambda l: (l["side"], l["price"])))
        if event_view(have) != w:
            sys.exit(f"network seed {seed}, event {want['event_sequence']}:\n"
                     f"  python: {json.dumps(w)}\n  server: {json.dumps(event_view(have))}")
    replies = replies_of(out.stdout, f"network seed {seed}")
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
            "duplicates": sum(1 for e in expected if e.get("duplicate")), "executions": model.exe,
            "feed_events_checked": len(updates)}


def strip(reply):
    return {k: v for k, v in reply.items() if k != "seq_ref"}


MALFORMED = [  # well-framed messages that must each be answered INVALID_REQUEST
    b"{not json",
    b"[1,2,3]",
    b'{"protocol_version":1,"type":"teleport","request_id":"x"}',
    b'{"protocol_version":2,"type":"subscribe"}',
    b'{"protocol_version":1,"type":"new_limit","request_id":"x","request_id":"y",'
    b'"symbol":"AAPL","side":"BUY","price":5,"quantity":1}',
    b'{"protocol_version":1,"type":"new_limit","request_id":"x","symbol":"AAPL","side":"BUY",'
    b'"price":5,"quantity":1,"extra":true}',
    b'{"protocol_version":1,"type":"cancel","request_id":""}',
    b"{" * 5000 + b"}" * 5000,
]


def chaos(port, stop, seed, counts, violations):
    """Hostile connections until `stop` is set; never a valid mutating command, so the
    honest traffic's linearizability check stays exact. Attacks: raw garbage, impossible frame
    lengths, half-sent frames then a drop, malformed JSON (each must get INVALID_REQUEST and
    leave the connection open), a subscriber that never reads, a snapshot then an abrupt close,
    and connect-then-close."""
    import socket
    import struct
    rng = random.Random(seed)

    def frame(b):
        return struct.pack(">I", len(b)) + b

    def read_frame(s):
        head = b""
        while len(head) < 4:
            chunk = s.recv(4 - len(head))
            if not chunk:
                return None
            head += chunk
        n = struct.unpack(">I", head)[0]
        body = b""
        while len(body) < n:
            chunk = s.recv(min(65536, n - len(body)))
            if not chunk:
                return None
            body += chunk
        return json.loads(body)

    def must_answer(s, data):
        """Sends and reads one reply on a healthy connection: no reply (timeout, reset, EOF)
        is reported as a string, so it becomes a violation rather than a 'connection error'.
        A SERVER_OVERLOADED refusal at the connection limit is legitimate."""
        try:
            s.sendall(data)
            reply = read_frame(s)
        except OSError as e:
            return f"no reply ({type(e).__name__})"
        if reply is None:
            return "no reply (connection closed)"
        if reply.get("code") == "SERVER_OVERLOADED":
            return "overloaded"
        return reply

    while not stop.is_set():
        kind = rng.choice(["garbage", "bad_length", "half_frame", "malformed", "slow_subscriber",
                           "snapshot_drop", "connect_close"])
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
                if kind == "garbage":
                    s.sendall(rng.randbytes(rng.randint(1, 3000)))
                elif kind == "bad_length":
                    s.sendall(struct.pack(">I", rng.choice([0, 32769, 0xFFFFFFFF])) + b"{}")
                    reply = read_frame(s)  # an error, then the server closes
                    if reply is not None and reply.get("code") != "INVALID_REQUEST":
                        violations.append(f"bad_length got {reply}")
                elif kind == "half_frame":
                    body = rng.choice(MALFORMED)
                    s.sendall(struct.pack(">I", len(body) + 10) + body[:rng.randint(0, len(body))])
                    time.sleep(rng.random() * 0.05)
                elif kind == "malformed":
                    for _ in range(rng.randint(1, 5)):  # several on one connection: it stays open
                        body = rng.choice(MALFORMED)
                        reply = must_answer(s, frame(body))
                        if reply == "overloaded":
                            break
                        if not isinstance(reply, dict) or reply.get("type") != "error" or \
                                reply.get("code") != "INVALID_REQUEST":
                            violations.append(f"malformed {body[:60]!r} got {reply}")
                            break
                elif kind == "slow_subscriber":
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
                    s.sendall(frame(b'{"protocol_version":1,"type":"subscribe","request_id":"s"}'))
                    time.sleep(rng.random() * 0.5)  # never reads: may be cut off as slow
                elif kind == "snapshot_drop":
                    request = frame(b'{"protocol_version":1,"type":"snapshot_request",'
                                    b'"symbol":"AAPL","request_id":"q"}')
                    if rng.random() < 0.5:
                        s.sendall(request)  # and drop the connection before the reply
                    else:
                        reply = must_answer(s, request)
                        if reply != "overloaded" and (not isinstance(reply, dict) or
                                                      reply.get("type") != "snapshot_response"):
                            violations.append(f"snapshot got {reply}")
            counts[kind] = counts.get(kind, 0) + 1
        except (OSError, ValueError) as e:
            # The server closing a hostile connection first is legitimate (reset, timeout).
            counts["connection_errors"] = counts.get("connection_errors", 0) + 1
            if isinstance(e, ValueError):
                violations.append(f"{kind}: unparseable reply ({e})")


def compare_concurrent(exe, seed, n, clients, journal_dir, chaos_threads=0):
    """The workload split round-robin over `clients` concurrent `client submit` processes,
    with a subscriber connected first. The interleaving is the server's, so the check is a
    linearizability check against the model:
      * accepted (non-duplicate) replies carry command sequences 1..S, increasing within each
        client (FIFO per connection), and the model fed those commands in sequence order
        reproduces every one of them exactly (order ids, executions, remaining, status);
      * every other reply (rejection or duplicate replay) must equal the model's reply at some
        state between that client's previous and next accepted commands;
      * the feed carries exactly the model's events, and the final books and watermarks agree.
    """
    rng = random.Random(seed)
    cmds = workload(rng, n)
    parts = [cmds[j::clients] for j in range(clients)]
    serve = [exe, "serve", "--port", "0"]
    jpath = str(pathlib.Path(journal_dir) / f"conc{seed}.exj") if journal_dir else None
    if jpath:
        serve += ["--journal", jpath, "--durability", "none"]
    srv = subprocess.Popen(serve, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    sub, procs, paths, feed = None, [], [], []
    attacks, violations = {}, []
    import threading
    try:
        port = json.loads(srv.stdout.readline())["port"]
        sub = subprocess.Popen([exe, "client", "subscribe", "--port", str(port)],
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        snap = json.loads(sub.stdout.readline())
        if snap.get("type") != "snapshot_response" or not snap.get("subscribed") or \
                snap["event_watermark"] != 0:
            sys.exit(f"concurrent seed {seed}: bad subscribe snapshot {json.dumps(snap)[:200]}")

        def read_feed():
            for line in sub.stdout:
                if line.strip():
                    feed.append(json.loads(line))
        reader = threading.Thread(target=read_feed, daemon=True)
        reader.start()
        for part in parts:
            with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
                for c in part:
                    f.write(json.dumps(c) + "\n")
                paths.append(f.name)
        stop_chaos = threading.Event()
        attackers = [threading.Thread(target=chaos, args=(port, stop_chaos, seed * 1000 + k,
                                                          attacks, violations), daemon=True)
                     for k in range(chaos_threads)]
        for t in attackers:
            t.start()
        for p in paths:  # appended one by one, so a failed start still cleans up the others
            procs.append(subprocess.Popen([exe, "client", "submit", "--port", str(port),
                                           "--input", p], stdout=subprocess.PIPE,
                                          stderr=subprocess.DEVNULL, text=True))
        outs = [p.communicate(timeout=300)[0] for p in procs]
        stop_chaos.set()
        for t in attackers:
            t.join(timeout=30)
        if violations:
            sys.exit(f"concurrent seed {seed}: hostile connections saw {len(violations)} "
                     f"protocol violations, e.g. {violations[0]}")
        if srv.poll() is not None:
            sys.exit(f"concurrent seed {seed}: the server exited ({srv.returncode}) under load")
        views = [[reply_view(r) for r in replies_of(out, f"concurrent seed {seed}, client {j}")]
                 for j, out in enumerate(outs)]
        accepted = {}  # command sequence -> (command, reply)
        owner = {}     # command sequence -> client
        for j, (part, vs) in enumerate(zip(parts, views)):
            if len(vs) != len(part):
                sys.exit(f"concurrent seed {seed}: client {j} got {len(vs)} replies for {len(part)}")
            last = 0
            for c, v in zip(part, vs):
                if v["status"] == "accepted" and not v["duplicate"]:
                    s = v["command_sequence"]
                    if s in accepted:
                        sys.exit(f"concurrent seed {seed}: sequence {s} given to client "
                                 f"{owner[s]} and to client {j}")
                    if s <= last:
                        sys.exit(f"concurrent seed {seed}: client {j} got sequence {s} after "
                                 f"{last} (per-connection order broken)")
                    last = s
                    accepted[s] = (c, v)
                    owner[s] = j
        total = len(accepted)
        if sorted(accepted) != list(range(1, total + 1)):
            sys.exit(f"concurrent seed {seed}: accepted sequences are not 1..{total}")
        deadline = time.monotonic() + 60
        while sum(1 for u in feed if u.get("type") == "book_update") < total and \
                time.monotonic() < deadline:
            time.sleep(0.05)
    finally:
        for p in paths:
            pathlib.Path(p).unlink(missing_ok=True)
        for p in procs:
            stop(p, read=False, timeout=5)
        rest = stop(srv)
        stop(sub, read=False, timeout=30)  # its stdout belongs to the reader thread
    # Windows for the replies that did not take a sequence number.
    pending = {}  # first state index -> [(last state index, client, command, reply)]
    for j, (part, vs) in enumerate(zip(parts, views)):
        seqs = [v["command_sequence"] if v["status"] == "accepted" and not v["duplicate"] else None
                for v in vs]
        for i, (c, v) in enumerate(zip(part, vs)):
            if seqs[i] is not None:
                continue
            lo = max([s for s in seqs[:i] if s is not None], default=0)
            hi = min([s for s in seqs[i + 1:] if s is not None], default=total + 1)
            pending.setdefault(lo, []).append((hi - 1, j, c, v))
    model, active, checked = Model(), [], 0
    for state in range(total + 1):  # `state` accepted commands applied so far
        active += pending.get(state, [])
        still = []
        for last, j, c, v in active:
            r = model.precheck(c)
            if r is not None and strip(r) == v:
                checked += 1
            elif state == last:
                sys.exit(f"concurrent seed {seed}: client {j} command {json.dumps(c)} got "
                         f"{json.dumps(v)}, which the model gives in no state it could have run in")
            else:
                still.append((last, j, c, v))
        active = still
        if state < total:
            c, v = accepted[state + 1]
            r = model.process(c)
            if strip(r) != v:
                sys.exit(f"concurrent seed {seed}, sequence {state + 1}: command {json.dumps(c)}\n"
                         f"  python: {json.dumps(strip(r))}\n  server: {json.dumps(v)}")
    updates = [u for u in feed if u.get("type") == "book_update"]
    if len(updates) != len(model.events):
        sys.exit(f"concurrent seed {seed}: {len(updates)} book_updates, model has {len(model.events)}")
    for want, have in zip(model.events, updates):
        w = dict(want, levels=sorted(want["levels"], key=lambda l: (l["side"], l["price"])))
        if event_view(have) != w:
            sys.exit(f"concurrent seed {seed}, event {want['event_sequence']} differs")
    final = [json.loads(l) for l in rest.splitlines() if l.startswith("{")][-1]
    if final.get("type") != "shutdown":
        sys.exit(f"concurrent seed {seed}: no shutdown summary")
    for b in final["books"]:
        for side, key in (("BUY", "bids"), ("SELL", "asks")):
            if b[key] != model.levels(b["symbol"], side):
                sys.exit(f"concurrent seed {seed}: final {b['symbol']} {key} differ")
    if final["last_command_sequence"] != model.seq or final["last_execution_sequence"] != model.exe:
        sys.exit(f"concurrent seed {seed}: watermarks differ")
    if jpath:  # what was made durable replays to exactly the state the server ended with
        r = subprocess.run([exe, "replay", "--journal", jpath, "--verify"], capture_output=True,
                           text=True, timeout=300)
        if r.returncode != 0 or json.loads(r.stdout)["state_hash"] != final.get("state_hash"):
            sys.exit(f"concurrent seed {seed}: journal replay (exit {r.returncode}) does not "
                     f"reproduce the server's final state hash")
    # Evidence that the clients really interleaved: consecutive sequences from different clients.
    switches = sum(1 for s in range(2, total + 1) if owner[s] != owner[s - 1])
    stats = {"accepted": total, "other_replies_linearized": checked, "executions": model.exe,
             "feed_events_checked": len(updates), "client_switches": switches}
    if chaos_threads:
        stats["hostile_connections"] = sum(attacks.values())
        for k, v in attacks.items():
            stats[f"hostile_{k}"] = v
    return stats


def main():
    args = sys.argv[1:]
    restart = "--restart" in args  # implies --network --journal
    network = "--network" in args or restart
    journal = "--journal" in args or restart
    args = [a for a in args if a not in ("--network", "--journal", "--restart")]
    opts = {"--seeds": "20", "--commands": "3000", "--enginex": str(ROOT / "build" / "release" / "enginex"),
            "--price-band": "9950:10050", "--max-qty": "100", "--clients": "0", "--chaos": "0"}
    for k in list(opts):
        if k in args:
            i = args.index(k); opts[k] = args[i + 1]; del args[i:i + 2]
    global PRICE_BAND, MAX_QTY
    try:
        lo, hi = (int(x) for x in opts["--price-band"].split(":"))
        PRICE_BAND, MAX_QTY = (lo, hi), int(opts["--max-qty"])
    except ValueError:
        sys.exit("--price-band takes LO:HI (integer ticks) and --max-qty an integer")
    if not 1 <= lo <= hi or MAX_QTY < 1:
        sys.exit("--price-band needs 1 <= LO <= HI and --max-qty >= 1")
    clients = int(opts["--clients"])
    hostile = int(opts["--chaos"])
    if clients < 0 or clients > 40 or (clients and restart):
        sys.exit("--clients takes 1..40 concurrent clients (not combined with --restart)")
    if hostile < 0 or hostile > 16 or (hostile and not clients):
        sys.exit("--chaos takes 1..16 hostile connection threads and needs --clients")
    total = {}
    with tempfile.TemporaryDirectory() as jdir:
        for seed in range(1, int(opts["--seeds"]) + 1):
            if clients:
                stats = compare_concurrent(opts["--enginex"], seed, int(opts["--commands"]),
                                           clients, jdir if journal else None, hostile)
            elif network:
                stats = compare_network(opts["--enginex"], seed, int(opts["--commands"]),
                                        jdir if journal else None, restart)
            else:
                stats = compare(opts["--enginex"], seed, int(opts["--commands"]))
            for k, v in stats.items():
                total[k] = total.get(k, 0) + v
    mode = (f"from {clients} concurrent TCP clients{' (journaled)' if journal else ''}"
            f"{f' alongside {hostile} hostile connection threads' if hostile else ''}, "
            "linearized against the model" if clients else
            "over TCP, journaled, restarted and resent" if restart else
            "over TCP (journaled)" if network and journal else
            "over TCP" if network else "via simulate")
    print(f"exchange_pymodel: {opts['--seeds']} seeds x {opts['--commands']} commands {mode} agree "
          f"line by line and in final books ({json.dumps(total)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
