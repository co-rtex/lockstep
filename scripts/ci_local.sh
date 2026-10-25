#!/usr/bin/env bash
# Runs the real .github/workflows/ci.yml jobs locally with `act` (https://nektosact.com) in
# Docker, using the catthehacker ubuntu-24.04 runner image. This executes the workflow file
# as written; it is not the GitHub-hosted runner itself.
#
#   scripts/ci_local.sh [job ...]   jobs: gcc-debug clang-asan-ubsan clang-tsan release format
set -euo pipefail
cd "$(dirname "$0")/.."
command -v act >/dev/null || { echo "install act first (e.g. brew install act)"; exit 2; }
JOBS=("$@")
[ ${#JOBS[@]} -eq 0 ] && JOBS=(gcc-debug clang-asan-ubsan clang-tsan release format)
ARCH=linux/amd64
[ "$(uname -m)" = arm64 ] || [ "$(uname -m)" = aarch64 ] && ARCH=linux/arm64
ARTIFACTS=$(mktemp -d)
trap 'rm -rf "$ARTIFACTS"' EXIT
for job in "${JOBS[@]}"; do
    echo "=== act job: $job"
    rc=0
    act -j "$job" -P ubuntu-24.04=ghcr.io/catthehacker/ubuntu:act-24.04 \
        --container-architecture "$ARCH" --artifact-server-path "$ARTIFACTS" \
        > "$ARTIFACTS/$job.log" 2>&1 || rc=$?
    grep -E "tests passed|smoke OK|format OK|Job succeeded|Job failed|❌" "$ARTIFACTS/$job.log" || true
    if [ "$rc" -ne 0 ]; then
        echo "job $job failed (exit $rc); full log follows"; cat "$ARTIFACTS/$job.log"; exit "$rc"
    fi
done
echo "all requested workflow jobs succeeded under act"
