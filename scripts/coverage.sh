#!/usr/bin/env bash
# Source-based coverage (clang -fprofile-instr-generate) of the whole ctest suite, reported
# for the ITCH path, the feed mirror and the server. Usage: scripts/coverage.sh [build-dir]
# Reuses already-downloaded dependencies from build/release when present (no network needed).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build/coverage}"
deps=()
if [[ -d "$root/build/release/_deps/nlohmann_json-src" ]]; then
    deps=(-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON="$root/build/release/_deps/nlohmann_json-src"
          -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST="$root/build/release/_deps/googletest-src")
fi
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
    -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate" "${deps[@]}" >/dev/null
cmake --build "$build"
prof="$build/profiles-$$"
mkdir -p "$prof"
(cd "$build" && LLVM_PROFILE_FILE="$prof/%p.profraw" ctest --output-on-failure)
profdata=$(command -v llvm-profdata || xcrun --find llvm-profdata)
cov=$(command -v llvm-cov || xcrun --find llvm-cov)
"$profdata" merge -sparse "$prof"/*.profraw -o "$build/coverage.profdata"
src="$(cd "$build" && grep -m1 'CMAKE_HOME_DIRECTORY' CMakeCache.txt | cut -d= -f2)"
"$cov" report "$build/tests/enginex_tests" -object "$build/enginex" \
    -instr-profile="$build/coverage.profdata" \
    "$src/src/itch" "$src/include/enginex/itch" "$src/src/mirror.cpp" "$src/src/cli_itch.cpp" \
    "$src/src/server.cpp"
