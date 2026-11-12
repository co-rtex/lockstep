#!/usr/bin/env bash
# MoldUDP64 over real UDP on loopback, as two processes: `itch mold-recv` rebuilds the books
# from what `itch mold-send` transmits, re-requesting whatever is lost. The sender drops
# packets deliberately on the live feed and on retransmissions. The receiver's book hash must
# equal a direct file replay.
#
#   scripts/mold_udp_demo.sh [BUILD_DIR] [ITCH_FILE]
#     defaults: build/release, a 200 000-message synthetic fixture
#   env: DROP_PERCENT (5), RETRANSMIT_DROP_PERCENT (10), RATE_PPS (100000),
#        RECV_PORT (47123), SEND_PORT (47124)
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build/release}
[ -x "$BUILD/enginex" ] || { echo "build first: cmake --preset release && cmake --build --preset release"; exit 2; }
WORK=$(mktemp -d "${TMPDIR:-/tmp}/enginex-mold-udp.XXXXXX")
FILE=${2:-}
if [ -z "$FILE" ]; then
    FILE="$WORK/fixture.itch"
    "$BUILD/enginex" itch generate --out "$FILE" --seed 5 --messages 200000 --symbols 8 > /dev/null
fi
RECV_PORT=${RECV_PORT:-47123}
SEND_PORT=${SEND_PORT:-47124}

echo "== direct replay of $FILE"
set +e
"$BUILD/enginex" itch replay --input "$FILE" --json --depth 1 > "$WORK/direct.json"
set -e
DIRECT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["state_hash"])' "$WORK/direct.json")
echo "  state hash $DIRECT"

echo "== receiver on 127.0.0.1:$RECV_PORT, sender on 127.0.0.1:$SEND_PORT"
"$BUILD/enginex" itch mold-recv --port "$RECV_PORT" --request-port "$SEND_PORT" \
    > "$WORK/recv.json" 2> "$WORK/recv.err" &
RECV_PID=$!
sleep 0.3
"$BUILD/enginex" itch mold-send --input "$FILE" --port "$SEND_PORT" --dest-port "$RECV_PORT" \
    --drop-percent "${DROP_PERCENT:-5}" --retransmit-drop-percent "${RETRANSMIT_DROP_PERCENT:-10}" \
    --rate-pps "${RATE_PPS:-100000}" --linger-ms 500 > "$WORK/send.json"
set +e
wait "$RECV_PID"
RECV_STATUS=$?
set -e
python3 - "$WORK/send.json" "$WORK/recv.json" "$DIRECT" "$RECV_STATUS" <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))
r = json.load(open(sys.argv[2]))
direct, status = sys.argv[3], int(sys.argv[4])
print(f"  sender:   {s['sent']} of {s['packets']} packets sent ({s['dropped_deliberately']} dropped), "
      f"{s['requests_answered']} re-requests answered, {s['retransmitted_packets']} retransmitted "
      f"({s['retransmit_dropped_deliberately']} of those dropped)")
print(f"  receiver: {r['final_state']}, {r['delivered']} messages in order={r['in_order_exactly_once']}, "
      f"{r['gaps_opened']} gaps, {r['requests_sent']} requests, {r['seconds']:.2f} s")
same = r["book_hash"] == direct
print(f"  book hash {r['book_hash']} {'==' if same else '!='} direct replay")
sys.exit(0 if same and status == 0 and r["final_state"] == "ended" else 1)
PY
echo "mold-udp demo OK"
