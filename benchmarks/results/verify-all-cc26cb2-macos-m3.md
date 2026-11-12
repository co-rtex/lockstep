# Verification run

* commit: cc26cb287cb0
* started: 2026-11-11T07:47:43Z
* host: Darwin arm64, Apple clang version 17.0.0 (clang-1700.6.4.2)

| step | result | seconds | log |
|---|---|---:|---|
| release-build | pass | 3 | `build/verify/release-build.log` |
| release-ctest-1 | pass | 6 | `build/verify/release-ctest-1.log` |
| release-ctest-2 | pass | 7 | `build/verify/release-ctest-2.log` |
| release-ctest-3 | pass | 6 | `build/verify/release-ctest-3.log` |
| debug | pass | 74 | `build/verify/debug.log` |
| ubsan | pass | 153 | `build/verify/ubsan.log` |
| hardened | pass | 88 | `build/verify/hardened.log` |
| cli-smoke | pass | 3 | `build/verify/cli-smoke.log` |
| exchange-demo | pass | 17 | `build/verify/exchange-demo.log` |
| itch-demo | pass | 1 | `build/verify/itch-demo.log` |
| mold-udp-demo | pass | 1 | `build/verify/mold-udp-demo.log` |
| cli-arg-sweep | pass | 220 | `build/verify/cli-arg-sweep.log` |
| fuzz-200k | pass | 27 | `build/verify/fuzz-200k.log` |
| fault-injection | pass | 0 | `build/verify/fault-injection.log` |
| real-data | pass | 369 | `build/verify/real-data.log` |
| format | skipped: pass --format with CLANG_FORMAT set to clang-format 19.1.7 | | |
| mutation | pass | 675 | `build/verify/mutation.log` |
* finished: 2026-11-11T08:15:13Z, all run steps passed
