#!/usr/bin/env bash
# Reproduces the real-data validation in docs/itch.md § Real Nasdaq sample.
#
#   scripts/real_data_check.sh [BUILD_DIR]     (default build/release; build it first)
#
# Downloads ONLY the first 256 MiB of Nasdaq's publicly posted sample file
# 01302019.NASDAQ_ITCH50.gz (4.76 GB in total) with an HTTP range request, checks its SHA-256,
# decompresses the valid prefix, and runs: a strict replay, the reference-model differential
# test, and the in-memory benchmark. Data stay under build/realdata/ (git-ignored) and are not
# redistributed. Never run by CI or ctest: it needs network access and ~1 GB of disk.
set -euo pipefail

cd "$(dirname "$0")/.."
BUILD=${1:-build/release}
[ -x "$BUILD/enginex" ] || { echo "build first: cmake --preset release && cmake --build --preset release"; exit 2; }
URL='https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz'
EXPECTED_SHA256=e917b2fb0ebb3234a13e1837bae0fb73a89b14bbc9377a8cd0c3cdd9f4e56ea5
DIR=build/realdata
mkdir -p "$DIR"

if [ ! -f "$DIR/prefix256.gz" ]; then
    echo "downloading the first 256 MiB of $URL"
    curl -fsS -r 0-268435455 -o "$DIR/prefix256.gz" "$URL"
fi
actual=$(shasum -a 256 "$DIR/prefix256.gz" | cut -d' ' -f1)
if [ "$actual" != "$EXPECTED_SHA256" ]; then
    echo "SHA-256 mismatch: got $actual, expected $EXPECTED_SHA256 (the file may have changed)"
    exit 1
fi
if [ ! -f "$DIR/prefix256.itch" ]; then
    python3 - "$DIR/prefix256.gz" "$DIR/prefix256.itch" <<'EOF'
import sys, zlib
d = zlib.decompressobj(16 + zlib.MAX_WBITS)  # gzip; a truncated stream yields its valid prefix
with open(sys.argv[1], 'rb') as src, open(sys.argv[2], 'wb') as dst:
    while chunk := src.read(1 << 20):
        dst.write(d.decompress(chunk))
EOF
fi

echo "== strict replay (the only expected stop is the cut-off final message)"
set +e
"$BUILD/enginex" itch replay --input "$DIR/prefix256.itch" --strict --json --depth 1 > "$DIR/replay.json"
set -e
python3 - "$DIR/replay.json" <<'EOF'
import json, sys
r = json.load(open(sys.argv[1]))
anomalies = sum(r["anomalies"].values())
errors = sum(r["decode_errors"].values())
print(f"  frames {r['frames']}, decode errors {errors}, anomalies {anomalies}, "
      f"stop: {r['stopped']}")
ok = errors == 0 and anomalies == 0 and r["frame_error"] in ("none", "truncated_tail")
sys.exit(0 if ok else 1)
EOF

echo "== reference-model differential (every order of every book)"
ENGINEX_ITCH_REAL_FILE="$DIR/prefix256.itch" "$BUILD/tests/enginex_tests" \
    --gtest_filter='ItchRealData.*' | grep -E "real-data differential|OK|FAILED"

echo "== in-memory benchmark (decode, replay)"
for m in itch_decode itch_replay; do
    "$BUILD/enginex_itch_bench" --input "$DIR/prefix256.itch" --only "$m" --trials 3 |
        grep -E "^\[|trials median"
done
