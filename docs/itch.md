# ITCH 5.0 market-data path

EngineX contains two systems that are deliberately kept apart:

| | Synthetic exchange (R1–R3) | ITCH research path (this document) |
|---|---|---|
| Direction | order **entry** → matching → acks and a feed | outbound **market data** → reconstruction |
| Input | EngineX JSON commands / TCP protocol v1 | Nasdaq TotalView-ITCH 5.0 binary messages |
| Who matches | EngineX's own matching engine | nobody: executions are *reported* by the feed |
| Code | `src/{engine,state,journal,server}.cpp` … | `include/enginex/itch/`, `src/itch/`, `src/cli_itch.cpp` |
| Types | `enginex::Side`, `Command`, `OrderId` | `enginex::itch::Side`, `Message`, `OrderRef` |

ITCH messages are never turned into EngineX commands, and historical feed data is never
presented as orders placed through the synthetic exchange. The only shared code is the
in-tree SHA-256 used for state hashes. The two `Side` enums are distinct types on purpose; the
compiler rejects mixing them (this caught one ambiguity in `cli_itch.cpp`).

## Specification and provenance

* Primary source: *Nasdaq TotalView-ITCH 5.0* interface specification, nasdaqtrader.com:
  `NQTVITCHspecification.pdf` (revision log through **2023-04-28**, adds the DLCR message) and
  the older `NQTVITCHSpecification_5.0.pdf` (dated 2015-03-06). Offsets and lengths in
  `messages.hpp` were checked against both; the later revision only adds messages
  (`J` 2017, `h` 2018, `O` 2023) and does not move fields in older ones.
* The PDFs are not redistributed in this repository.
* All test fixtures are **synthetic**: produced from the public format by
  `tools/itch_fixture.py` (hand-scripted) or the seeded generator. No Nasdaq data is in the
  repository or needed to build and test.
* **Validation on real data** (local only): a prefix of one of Nasdaq's publicly posted
  historical sample files. See *Real Nasdaq sample* below. The data were downloaded for local
  validation and are not redistributed: they stay under the git-ignored `build/`.

## Layers

```text
 bytes ──► StreamFramer ──► decode() ──► BookBuilder::apply() ──► depth / top / state hash
          (file framing)    (payload)     (order-level book)
          fatal errors:     DecodeError:   Anomaly: counted, message rejected
          ZeroLength,       Unknown type,  (or informational); --strict stops
          TruncatedTail     LengthMismatch,
                            InvalidField
```

`replay()` wires the three together and applies the error policy.

### File framing (an assumption, documented)

ITCH defines message *payloads*. In production they travel inside MoldUDP64 (UDP) or
SoupBinTCP (TCP) packets. Nasdaq's published historical sample files instead store each
message preceded by a **2-byte big-endian length**. That convention is the only supported
input. `StreamFramer` handles any chunking (tested at every split point and byte by byte):
complete messages are passed as zero-copy spans, and only a message that straddles a chunk
boundary is copied (into a carry buffer of at most 2 + 65 535 bytes). A zero length prefix,
or a stream that ends inside a message, is a fatal `FrameError` with the stream offset, because
the message boundaries after it are unknown.

**The file path has no gap detection.** The file framing carries no sequence numbers. ITCH
timestamps are nanoseconds since midnight, not sequence numbers, and equal or decreasing
timestamps are legal. The book counts timestamp regressions for information only. Gap
detection lives in the transport layer below: see *MoldUDP64*.

### MoldUDP64 (transport sequencing and gaps)

`include/enginex/itch/moldudp64.hpp` parses MoldUDP64 downstream packets and sequences their
messages. Every packet has a 20-byte header (10-byte session, 8-byte sequence number of its
first message, 2-byte message count, big-endian), followed by `count` blocks of a 2-byte
length and the message. Count 0 is a heartbeat and 0xFFFF is end of session; both carry the
next expected sequence number. Text could not be extracted from Nasdaq's `moldudp64.pdf`
(custom font encoding), so the layout was cross-checked against the FINRA and Japannext copies
of the specification and the Wireshark dissector, which agree.

