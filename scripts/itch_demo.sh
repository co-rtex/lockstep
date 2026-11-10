#!/usr/bin/env bash
# One-command walkthrough of the ITCH 5.0 market-data path (independent of the exchange).
# Everything here is synthetic: hand-scripted fixtures and seeded generated streams built
# from the public specification. No Nasdaq data is used.
#
#   scripts/itch_demo.sh [BUILD_DIR]      (default build/release; build it first)
set -euo pipefail

cd "$(dirname "$0")/.."
BUILD=${1:-build/release}
BIN=$BUILD/enginex
[ -x "$BIN" ] || { echo "build first: cmake --preset release && cmake --build --preset release"; exit 2; }
TMP=$(mktemp -d "${TMPDIR:-/tmp}/enginex-itch-demo.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
hash_of() { python3 -c 'import json,sys; print(json.load(sys.stdin)["state_hash"])'; }

step "Golden fixture (bytes packed in Python from the spec offsets), strict replay"
"$BIN" itch replay --input tests/testdata/itch/lifecycle.itch --strict --depth 3
python3 tools/itch_fixture.py --check && echo "  fixture files match tools/itch_fixture.py"

step "First frames, decoded (one JSON line per frame)"
"$BIN" itch dump --input tests/testdata/itch/lifecycle.itch --limit 4 | sed 's/^/  /'

step "Malformed fixture: unknown type and wrong length are counted, truncated tail is fatal"
set +e
"$BIN" itch replay --input tests/testdata/itch/malformed.itch --json > "$TMP/bad.json"
code=$?
set -e
python3 - "$TMP/bad.json" "$code" <<'EOF'
import json, sys
r = json.load(open(sys.argv[1]))
print(f"  exit {sys.argv[2]}; frames {r['frames']}, decoded {r['decoded']}, "
      f"decode_errors {r['decode_errors']}, frame_error {r['frame_error']} "
      f"at byte {r.get('frame_error_offset')}")
EOF

step "Deterministic replay: generate a seeded stream, replay it twice"
"$BIN" itch generate --out "$TMP/s.itch" --seed 42 --messages 200000 --symbols 4 | sed 's/^/  /'
h1=$("$BIN" itch replay --input "$TMP/s.itch" --json | hash_of)
h2=$("$BIN" itch replay --input "$TMP/s.itch" --json --strict | hash_of)
echo "  state hash run 1: $h1"
echo "  state hash run 2: $h2"
[ "$h1" = "$h2" ] && echo "  identical"

step "Same stream with 2% injected anomalies: lenient counts them, --strict stops at the first"
"$BIN" itch generate --out "$TMP/a.itch" --seed 42 --messages 50000 --symbols 4 \
    --anomalies-per-million 20000 > /dev/null
"$BIN" itch replay --input "$TMP/a.itch" --json | python3 -c '
import json, sys; r = json.load(sys.stdin)
print("  lenient: anomalies", {k: v for k, v in r["anomalies"].items() if v})'
set +e
"$BIN" itch replay --input "$TMP/a.itch" --json --strict | python3 -c '
import json, sys; r = json.load(sys.stdin)
print("  strict: stopped at message", r["stopped_at_message"], "-", r["stopped"])'
set -e

if [ -x "$BUILD/enginex_itch_bench" ]; then
    step "In-process benchmark (short run; see docs/benchmarks.md for the recorded results)"
    "$BUILD/enginex_itch_bench" --messages 200000 --trials 3 --reserve 65536 | sed 's/^/  /'
fi
