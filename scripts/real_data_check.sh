#!/usr/bin/env bash
# Reproduces the real-data validation in docs/itch.md § Real Nasdaq sample.
#
#   scripts/real_data_check.sh [BUILD_DIR] [DATE ...]   (default build/release; all dates below)
#
# For each DATE (01302019, 01302020, 03272019), downloads ONLY the first 256 MiB of Nasdaq's
# publicly posted sample file DATE.NASDAQ_ITCH50.gz (several GB each) with an HTTP range
# request, checks its SHA-256, decompresses the valid prefix, and runs: a strict replay, the
# reference-model differential test, the independent Python decoder and book, and the
# in-memory benchmark. Data stay under
# build/realdata/ (git-ignored) and are not redistributed. Never run by CI or ctest: it needs
# network access and ~1 GB of disk per date.
set -euo pipefail

cd "$(dirname "$0")/.."
BUILD=${1:-build/release}
shift || true
DATES=("$@")
[ ${#DATES[@]} -gt 0 ] || DATES=(01302019 01302020 03272019)
[ -x "$BUILD/enginex" ] || { echo "build first: cmake --preset release && cmake --build --preset release"; exit 2; }
DIR=build/realdata
mkdir -p "$DIR"

expected_sha() {  # SHA-256 of the first 256 MiB, recorded when first downloaded
    case "$1" in
        01302019) echo e917b2fb0ebb3234a13e1837bae0fb73a89b14bbc9377a8cd0c3cdd9f4e56ea5 ;;
        01302020) echo b094ae72321e22b3f37f51eef32e2934a58a78e21c69701fe6d4382ea761b946 ;;
        03272019) echo 1935562fe0aef4ddcf4a980f1e7526063a59b8d51ca5a9dee26f3ea8154219e3 ;;
        *) echo "unknown date $1" >&2; exit 2 ;;
    esac
}

for DATE in "${DATES[@]}"; do
    GZ="$DIR/$DATE.prefix256.gz"
    ITCH="$DIR/$DATE.prefix256.itch"
    if [ "$DATE" = 01302019 ] && [ -f "$DIR/prefix256.gz" ] && [ ! -f "$GZ" ]; then
        cp "$DIR/prefix256.gz" "$GZ"  # file name used by earlier runs
    fi
    URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/$DATE.NASDAQ_ITCH50.gz"
    echo "######## $DATE"
    if [ ! -f "$GZ" ]; then
        echo "downloading the first 256 MiB of $URL"
        # --max-filesize: refuse to fetch the whole multi-GB file if a server ignores Range.
        # Download to a temporary name so an interrupted run never leaves a partial $GZ.
        curl -fsS -r 0-268435455 --max-filesize 268435456 -o "$GZ.part" "$URL"
        mv "$GZ.part" "$GZ"
    fi
    actual=$(shasum -a 256 "$GZ" | cut -d' ' -f1)
    if [ "$actual" != "$(expected_sha "$DATE")" ]; then
        echo "SHA-256 mismatch for $DATE: got $actual (the file may have changed)"
        exit 1
    fi
    if [ ! -f "$ITCH" ]; then
        # Decompress to a temporary name: a truncated $ITCH would otherwise be accepted later
        # (the strict check tolerates a cut-off final message by design).
        python3 - "$GZ" "$ITCH.tmp" <<'PY'
import sys, zlib
d = zlib.decompressobj(16 + zlib.MAX_WBITS)  # gzip; a truncated stream yields its valid prefix
with open(sys.argv[1], 'rb') as src, open(sys.argv[2], 'wb') as dst:
    while chunk := src.read(1 << 20):
        dst.write(d.decompress(chunk))
PY
        mv "$ITCH.tmp" "$ITCH"
    fi

    echo "== strict replay (the only expected stop is the cut-off final message)"
    set +e
    "$BUILD/enginex" itch replay --input "$ITCH" --strict --json --depth 1 > "$DIR/$DATE.replay.json"
    set -e
    python3 - "$DIR/$DATE.replay.json" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
anomalies = sum(r["anomalies"].values())
errors = sum(r["decode_errors"].values())
print(f"  frames {r['frames']}, decode errors {errors}, anomalies {anomalies}, "
      f"crossed books by trading state {r['crossed_books_by_trading_state']}, "
      f"stop: {r['stopped']}")
ok = errors == 0 and anomalies == 0 and r["frame_error"] in ("none", "truncated_tail")
sys.exit(0 if ok else 1)
PY

    echo "== reference-model differential (every order of every book)"
    if ! ENGINEX_ITCH_REAL_FILE="$ITCH" "$BUILD/tests/enginex_tests" \
            --gtest_filter='ItchRealData.*' > "$DIR/$DATE.differential.log" 2>&1; then
        cat "$DIR/$DATE.differential.log"
        exit 1
    fi
    grep -E "real-data differential|SKIPPED" "$DIR/$DATE.differential.log" || true
    if grep -q "SKIPPED" "$DIR/$DATE.differential.log"; then
        echo "differential test was skipped"
        exit 1
    fi

    echo "== independent Python decoder (every field of every frame) and book (every level)"
    python3 tools/itch_pydecode.py "$ITCH" --enginex "$BUILD/enginex"
    python3 tools/itch_pybook.py "$ITCH" --enginex "$BUILD/enginex"

    echo "== in-memory benchmark (decode, replay)"
    for m in itch_decode itch_replay; do
        "$BUILD/enginex_itch_bench" --input "$ITCH" --only "$m" --trials 3 > "$DIR/$DATE.$m.log"
        grep -E "^\[|trials median" "$DIR/$DATE.$m.log" || true
    done
done
