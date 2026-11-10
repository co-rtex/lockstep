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
  `tools/itch_fixture.py` (hand-scripted) or the seeded generator. No Nasdaq data, licensed
  or otherwise, is in the repository or needed to build and test.

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
  order. If the buffer bound would be exceeded, it goes `Failed` and **stops** rather than
  skipping. A book built on top of it is then correct up to the hole and never silently
  wrong after it. Packets from another session are rejected.
* Tests (`itch_moldudp64_test.cpp`): golden packet bytes; every truncation, trailing bytes,
  control with payload and sequence wrap; 100 000 random packets; each sequencer rule. An
  end-to-end test sends 30 000 synthetic ITCH messages as MTU-sized packets over a simulated
  channel with 3 % loss, 5 % duplication and reordering within 8 packets, re-requests missing
  ranges from the sender (over a channel that is itself lossy), and requires the rebuilt book
  hash to equal a direct replay, for three channel seeds. Without recovery, the book stops
  exactly at the hole. Mutation check: letting the sequencer skip over gaps fails 5 of the 13
  tests, including both end-to-end tests.
* Not implemented: a UDP socket receiver, multicast group handling, a re-request client and
  server on the wire (the tests simulate retransmission in-process), and SoupBinTCP.

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
* The order index is an open-addressing table (`order_index.hpp`): Fibonacci hashing, linear
  probing, backward-shift deletion (no tombstones), load factor ≤ ½, no allocation per
  operation. It replaced `std::unordered_map`; see *Performance*. The old index remains
  available as `-DENGINEX_ITCH_STD_INDEX=ON` for same-commit comparisons.
* Each side of each book is a `std::map<price, Level>`. `Node::level` points into the map
  node, which `std::map` keeps stable; a level is erased only after its last order is
  unlinked. Map nodes come from a per-builder free-list arena, so level churn makes no heap
  calls (≈ 0.30 → 0.003 allocations/msg at 5000 symbols). `-DENGINEX_ITCH_STD_LEVEL_ALLOC=ON`
  restores `std::allocator` for comparison.
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
| Order index | vs `std::unordered_map` under random operations; deletion inside wrapping collision chains | `OrderIndex.*` |
| Coverage | `scripts/coverage.sh` (clang source-based, whole ctest suite incl. CLI subprocesses). 2026-11-10: lines 94.7 %, branches 86.5 %, regions 91.0 % over `src/itch`, `include/enginex/itch`, `mirror.cpp`, `cli_itch.cpp`, `server.cpp`; per file ≥ 90 % lines (decoder.hpp 90.5 %, mirror.cpp 90.8 %, server.cpp 90.1 %). Uncovered code is mostly `to_string` defaults, accessors and the `-DENGINEX_ITCH_STD_INDEX` A/B index | |
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

If you have a licensed historical file in this framing, `enginex itch replay --input FILE`
works the same way. No such file was used in developing or measuring this code.

## Performance

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
* No historical data was available. The behavior on real full-day files (message mix,
  anomaly rates, peak order counts) is untested. The synthetic workload's mix is documented
  in `synthetic.hpp` and is not a model of Nasdaq traffic.
* The reconstructed book is the **displayed** Nasdaq book only. Non-displayed liquidity
  (`P`), auction state (`I`, `Q`), and halts (`H`, `h`) are recorded as statistics or state
  but do not alter the book.
* MPID attribution is kept per order but is not part of the state hash.
* Symbols are not validated against the directory beyond the `symbol_mismatch` count.
