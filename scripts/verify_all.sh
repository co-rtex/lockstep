#!/usr/bin/env bash
# The whole local verification matrix in one command, for release checks and for reproducing
# the evidence in docs/verification.md. Every step runs even if an earlier one fails; each
# step's exit code and duration go to a summary table. Exit status is non-zero if any step
# failed. Skipped steps are listed as skipped, never as passed.
#
#   scripts/verify_all.sh [--repeat N] [--real-data] [--mutation] [--format]
#
#   --repeat N   run the Release suite N times (flakiness check; default 2)
#   --real-data  also run scripts/real_data_check.sh (needs the local Nasdaq sample files)
#   --mutation   also run tools/mutation_check.py, last, after every build has finished
#                (it writes mutants into the source tree; nothing else may build meanwhile)
#   --format     also run the clang-format check (needs CLANG_FORMAT=/path/to/clang-format 19.1.7)
#
# Build directories: build/release, build/debug, build/ubsan, build/hardened, each configured
# here if missing (dependencies reused from build/release when it has downloaded them).
# Summary: build/verify/summary.md, plus one log per step in build/verify/.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
REPEAT=2
REAL=0
MUTATION=0
FORMAT=0
while [ $# -gt 0 ]; do
    case "$1" in
        --repeat) REPEAT=${2:?--repeat needs N}; shift 2 ;;
        --real-data) REAL=1; shift ;;
        --mutation) MUTATION=1; shift ;;
        --format) FORMAT=1; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
OUT=build/verify
mkdir -p "$OUT"
SUMMARY=$OUT/summary.md
{
    echo "# Verification run"
    echo
    echo "* commit: $(git rev-parse --short=12 HEAD)$(git diff --quiet HEAD -- . ':!autopilot' || echo ' (working tree modified)')"
    echo "* started: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "* host: $(uname -sm), $(c++ --version 2>/dev/null | head -1)"
    echo
    echo "| step | result | seconds | log |"
    echo "|---|---|---:|---|"
} > "$SUMMARY"
FAILED=0

step() {  # step NAME COMMAND...
    local name=$1
    shift
    local log="$OUT/$name.log"
    local t0=$SECONDS
    echo "=== $name"
    "$@" > "$log" 2>&1
    local rc=$?
    local result="pass"
    if [ $rc -ne 0 ]; then
        result="**FAIL (exit $rc)**"
        FAILED=1
        tail -20 "$log"
    fi
    echo "| $name | $result | $((SECONDS - t0)) | \`$log\` |" >> "$SUMMARY"
}
skip() {  # skip NAME REASON
    echo "=== $1 (skipped: $2)"
    echo "| $1 | skipped: $2 | | |" >> "$SUMMARY"
}

DEPS=()
if [ -d build/release/_deps/googletest-src ]; then
    DEPS=(-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$ROOT/build/release/_deps/googletest-src"
          -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$ROOT/build/release/_deps/nlohmann_json-src")
fi
configure() {  # configure DIR CMAKE_ARGS...
    local dir=$1
    shift
    [ -f "$dir/CMakeCache.txt" ] || cmake -S . -B "$dir" -G Ninja -DENGINEX_ENABLE_TESTS=ON "$@" "${DEPS[@]}"
}
build_and_test() {  # build_and_test DIR (ctest output must report 100 %)
    cmake --build "$1" && ctest --test-dir "$1" -j8 --timeout 900 --output-on-failure
}

configure build/release -DCMAKE_BUILD_TYPE=Release > /dev/null
configure build/debug -DCMAKE_BUILD_TYPE=Debug > /dev/null
configure build/ubsan -DCMAKE_BUILD_TYPE=Debug -DENGINEX_SANITIZER=undefined > /dev/null
configure build/hardened -DCMAKE_BUILD_TYPE=Debug \
    "-DCMAKE_CXX_FLAGS=-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG -D_GLIBCXX_ASSERTIONS" > /dev/null

step release-build cmake --build build/release
for i in $(seq 1 "$REPEAT"); do
    step "release-ctest-$i" ctest --test-dir build/release -j8 --timeout 300 --output-on-failure
done
step debug build_and_test build/debug
step ubsan build_and_test build/ubsan
step hardened build_and_test build/hardened
step cli-smoke scripts/check.sh smoke build/release
step exchange-demo scripts/demo.sh build/release
step itch-demo scripts/itch_demo.sh build/release
step mold-udp-demo scripts/mold_udp_demo.sh build/release
step cli-arg-sweep python3 tools/cli_arg_sweep.py --enginex build/release/enginex
step fuzz-200k build/release/enginex_fuzz_itch_standalone 200000 1 \
    tests/testdata/itch/lifecycle.itch tests/testdata/itch/malformed.itch fuzz/corpus/mold_gaps.bin \
    fuzz/corpus/mold_packets.bin
step fault-injection python3 tools/fault_injection.py --enginex build/release/enginex
if [ "$REAL" = 1 ]; then
    step real-data scripts/real_data_check.sh build/release
else
    skip real-data "pass --real-data (needs the local Nasdaq sample files)"
fi
if [ "$FORMAT" = 1 ] && [ -n "${CLANG_FORMAT:-}" ]; then
    step format scripts/check.sh format
else
    skip format "pass --format with CLANG_FORMAT set to clang-format 19.1.7"
fi
if [ "$MUTATION" = 1 ]; then
    step mutation python3 tools/mutation_check.py build/release
else
    skip mutation "pass --mutation (about 6 minutes; nothing else may build meanwhile)"
fi
echo "* finished: $(date -u +%Y-%m-%dT%H:%M:%SZ), $([ $FAILED = 0 ] && echo 'all run steps passed' || echo 'SOME STEPS FAILED')" >> "$SUMMARY"
echo
cat "$SUMMARY"
exit $FAILED