* `parse()` validates the whole packet (length, every block boundary, no trailing bytes,
  control packets carry no payload, no 64-bit sequence wrap) before returning any message.
  A malformed packet delivers nothing.
* `Sequencer` releases messages strictly in order, each exactly once. It drops duplicates and
  the already-delivered part of overlapping packets. It holds packets that arrive ahead of a
  hole in a bounded reorder buffer and reports `missing()` ranges for re-request (heartbeats
  can reveal a hole with no data packet following). When the hole is filled, it drains in
  order. If a buffer bound would be exceeded (default 65 536 messages or 64 MiB of payload),
  or a packet starts more than `max_ahead` (default 10⁶) beyond the next expected sequence, it
  goes `Failed` and **stops** rather than skipping. A corrupt header and a huge loss look the
  same, and neither may grow memory without bound. A book built on top of it is then correct up to the hole and never silently
  wrong after it. Packets from another session are rejected.
* Tests (`itch_moldudp64_test.cpp`): golden packet bytes; every truncation, trailing bytes,
  control with payload and sequence wrap; 100 000 random packets; each sequencer rule. An
  end-to-end test sends 30 000 synthetic ITCH messages as MTU-sized packets over a simulated
  channel with 3 % loss, 5 % duplication and reordering within 8 packets, re-requests missing
  ranges from the sender (over a channel that is itself lossy), and requires the rebuilt book
  hash to equal a direct replay, for three channel seeds. Without recovery, the book stops
  exactly at the hole. Mutation check: letting the sequencer skip over gaps fails 5 of the 13
  tests, including both end-to-end tests.
* `enginex itch mold-sim --input FILE [--loss-percent P] [--no-recovery]` runs the same
  experiment from the command line, on a **simulated** channel. It reports packets lost and
  duplicated, gaps opened and closed, and whether the rebuilt book equals a direct replay.
  Example: 200 010 messages in 4 799 packets, 143 lost, 82 gaps opened and all closed by
  re-request, book hash identical to the direct replay. On real messages (the first 2.87 M
  of the Nasdaq sample): 61 107 packets, 1 852 lost, 1 072 gaps opened and closed, and the
  same book as the direct replay. Exit 0 only if the book equals the delivered prefix (never
  silently wrong) and, with recovery, delivery is complete.
* Not implemented: a UDP socket receiver, multicast group handling, a re-request client and
  server on the wire (retransmission is simulated in-process), and SoupBinTCP.

### Payload decoding

* Every message's length is checked against the specification **before** any field is read.
  `ByteReader` then assembles big-endian integers from individual bytes: no pointer casts, no
  alignment or aliasing assumptions, and every read is bounds-checked (out-of-range reads
  throw rather than invoking undefined behavior, as a second line of defense).
* Timestamps are 6-byte (48-bit) integers. Prices are `Price(4)`, a `uint32` with 4 implied
  decimals, except in `V`, which uses `Price(8)` `uint64`. The `Q` message's shares field is
  8 bytes.
* Enumerated fields that the book depends on are validated: the add-order side (`B`/`S`) and
  the `C` printable flag (`Y`/`N`). Other alphanumeric fields are passed through unchanged.
* `encode()` is the exact inverse, used only for fixtures and the generator. The tests check
  that every successful decode re-encodes to identical bytes.

## Supported messages

All 23 message types of the current revision are decoded into typed structs. The table shows
what the reconstructor does with each.

