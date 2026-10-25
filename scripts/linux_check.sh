#!/usr/bin/env bash
# Runs the Linux CI matrix inside Docker (ubuntu:24.04). The source tree is mounted read-only
# and every build lives inside the container, so macOS build directories are untouched.
#
# Usage: scripts/linux_check.sh [job ...]
#   jobs: gcc-debug clang-asan clang-tsan release   (default: all)
set -euo pipefail

cd "$(dirname "$0")/.."
IMAGE=enginex-ci:ubuntu24.04
docker build -q -t "$IMAGE" -f ci/Dockerfile ci >/dev/null

JOBS=("$@")
if [ ${#JOBS[@]} -eq 0 ]; then
    JOBS=(gcc-debug clang-asan clang-tsan release)
fi

for job in "${JOBS[@]}"; do
    echo "=== linux job: $job ==="
    docker run --rm -v "$PWD:/src:ro" -e JOB="$job" "$IMAGE" bash -euo pipefail -c '
        mkdir -p /work
        # Copy the working tree without .git and local build output (host-owned git objects
        # may not be readable through the bind mount); fail hard if the copy fails.
        (cd /src && tar --exclude=./.git --exclude=./build --exclude=./data -cf - .) | tar -xf - -C /work
        cd /work
        case "$JOB" in
          gcc-debug)  CXX=g++     ARGS="-DCMAKE_BUILD_TYPE=Debug" ;;
          clang-asan) CXX=clang++ ARGS="-DCMAKE_BUILD_TYPE=Debug -DENGINEX_SANITIZER=address,undefined" ;;
          clang-tsan) CXX=clang++ ARGS="-DCMAKE_BUILD_TYPE=RelWithDebInfo -DENGINEX_SANITIZER=thread" ;;
          release)    CXX=g++     ARGS="-DCMAKE_BUILD_TYPE=Release" ;;
          *) echo "unknown job $JOB"; exit 2 ;;
        esac
        export CXX
        $CXX --version | head -1
        cmake -S . -B build -G Ninja -DENGINEX_ENABLE_TESTS=ON $ARGS >/dev/null
        cmake --build build --parallel
        export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1
        export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
        export TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1
        ctest --test-dir build --output-on-failure -j "$(nproc)"
        if [ "$JOB" = release ]; then
            scripts/check.sh smoke build
        fi
    '
done
