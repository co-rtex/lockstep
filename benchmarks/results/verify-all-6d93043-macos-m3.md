# Verification run

* commit: 6d930439db6b
* started: 2026-11-11T22:02:19Z
* host: Darwin arm64, Apple clang version 17.0.0 (clang-1700.6.4.2)

| step | result | seconds | log |
|---|---|---:|---|
| release-build | pass | 0 | `build/verify/release-build.log` |
| release-ctest-1 | pass | 8 | `build/verify/release-ctest-1.log` |
| release-ctest-2 | pass | 9 | `build/verify/release-ctest-2.log` |
| release-ctest-3 | pass | 8 | `build/verify/release-ctest-3.log` |
| debug | pass | 17 | `build/verify/debug.log` |
| ubsan | pass | 36 | `build/verify/ubsan.log` |
| hardened | pass | 21 | `build/verify/hardened.log` |
| cli-smoke | pass | 3 | `build/verify/cli-smoke.log` |
| exchange-demo | pass | 20 | `build/verify/exchange-demo.log` |
| itch-demo | pass | 0 | `build/verify/itch-demo.log` |
| mold-udp-demo | pass | 2 | `build/verify/mold-udp-demo.log` |
| cli-arg-sweep | pass | 218 | `build/verify/cli-arg-sweep.log` |
| fuzz-200k | pass | 28 | `build/verify/fuzz-200k.log` |
| fault-injection | pass | 1 | `build/verify/fault-injection.log` |
| real-data | pass | 429 | `build/verify/real-data.log` |
| format | skipped: pass --format with CLANG_FORMAT set to clang-format 19.1.7 | | |
| mutation | pass | 757 | `build/verify/mutation.log` |
* finished: 2026-11-11T22:28:16Z, all run steps passed

Details from the step logs (`scripts/verify_all.sh --repeat 3 --real-data --mutation`, exit 0, 26 min):

* Release (three runs), Debug, UBSan, hardened libc++: 533/533 each (1 opt-in real-data
  ctest skips without its local file).
* Real data: 3 sample days (2019-01-30, 2019-03-27, 2020-01-30), strict
  replay, C++ reference-model differential and independent Python decoder/book all identical.
* Mutation: 69/69 killed (757 s).
* clang-format: not run (no clang-format 19.1.7 here). Linux CI (GCC, ASan, TSan): not run
  here; see `docs/linux-validation.md`.