| Type | Message | Length | Decoded | Book effect | Tested by |
|---|---|---:|---|---|---|
| `S` | System Event | 12 | yes | none (last event code recorded) | golden, fixture |
| `R` | Stock Directory | 39 | yes | locate → symbol | golden, fixture |
| `H` | Stock Trading Action | 25 | yes | trading state per locate | golden, fixture |
| `Y` | Reg SHO Restriction | 20 | yes | none | golden |
| `L` | Market Participant Position | 26 | yes | none | golden |
| `V` | MWCB Decline Level (Price(8)) | 35 | yes | none | golden |
| `W` | MWCB Status | 12 | yes | none | golden |
| `K` | IPO Quoting Period Update | 28 | yes | none | golden |
| `J` | LULD Auction Collar | 35 | yes | none | golden |
| `h` | Operational Halt | 21 | yes | none | golden |
| `A` | Add Order (no MPID) | 36 | yes | **add** at the back of its level | golden, scenario, differential, fixture |
| `F` | Add Order with MPID | 40 | yes | **add**; MPID kept for the order's life | golden, scenario, differential, fixture |
| `E` | Order Executed | 31 | yes | **reduce**, priority kept | golden, scenario, differential, fixture |
| `C` | Order Executed with Price | 36 | yes | **reduce**, priority kept; display price unchanged | golden, scenario, differential, fixture |
| `X` | Order Cancel | 23 | yes | **reduce**, priority kept | golden, scenario, differential, fixture |
| `D` | Order Delete | 19 | yes | **remove** | golden, scenario, differential, fixture |
| `U` | Order Replace | 35 | yes | **remove + add** (new ref, back of queue) | golden, scenario, differential, fixture |
| `P` | Trade (non-cross) | 44 | yes | none (non-displayed volume) | golden, scenario, fixture |
| `Q` | Cross Trade | 40 | yes | none (cross volume) | golden, scenario, fixture |
| `B` | Broken Trade | 19 | yes | none (count) | golden, scenario, fixture |
| `I` | NOII | 50 | yes | none | golden |
| `N` | Retail Interest (RPII) | 20 | yes | none | golden |
| `O` | DLCR Price Discovery | 48 | yes | none | golden |

Any other type byte is `UnknownType`: it is counted, its byte value recorded, and the frame
skipped. The framing tells us its length, so later messages are unaffected.

## Book semantics

The book follows the specification text (§4.3–4.5):

* **Add** (`A`/`F`): a new displayed order joins the **back** of its price level's FIFO.
* **Execute** (`E`, `C`) and **cancel** (`X`) reduce the order's remaining shares. Effects
  are cumulative, and an order **keeps its queue position**. At zero shares the order is dead
  and removed. `C` reports an execution price that may differ from the display price; the
  order's displayed price does not change. Non-printable `C` executions are excluded from
  printable volume (the specification warns they would be double-counted).
* **Delete** (`D`) removes the order.
* **Replace** (`U`) removes the original order. A new order with the new reference, shares
  and price joins the **back** of its level. It keeps the original's side, stock and MPID,
  which the message does not repeat. Priority is lost **even when price and shares are
  unchanged**; a test pins this.
* **Trades** (`P` non-displayed, `Q` cross, `B` break) never change displayed orders.
* Levels aggregate shares and order count. Best bid is the highest bid level, best ask the
  lowest ask level. Depth is reported best-first; orders within a level in FIFO order.
* **Locked and crossed books are reported, not rejected.** A historical feed can legitimately
  show either transiently, and the reconstructor does not "fix" market data.
* Books are per stock locate. Locate is authoritative; the symbol comes from `R`. If no `R`
  was seen, the symbol is learned from the first add. A later `R` with a different symbol for
  that locate is then counted as `directory_conflict`, and stops a `--strict` replay.

### Anomalies (input that contradicts the reconstructed book)

| Anomaly | Trigger | Action |
|---|---|---|
| `unknown_order_ref` | E/C/X/D/U for a reference that is not live | rejected |
| `duplicate_order_ref` | A/F, or U's new reference, already live | rejected |
| `over_execution` | E/C/X shares > remaining shares | rejected (not clamped) |
| `zero_shares` | A/F/U with 0 shares, or E/C/X of 0 shares | rejected |
| `locate_mismatch` | a modify message's locate differs from the order's | rejected |
| `symbol_mismatch` | an add's stock differs from the directory symbol of its locate | applied, counted |
| `directory_conflict` | `R` remaps a locate to a different symbol | applied, counted |

