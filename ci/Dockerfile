# Linux toolchain image mirroring the GitHub Actions ubuntu-24.04 jobs. Used by
# scripts/linux_check.sh to run the CI matrix locally (GCC, Clang, ASan/UBSan, TSan).
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++ clang libclang-rt-18-dev cmake ninja-build python3 python3-venv git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
