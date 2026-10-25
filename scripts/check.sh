#!/usr/bin/env bash
# Local quality gate.
#
#   scripts/check.sh smoke BUILD_DIR   CLI smoke tests against an existing build
#   scripts/check.sh format            clang-format check (no changes made)
#   scripts/check.sh all               configure+build+test debug and release, smoke, format
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD

smoke() {
    local build=$1
    local bin="$build/enginex"
    local tmp
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' RETURN

    echo "--- smoke: simulate demo matches expected output"
    "$bin" simulate --input examples/demo_orders.jsonl --output "$tmp/demo.json" 2>/dev/null
    diff -u examples/expected_demo_results.json "$tmp/demo.json"

    echo "--- smoke: simulate from stdin"
    "$bin" simulate --input - --compact < examples/demo_orders.jsonl | python3 -c \
        'import json,sys; d=json.load(sys.stdin); assert d["final_state"]["books"][0]["bids"]==[{"price":10100,"quantity":4,"order_count":1}]'

    echo "--- smoke: usage errors exit 2"
    set +e
    "$bin" simulate --bogus >/dev/null 2>&1; [ $? -eq 2 ] || { echo "expected exit 2"; exit 1; }
    "$bin" benchmark --scenario nope >/dev/null 2>&1; [ $? -eq 2 ] || { echo "expected exit 2"; exit 1; }
    set -e

    echo "--- smoke: short seeded benchmark for every scenario"
    for s in no-cross high-cross same-price-burst multi-level-sweep cancel-heavy mixed; do
        "$bin" benchmark --scenario "$s" --seed 42 --orders 20000 --trials 2 --warmup 1 \
            --out "$tmp/bench.csv" >/dev/null
    done
    test "$(wc -l < "$tmp/bench.csv")" -eq $((1 + 6 * 3))
    if [ -x "$build/enginex_bench" ]; then
        "$build/enginex_bench" --orders 2000 --trials 1 --scenario mixed >/dev/null
    fi
    if declare -F smoke_r2 >/dev/null; then smoke_r2 "$bin" "$tmp"; fi
    if declare -F smoke_r3 >/dev/null; then smoke_r3 "$bin" "$tmp"; fi
    echo "smoke OK"
}

smoke_r2() {
    local bin=$1 tmp=$2
    echo "--- smoke (R2): journaled run, restart, replay --verify"
    "$bin" run --journal "$tmp/demo.exj" --input examples/demo_orders.jsonl > "$tmp/run1.jsonl"
    "$bin" run --journal "$tmp/demo.exj" --input /dev/null > "$tmp/run2.jsonl"
    "$bin" replay --journal "$tmp/demo.exj" --verify > "$tmp/replay.json"
    python3 - "$tmp" <<'PY'
import json, sys
tmp = sys.argv[1]
sim = json.load(open(f"{tmp}/demo.json"))["final_state"]["state_hash"]
run1 = [json.loads(l) for l in open(f"{tmp}/run1.jsonl")]
run2 = [json.loads(l) for l in open(f"{tmp}/run2.jsonl")]
rep = json.load(open(f"{tmp}/replay.json"))
assert run1[0]["type"] == "recovery" and run1[0]["created"], run1[0]
assert run1[-1]["state_hash"] == sim, "journaled run differs from simulate"
assert run2[0]["records_replayed"] == 7 and run2[0]["state_hash"] == sim, run2[0]
assert rep["status"] == "ok" and rep["state_hash"] == sim and rep["records"] == 7, rep
PY
    echo "--- smoke (R2): kill -9 and corruption harness"
    python3 tools/fault_injection.py --enginex "$bin" --rounds 4 --seed 3 >/dev/null
    echo "--- smoke (R2): durable and recovery benchmark modes"
    "$bin" benchmark --scenario mixed --mode durable --durability none --orders 2000 --trials 1 \
        --journal-dir "$tmp" --out "$tmp/r2.csv" >/dev/null
    "$bin" benchmark --scenario mixed --mode recovery --orders 20000 --trials 1 \
        --journal-dir "$tmp" --out "$tmp/r2.csv" >/dev/null
    test "$(grep -c durable_ack_none "$tmp/r2.csv")" -eq 2
    test "$(grep -c recovery_replay "$tmp/r2.csv")" -eq 2
}

format_check() {
    local cf=${CLANG_FORMAT:-clang-format}
    if ! command -v "$cf" >/dev/null 2>&1; then
        echo "clang-format not found (set CLANG_FORMAT); skipping format check" >&2
        return 1
    fi
    git ls-files '*.cpp' '*.hpp' | xargs "$cf" --dry-run --Werror
    echo "format OK"
}

case "${1:-all}" in
    smoke) smoke "${2:?build dir}" ;;
    format) format_check ;;
    all)
        for preset in debug release; do
            cmake --preset "$preset" >/dev/null
            cmake --build --preset "$preset"
            ctest --preset "$preset" -j 8
        done
        smoke build/release
        format_check
        ;;
    *) echo "usage: $0 [smoke BUILD_DIR | format | all]" >&2; exit 2 ;;
esac