A rejected message leaves the book byte-for-byte unchanged; the tests compare state hashes.
This is a deliberate policy with known consequences. A rejected `U` (for example, an unknown
original) means the new reference is never created, so later messages for it are
`unknown_order_ref` too (a cascade). Consumers that need the first cause should use
`--strict`, which stops at the first anomaly with its message number. Replaying from the
middle of a day (no prior adds) produces `unknown_order_ref` for orders added earlier. A
correct full-day file replayed from its start should produce **zero** anomalies; the clean
synthetic streams do.

### Data layout

* Orders live in one pool (`std::vector<Node>`) with a free list. Each price level holds an
  intrusive doubly linked FIFO of 32-bit pool indices.
* The order index is a **paged direct index** (`paged_index.hpp`, D27). Nasdaq order
  references are dense and mostly increasing. On the real sample, 73 % of adds carry a larger
  reference than the previous add, and there are ≈ 20 M distinct references in 21 M
  messages. So references below 2³² index 4 096-entry pages directly: two dependent loads, no
  hashing or probing. Pages are created on demand and freed when empty, and the page count
  is capped (256 MiB). References above the direct range, and any arriving when the budget is
  spent, go to an open-addressing hash table (`order_index.hpp`: Fibonacci hashing, linear
  probing, backward-shift deletion, load ≤ ½). The history is `std::unordered_map`, then that
  hash table alone, then this; `-DENGINEX_ITCH_STD_INDEX=ON` and
  `-DENGINEX_ITCH_HASH_INDEX=ON` rebuild the earlier variants.
* Each side of each book is a **contiguous vector of (rank, Level\*) sorted so that the best
  price is at the back**. Rank is the price for bids and `UINT32_MAX − price` for asks.
  Lookups are binary searches over contiguous memory. Inserting or removing a level near the
  top moves only the few entries behind it, which matters because 43 % of real adds open a
  new level. `Level` objects live in a deque pool (stable addresses, free list), so
  `Node::level` stays valid until the level's last order is unlinked. The earlier
  `std::map` levels (with a node arena) stay available as `-DENGINEX_ITCH_MAP_LEVELS=ON`.
* Containers that hash are never iterated for output. All output (depth, canonical state,
  JSON) comes from ordered structures, so results are deterministic.

### Determinism

`canonical_state()` writes every locate (all 8 symbol bytes, non-printables escaped), every
level, and every order in priority order. `state_hash()` is its SHA-256. Tests check that the
same bytes give the same hash across runs, across read chunk sizes (1, 2, 3, 7, 64, 4096
bytes and the whole buffer) and across both index implementations (benchmark rows).

## Error policy (`replay`)

* Framing errors are fatal.
* Decode errors and anomalies are counted and skipped (lenient, the default), or stop the
  replay at the first one (`--strict`). Informational anomalies also stop a strict replay.
* Replay stops at the **first** problem. The result then describes the stop point only:
  `frames` and `bytes_processed` count up to and including the stopping frame, and later
  framing errors are not examined. (This was a defect found by an independent review; there
  is a regression test.)
* Exit status of `enginex itch replay`: 0 completed, 1 stopped, 2 usage error.

## Verification

