# Linux validation plan for the sprint code

The R1–R3 code passed every Linux job in CI at `418d247`: GCC 13 Debug, Clang 18 ASan+UBSan,
Clang 18 TSan, Release and clang-format (see [verification.md](verification.md)). The code
added on the `enginex-quant-sprint-20261109` branch has been built and tested on macOS only,
with Apple clang 17. **No Linux, GCC, ASan, TSan or clang-format result exists for it.** This
page records why, and the shortest way to get those results.

## What ran, and what could not run here

| Check | Sprint status | Evidence or blocker |
|---|---|---|
| Release, Debug, UBSan (`-fno-sanitize-recover`), hardened libc++ | run | 523/523 each (verification.md); Debug and UBSan 3 consecutive runs each |
| Leaks (`leaks --atExit`), guard malloc | run | 0 leaks over 501 gtests; MallocScribble/GuardEdges whole binary |
| ASan, TSan on macOS | **blocked** | the sanitizer runtimes fail even on an empty `int main(){}`: ASan hangs, TSan exits 139 (verification.md) |
| GCC 13 | **not run** | no GCC installed on this machine |
| Docker (`scripts/linux_check.sh`) | **blocked** | `docker` is installed, but the sandbox denies the socket: `dial unix /var/run/docker.sock: connect: operation not permitted` |
| GitHub Actions on the branch | **not read** | `gh` cannot read its config in the sandbox (`open ~/.config/gh/config.yml: operation not permitted`). The branch is pushed by the owner's own sync process, not from this session |
| clang-format 19.1.7 | **dry run only: fails** | A read-only review ran the PyPI wheel's clang-format 19.1.7 with `--dry-run`; the binary's SHA-256 matches the wheel's RECORD. It reports **802 violations in 43 files**, many of them lines over 100 columns. Every file was clean at `418d247`. Applying it (`-i`) from this session was refused by the sandbox (`[Code from External]`), so the formatting commit is left to the owner (step 1 below) |
| Python tools on Python 3.8–3.12 | checked statically | every tracked `.py` parses with `ast.parse(feature_version=(3, 8))`, `(3, 10)` and `(3, 12)`; no f-string reuses its own quote (PEP 701 syntax) |

Partial proxies run here: a clang build with `-Wshadow-all` (0 warnings), a scripted search
for compound assignments to narrow types, explicit standard headers for libstdc++, and a
source-level portability review of the sprint diff. None of these replaces a GCC build.

## How to close the gap, cheapest first

1. **Formatting** (the CI format job fails until this is done). On any machine with Python:

   ```bash
   python3 -m venv /tmp/cf && /tmp/cf/bin/pip install clang-format==19.1.7
   git ls-files '*.cpp' '*.hpp' | xargs /tmp/cf/bin/clang-format -i
   CLANG_FORMAT=/tmp/cf/bin/clang-format scripts/check.sh format     # must print "format OK"
   cmake --build build/release && ctest --test-dir build/release -j8  # formatting only: still all pass
   ```

   Commit the result on its own ("style: clang-format 19.1.7"), so the formatting diff is
   reviewable apart from any logic change.

2. **GitHub Actions** on the pushed branch runs all five jobs (`.github/workflows/ci.yml`).
   Read each job's log, not just the status icon. ASan, UBSan and TSan print reports, and a
   test that exits 0 after a report still counts as a failure here.

3. **Locally in Docker**, on a machine where the socket is usable (results are not identical
   to the hosted runner, but close):

   ```bash
   scripts/linux_check.sh gcc-debug clang-asan clang-tsan release   # ubuntu:24.04 container
   scripts/ci_local.sh                                              # the workflow itself, via act
   ```

## Where failures are most likely

These are the riskiest places, from the portability review and from what is new since
`418d247`. If a job fails, check them first.

* **GCC-only warnings under `-Werror`** in new code. Examples: `-Wconversion` on arithmetic
  with `uint16_t`, `-Wdangling-reference` (new in GCC 13), and `-Wmaybe-uninitialized`
  around `std::optional`.
* **UDP behaviour on Linux** (`src/itch/mold_udp.cpp`, `MoldUdp.*` tests). The per-peer answer
  sockets share the request port through `SO_REUSEPORT` and are `connect()`ed. Linux prefers
  the connected socket for that peer, which the sender handles (`via_fd`), but this has never
  run there. Linux also doubles `SO_RCVBUF`, which changes how many datagrams the
  small-buffer test drops (it asserts recovery, not a drop count).
* **Specific tests named by the portability review**, all plausible rather than certain:
  * `MoldUdp.UnpacedBurstIntoASmallSocketBufferStillRebuildsTheExactBook`. Linux charges
    each datagram's full kernel buffer against `SO_RCVBUF`, so a 32 KiB buffer (64 KiB after
    doubling) holds only ≈ 20–30 datagrams. The receiver's request socket uses the same size,
    so retransmission bursts are mostly dropped again, and recovery advances about one buffer
    per 50 ms retry. It should converge (idle timeout 5 s). If it times out under TSan,
    either size the request socket separately or lengthen the timeouts.
  * `FeedMirrorSession.StalledMirrorIsCutOffThenResyncsToTheExactBook`. It needs ≈ 1.5 s of
    traffic to overflow an 8-frame queue plus the socket buffers, which are larger on Linux.
    If the traders are too slow under TSan, the slow-consumer assertion fails. Fix by
    lowering the client's `SO_RCVBUF` or lengthening the stall.
  * The ASan job runs LeakSanitizer, which has never run on the sprint code. A read-through
    found no leak; macOS `leaks` reported 0 over 501 gtests.
  * GCC Release at `-O3 -Werror` can raise `-Wmaybe-uninitialized` and
    `-Wstringop-overflow` false positives that the `-O0` Debug job cannot. The review named
    two candidate patterns: `std::optional<Session>` in `Sequencer::Options`, and the sized
    `std::vector` plus insert in MoldUDP64 packet building. If they fire, demote just that
    warning for GCC Release.
* **Timing-sensitive tests under sanitizers.** The TSan build is several times slower. Socket
  tests with timeouts (`MoldUdp.*`, `Server.*Timeout*`, `FeedMirrorSession.*`) are the
  candidates. `ctest -R 'MoldUdp|Server\.|FeedMirrorSession|exchange_pymodel'` reruns just
  those. The gtests carry no ctest labels, so `-L net` selects only the three Python network
  checks.
* **The Python checkers in ctest** (`exchange_pymodel_*`, `itch_pydecode_*`,
  `itch_pybook_lifecycle`, `cli_itch_generate_replay_dump`) need `python3` on the runner. The
  workflow installs it.

## Done when

All five jobs pass and no sanitizer report appears in any log. Then add a row for the sprint
commit to the "Where suites have run" table in [verification.md](verification.md), with the
run link, the runner image version and the test counts. Until then, every document must say
that GCC, ASan, TSan and clang-format have not run on the sprint code.
