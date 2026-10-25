#!/usr/bin/env bash
# Reviewer walkthrough of the spec §15 flows (R1 → R2 → R3), using a temporary directory.
#
#   scripts/demo.sh [BUILD_DIR]      (default build/release; build it first)
set -euo pipefail

cd "$(dirname "$0")/.."
BIN=${1:-build/release}/enginex
[ -x "$BIN" ] || { echo "build first: cmake --preset release && cmake --build --preset release"; exit 2; }
TMP=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$TMP"' EXIT
step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
report() { python3 tools/demo_report.py "$@"; }

step "R1: deterministic matching (examples/demo_orders.jsonl)"
"$BIN" simulate --input examples/demo_orders.jsonl --output "$TMP/demo.json" 2>/dev/null
report simulate "$TMP/demo.json"

step "R1: seeded benchmark (core, in-process; 100k commands)"
"$BIN" benchmark --scenario mixed --seed 42 --orders 100000 --trials 3 | sed 's/^/  /'

step "R2: journaled session killed mid-stream (crash injected after command 5 is synced)"
set +e
ENGINEX_FAULT=crash=after_sync@5 "$BIN" run --journal "$TMP/demo.exj" \
    --input examples/demo_orders.jsonl > "$TMP/run1.jsonl" 2>/dev/null
echo "  process exit code: $? (86 = injected crash); replies printed before the crash: $(($(wc -l < "$TMP/run1.jsonl") - 1))"
set -e
"$BIN" replay --journal "$TMP/demo.exj" --verify > "$TMP/r.json"
report replay "$TMP/r.json"

step "R2: restart and retry everything with the same request ids (no duplicate execution)"
"$BIN" run --journal "$TMP/demo.exj" --input examples/demo_orders.jsonl > "$TMP/run2.jsonl"
report retry "$TMP/run2.jsonl" "$TMP/demo.json"

step "R2: corruption is refused, a truncated tail is repaired"
cp "$TMP/demo.exj" "$TMP/corrupt.exj"
python3 -c 'import sys; p=sys.argv[1]; b=bytearray(open(p,"rb").read()); b[len(b)//2]^=0x20; open(p,"wb").write(b)' "$TMP/corrupt.exj"
set +e
"$BIN" replay --journal "$TMP/corrupt.exj" --verify > "$TMP/c.json"; echo "  flipped byte -> exit $? (1 = corrupt)"
report replay "$TMP/c.json"
cp "$TMP/demo.exj" "$TMP/trunc.exj"; python3 -c 'import sys,os; p=sys.argv[1]; os.truncate(p, os.path.getsize(p)-3)' "$TMP/trunc.exj"
"$BIN" replay --journal "$TMP/trunc.exj" --verify > /dev/null 2>&1; echo "  truncated tail -> exit $? (3 = incomplete tail)"
set -e
"$BIN" replay --journal "$TMP/trunc.exj" --repair-tail > "$TMP/t.json"
report replay "$TMP/t.json"

step "R3: TCP server, subscriber, order entry"
"$BIN" serve --port 0 --journal "$TMP/net.exj" > "$TMP/serve.out" 2>/dev/null &
SERVER=$!
for _ in $(seq 1 100); do grep -q listening "$TMP/serve.out" 2>/dev/null && break; sleep 0.05; done
PORT=$(report port "$TMP/serve.out")
echo "  server listening on 127.0.0.1:$PORT"
"$BIN" client subscribe --port "$PORT" --count 7 > "$TMP/sub.out" &
SUB=$!
sleep 0.3
"$BIN" client submit --port "$PORT" --input examples/demo_orders.jsonl > "$TMP/submit.out" || true
wait $SUB
report subscriber "$TMP/sub.out"

step "R3: slow consumer is cut off with resync_required; trading continues"
"$BIN" client subscribe --port "$PORT" --slow-ms 1000 --quiet > "$TMP/slow.out" &
SLOW=$!
sleep 0.2
python3 - "$PORT" <<'PY'
import json, socket, struct, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
buf = b""
def req(m):
    global buf
    d = json.dumps({"protocol_version": 1, **m}).encode()
    s.sendall(struct.pack(">I", len(d)) + d)
    while True:
        while len(buf) < 4 or len(buf) < 4 + struct.unpack(">I", buf[:4])[0]:
            buf += s.recv(65536)
        n = struct.unpack(">I", buf[:4])[0]; msg = json.loads(buf[4:4+n]); buf = buf[4+n:]
        if msg["type"] != "book_update": return msg
for i in range(5000):
    r = req({"type": "new_limit", "request_id": f"flood{i}", "symbol": "AAPL",
             "side": "BUY" if i % 2 else "SELL", "price": 9000 + i % 1500, "quantity": 1})
    assert r["type"] == "ack"
print("  trader: 5000 orders acknowledged while the slow subscriber fell behind")
PY
kill $SLOW 2>/dev/null || true; wait $SLOW 2>/dev/null || true
echo "  (the shutdown statistics below show how the slow subscriber was cut off)"

step "R3: graceful shutdown (SIGTERM) and journal check"
kill -TERM $SERVER; wait $SERVER
report shutdown "$TMP/serve.out"
"$BIN" replay --journal "$TMP/net.exj" --verify > "$TMP/n.json"
report replay "$TMP/n.json"
echo
echo "demo complete"