| What | How | Where |
|---|---|---|
| Field offsets of all 23 types | hand-written expected bytes from a test-local writer, independent of the encoder; one fully literal Add Order | `itch_codec_test.cpp` `ItchGolden.*` |
| Cross-language agreement | bytes packed by Python `struct` from the spec tables; hand-derived expected book and statistics | `tools/itch_fixture.py`, `itch_fixture_test.cpp`, ctest `itch_fixtures_up_to_date` |
| Malformed input | every wrong length for every type, every unknown type byte, invalid enums, 200 000 random payloads, explicit A/F/C random round trips | `ItchMalformed.*` |
| Framing | every 2-chunk split, byte-at-a-time and random chunking, truncation at every offset, zero-length prefix at every split | `StreamFramer.*` |
| Book semantics | scenario tests per rule (priority on E/X, loss on U, aggregation, P/Q/B no-ops, crossed/locked, per-locate isolation, attribution) | `ItchBook.*` |
| Anomalies | every kind, book hash unchanged after each rejected message | `ItchBook.AnomaliesAreRejected…` |
| Differential | naive reference model (`std::map`, arrival counters, brute-force levels) compared message by message over 12 seeds, half with 2 % injected anomalies | `ItchDifferential.*` |
| Mutation check | making U keep priority at the same price fails 2 scenario tests and all 12 differential seeds (run manually, then reverted) | recorded in the commit log |
| Corrupted files | 300 randomly corrupted/truncated files: no crash, invariants hold, deterministic | `ItchReplay.CorruptedFilesNeverBreakInvariants` |
| Fuzzing | `fuzz/itch_fuzz.cpp` (libFuzzer entry point: ITCH replay lenient+strict, and MoldUDP64 sequencing → decode → book) checks invariants, frame accounting, determinism and in-order exactly-once delivery. The standalone seeded mutation driver (not coverage-guided; Apple clang has no libFuzzer) ran **5 000 000 inputs (2.3 GB) with no violation** in a Release build (seed 7, 13 min), and 300 000 inputs under UBSan (Debug, seed 20261110, 41 min) with no violation and no UBSan report. After the sorted-vector/paged-index change (`bbd4ff6`): 2 000 000 more inputs (Release, seed 11), no violation. ctest runs 20 000 inputs. The coverage-guided build (`-DENGINEX_FUZZ_LIBFUZZER=ON`) is for Linux and has not been run here | `fuzz/README.md` |
| Order index | vs `std::unordered_map` under random operations; deletion inside wrapping collision chains | `OrderIndex.*` |
| Coverage | `scripts/coverage.sh` (clang source-based, whole ctest suite incl. CLI subprocesses). Latest run (commit `9c98867`): lines 94.2 %, branches 86.4 %, regions 91.8 % over `src/itch`, `include/enginex/itch`, `mirror.cpp`, `cli_itch.cpp`, `server.cpp`. `paged_index.hpp` 100 %, `order_index.hpp` 99 %, `book.cpp` 92.7 %, `moldudp64.cpp` 86.5 %. Uncovered code is mostly `to_string` defaults, accessors and the `-DENGINEX_ITCH_STD_INDEX` A/B index | |
| Sanitizers | UBSan: whole suite. MallocScribble/GuardEdges: ITCH tests. ASan and TSan cannot start in this sandboxed macOS environment (exact errors in `docs/PROGRESS.md`); they run in Linux CI | |

The reference model is deliberately independent in its *data structures*. It does, however,
encode the same reading of the specification and the same anomaly policy, so it checks the
implementation, not the interpretation. The interpretation is pinned by the scenario tests,
which follow the specification text, and by the hand-derived fixture.

## Command line

```bash
# Synthetic fixture (seeded, deterministic; not market data)
./build/release/enginex itch generate --out data/demo.itch --seed 42 --messages 200000 --symbols 4
# Rebuild books: text summary, or one JSON object with --json; --metrics-out adds wall time
./build/release/enginex itch replay --input data/demo.itch --depth 5
./build/release/enginex itch replay --input tests/testdata/itch/lifecycle.itch --strict --json
# Decode only: one JSON line per frame (errors included)
./build/release/enginex itch dump --input tests/testdata/itch/lifecycle.itch --limit 10
```

Any ITCH 5.0 file in this framing works the same way (see *Real Nasdaq sample*).

## Real Nasdaq sample (local validation)

Nasdaq posts historical TotalView-ITCH 5.0 sample files (gzip) at `emi.nasdaq.com/ITCH/`.
The full 2019-01-30 file (`01302019.NASDAQ_ITCH50.gz`) is 4.76 GB. Only its first 256 MiB
were downloaded, with an HTTP range request (SHA-256 of those bytes
`e917b2fb0ebb3234a13e1837bae0fb73a89b14bbc9377a8cd0c3cdd9f4e56ea5`), and the valid prefix was
decompressed locally (676 MB). It is used for validation and measurement on this machine
only; it is not committed, redistributed, or required by any build or test.

```bash
curl -r 0-268435455 -o prefix.gz https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/01302019.NASDAQ_ITCH50.gz
python3 -c "import zlib,sys; d=zlib.decompressobj(31); sys.stdout.buffer.write(d.decompress(open('prefix.gz','rb').read()))" > prefix.itch
enginex itch replay --input prefix.itch --strict --json        # exit 1 only for the cut-off last message
ENGINEX_ITCH_REAL_FILE=prefix.itch build/release/tests/enginex_tests --gtest_filter='ItchRealData*'
```

Results (21 255 695 messages, 03:03:59 to 09:37:05 ET, through the opening cross):

* **Framing and decoding:** every frame decodes, with 0 unknown types, 0 length mismatches
  and 0 invalid fields. The 2-byte length-prefixed file convention holds. The only framing
  error is the truncated final message where the download was cut.
* **Message mix** (all 16 types present): A 8.56 M, D 7.60 M, U 1.48 M, X 1.21 M, I 1.05 M,
  F 0.79 M, E 268 k, L 194 k, P 62 k, Y/H/R ≈ 8.7 k each, Q 8 713 (the opening cross),
  C 5 974, S 3, V 1.
* **Book semantics:** **zero anomalies in `--strict` mode**. Every execute, cancel, delete and
  replace referred to a live order with enough shares, so the semantics hold on a real
  session. There were no timestamp regressions, and none of the 8 713 books was crossed or
  locked at 09:37:05. AAPL at that time: best bid 162.16 × 100, best ask 162.20 × 32,
  2 723 / 2 656 levels.
* **Independent check:** `ItchRealData.MatchesReferenceModelIfAvailable` also replays the file
  through the naive reference model. All 1 539 130 live orders in 17 384 books, and every
  per-message anomaly verdict, agree.
* **What real data showed that synthetic data hid:** 43 % of adds and replaces open a new
  price level (synthetic stream: 1.6 %). Books are deep (thousands of levels). With 1.5 M live
  orders, a hash index no longer fits in cache. Both findings changed the data layout below.

Not covered by this sample: the rest of the trading day (≈ 95 % of the file), the closing
cross, halts and LULD collars in volume, and other days.

## Performance

On the real sample: decode 13.0 ns/msg (≈ 2.3 GiB/s), full replay 99.3 → 62.1 ns/msg after the
layout changes driven by that data. Synthetic results follow.

See [benchmarks.md § ITCH](benchmarks.md#itch-50-decoding-and-reconstruction). In summary, on
1 M synthetic messages over 8 symbols: decode ≈ 14.6 ns/msg (≈ 2.1 GiB/s), book apply
52.5 → 32.2 ns/msg after replacing the order index (26.8 ns/msg with a capacity hint), and
full replay 67.4 → 43.9 ns/msg. At 5000 symbols (2 M messages), apply costs 56.1 ns/msg
with the level-node arena, versus 70.6 ns/msg without. All in-process, single thread. These
are not wire-to-wire latencies.

## Limitations

* Files: only the 2-byte length-prefixed framing (no gap detection possible). Transport:
  MoldUDP64 packet parsing and sequencing are implemented and tested on simulated channels.
  There is no socket receiver, no live retransmission and no SoupBinTCP.
* Real data: only the first 6.5 hours (pre-market through 09:37) of one day's sample were
  used, not a full day or several days. The synthetic workload is *not* a model of Nasdaq
  traffic. It creates new levels 30× less often than the real sample, and its books are
  shallower.
* The reconstructed book is the **displayed** Nasdaq book only. Non-displayed liquidity
  (`P`), auction state (`I`, `Q`), and halts (`H`, `h`) are recorded as statistics or state
  but do not alter the book.
* MPID attribution is kept per order but is not part of the state hash.
* Symbols are not validated against the directory beyond the `symbol_mismatch` count.
