# EngineX architecture

EngineX is a standalone C++20 educational trading-systems lab. It is **not** part of, and
does not replace, the TradeGrid Go exchange (see
[tradegrid-compatibility.md](tradegrid-compatibility.md)).

```text
  TradeGrid (club, Go)                          EngineX (this repo, C++20, independent)
  ┌───────────────────────────┐                 ┌─────────────────────────────────────────────┐
  │ React UI → HTTP API       │   public spec   │  enginex CLI / TCP clients                   │
  │ orders · risk · accounts  │ ──────────────▶ │        │ JSON lines / length-framed JSON     │
  │ Go matching engine        │  + fixtures     │  enginex_codec  (JSON boundary only)         │
  │ (authoritative for club)  │  (read-only,    │        │ typed Command                       │
  └───────────────────────────┘   attributed)   │  ExchangeStateMachine  ← single sequencer    │
          no runtime link                       │   validate → sequence → MatchingEngine       │
                                                │  enginex_journal (R2)  enginex_net (R3)      │
                                                └─────────────────────────────────────────────┘
```

## Libraries and ownership

| Target | Contents | Depends on |
|---|---|---|
| `enginex_core` | types, `OrderBook`, `MatchingEngine`, `ExchangeStateMachine`, canonical state, SHA-256 | C++ standard library only |
| `enginex_codec` | JSON ⇄ typed commands/results, `simulate` | core, nlohmann/json 3.11.3 |
| `enginex_journal` | CRC32C, EXJ1 journal codec/scan, `JournalFile` (flock, fsync modes, fault injection), `DurableExchange` | core |
| `enginex_net` | framing + protocol v1 codec, `BoundedQueue`, POSIX sockets, `Server`, `Client` | codec, journal, threads |
| `enginex_itch` | ITCH 5.0 decoder/encoder, file framer, `BookBuilder` reconstruction, replay, synthetic streams ([itch.md](itch.md)); separate from the exchange | core (SHA-256 only) |
| `enginex_bench_lib` | workload generators, measurement harness (core, durable, recovery, network), metrics | core, journal, net |
| `enginex` | CLI (`simulate`, `benchmark`, …, `itch`) | all of the above |
| `enginex_itch_bench` | ITCH decode / book / replay measurements with allocation counting | itch |
| `enginex_tests` | GoogleTest suite | all libraries |

The core has no I/O, no clocks, no JSON and no threads. Everything that touches the outside
world sits in a separate library so the matching rules can be tested and benchmarked alone.

## One authoritative sequencer

`ExchangeStateMachine` is the only object that mutates order books (spec invariant 14). Each
command goes through two steps:

1. `prepare(cmd) const`: pure validation against the current state. It returns `Accept`
   (with the sequence number that will be assigned), `Duplicate` (with the stored original
   record) or `Reject` (with an error code). Nothing is mutated, so a rejection is atomic by
   construction.
2. `commit(cmd, decision)`: applies exactly that decision. It refuses a stale decision (one
   whose sequence is no longer `last + 1`), which guards the R2 "validate, then journal, then
   apply" pipeline against interleaving.

**Why one thread owns the book.** Matching is inherently sequential: the outcome of an order
depends on every earlier order at overlapping prices. Concurrent writers would need locking
around the whole book (no parallelism gained) or would make acceptance order depend on thread
scheduling, which breaks determinism and replay. A single sequencer gives a total order
(`command_sequence`), cheap replay, and no locks on the hot path. Concurrency (R3) is used only
around it: parsing, socket I/O and fan-out.

## Order book representation

```text
OrderBook (one per symbol)
  bids: std::map<PriceCents, PriceLevel>   best = std::prev(end())   (highest price)
  asks: std::map<PriceCents, PriceLevel>   best = begin()            (lowest price)
PriceLevel { price, aggregate, std::list<RestingOrder> orders }      FIFO: front = oldest
MatchingEngine::index_  unordered_map<OrderId, {symbol, side, LevelMap::iterator, list iterator}>
```

* **Insert:** O(log L) to find/create the level (L = levels on that side), O(1) to append.
* **Cancel:** average O(1) hash lookup, O(1) list erase, O(1) amortized level erase via the
  stored map iterator.
* **Match:** visits only the executable resting orders and the levels they sit on. There is no
  universal O(1) bound; a sweep of k orders across m levels costs O(k + m).
* **Snapshot of depth d:** O(d) per side.

Both sides use the same ascending map type (DECISIONS.md D2), so one iterator type can be
stored in the index for either side.

**Why a price-level map plus FIFO lists, not one sorted vector.** A sorted vector of orders
makes inserts and cancels O(n) memmoves, and an insert invalidates every stored position, so
O(1) cancel by id is impossible. The map gives ordered levels with stable iterators, and the
list gives O(1) head pop (fills) and O(1) erase from the middle (cancels) without moving the
other orders. The cost is pointer chasing and one allocation per order and per level. That is
cache-unfriendly, and it is deliberately the clear baseline the spec asks for. Any faster
layout must beat it on the recorded benchmarks (see [benchmarks.md](benchmarks.md)).

### Iterator invalidation: why cancels never dangle

* `std::map` iterators stay valid until *that element* is erased; inserting other levels does
  not invalidate them.
* `std::list` iterators stay valid until *that node* is erased.
* `std::unordered_map` rehashing invalidates iterators but **not** references or pointers to
  elements, and the index is only accessed by key, never by a stored iterator.

The engine keeps three ordering rules, all checked by `check_invariants()` after every
command in the property tests:

1. When a resting order fills completely, its index entry is erased *before* `pop_front()`
   frees the list node.
2. A cancel copies the location out of the index, erases the index entry, then erases the
   list node, and erases the level only if the list is now empty.
3. A level is erased only when its FIFO list is empty, so no index entry can point at it.

ASan and UBSan runs of the full suite (Linux, Clang 18) found no use-after-free or invalid
iterator use.

**Allocation (R4).** Lists, level maps and the order index are `std::pmr` containers on one
`unsynchronized_pool_resource` per `MatchingEngine` (DECISIONS.md D26), so recycled nodes
avoid malloc/free. Iterator-stability guarantees are unchanged; the engine is movable but
not move-assignable.

## Identity and determinism

* `command_sequence`: one per *accepted* mutating command (new or cancel). Rejections and
  duplicate retries do not consume one.
* `order_id = "ord-<command_sequence of its new_limit>"`; `execution_id = "exe-<n>"` with one
  global execution sequence across symbols.
* No wall-clock time, random value, address or hash-iteration order influences matching. FIFO
  priority is acceptance order. The one random value in the core is the SipHash key of the
  request-id index. It decides only where an id sits in the table. Every lookup ends in an
  exact string comparison and the table is never iterated, so results and `state_hash` do not
  depend on it (a test runs two machines with different keys in lockstep).
* The canonical state (spec §8.2) is a fixed-order JSON document containing watermarks, books
  (bids descending, asks ascending, FIFO within a level), every accepted command with its
  outcome (this is the idempotency map) and every execution. `state_hash` is its SHA-256.

## Idempotency

Accepted request ids map to a `CommandRecord`: the full command body, its sequence, its
contiguous execution range and its outcome quantity. A retry with an identical body rebuilds
and returns the original response (`duplicate: true`) without touching the book. A different
body under the same id returns `DUPLICATE_REQUEST_ID_CONFLICT`. Rejected requests are not
remembered (DECISIONS.md D4): they are never journaled, so remembering them could not survive
a restart.

Storage (D31). Records live in a `std::deque<CommandRecord>` indexed by command sequence, so
references stay stable and memory is allocated in blocks. The `request_id` lookup is an
open-addressing table of 8-byte slots `{32-bit SipHash fingerprint, record index}` at load
≤ ½, so a new id usually costs one cache miss. It replaced a node-based `unordered_map`
whose two misses per new id made per-command cost grow from 108 to 296 ns between 100 k and
3 M commands.

## Validation and numeric safety

Prices are integer cents in [1, 100 000 000]; quantities are integers in [1, 1 000 000].
JSON numbers must be exact integers (no `100.0`, no strings or booleans), and values outside
int64 are rejected at the boundary. The state machine applies the range rules, and checked
arithmetic guards level aggregates: with the configured open-order cap, the largest possible
aggregate (1e6 × cap) is far below 2^63.

## Durability layer (R2)

`DurableExchange` wraps the state machine and does not change its rules. Every accepted
command is `prepare`d, `commit`ted in memory, encoded into a batch buffer, written, and
synchronized; only then is its reply released. Startup replays the journal through the very
same `prepare`/`commit` path, verifying that every record is accepted at exactly its logged
sequence. Replay therefore cannot silently diverge from live processing. Any I/O failure puts
it into fail-stop. The full contract, crash matrix and failure diagrams are in
[journaling.md](journaling.md).

## Networked service (R3)

```text
               ┌─────────────── per connection (≤ 64) ───────────────┐
 client ──TCP──▶ reader thread: FrameDecoder → parse JSON ──try_push──┐   (full → SERVER_OVERLOADED)
        ◀─TCP── writer thread ◀── bounded outbound queue (frames+bytes)│◀─────────────┐
               └─────────────────────────────────────────────────────┘│              │
                                                                      ▼              │ try_push only
                                         bounded ingress queue (4096, all connections)│ (overflow →
                                                                      │              │  resync_required,
                                   ┌──────────────── sequencer thread ─▼──────────┐   │  disconnect)
                                   │ pop batch → for each request:                │   │
                                   │   command: prepare → commit → encode record, │   │
                                   │            build ack + book_update           │   │
                                   │   snapshot/subscribe: flush batch first, then │   │
                                   │            snapshot at watermark (+register) │   │
                                   │ flush: one write + one sync for the batch ───┼───┘ then release
                                   └──────────────────────────────────────────────┘    acks/events in order
```

* **Only the sequencer thread touches exchange state** (invariant 14). Readers and writers
  move bytes and parse JSON. They never see the book.
* **Nothing blocks the sequencer on a client.** Readers `try_push` into the ingress queue; the
  sequencer `try_push`es into each connection's outbound queue. The two queues bound memory
  by frame count and bytes (DECISIONS.md D16). A slow reader's queue fills, and the connection
  is cut with a final `resync_required` frame. A reader that stays unwritable past the send
  timeout is cut by its writer thread.
* **Why subscriber queues are bounded.** An unbounded queue turns one stuck client into
  unbounded server memory. Blocking on a full queue would turn one stuck client into a stalled
  exchange. Bounding plus disconnect-and-resync keeps both memory and latency independent of
  the slowest consumer, at the price that slow consumers must resubscribe and get a fresh
  snapshot.
* **Group commit** amortizes the flush across a batch without weakening acknowledgment
  semantics: no reply or event leaves before the sync covering it (D14).
* **Handoff correctness:** snapshot and subscription registration happen in one sequencer step
  after the batch is flushed, so the first event is always `watermark + 1` (D15).
* Shutdown order: acceptor → readers → sequencer drains the queue and syncs → writers flush →
  join. See [protocol.md](protocol.md) §9.

## Benchmarks vs end-to-end latency

The R1 benchmark times `ExchangeStateMachine::process` in-process: validation, sequencing,
idempotency bookkeeping and matching. It excludes JSON, I/O, durability and networking. Its
p99 (a few hundred ns) is **not** comparable with the durable acknowledgment p99 (R2, ms on
this machine, dominated by `F_FULLFSYNC`) or the network round-trip p99 (R3, tens of µs in
memory, ms with a synced journal). [benchmarks.md](benchmarks.md) reports each separately,
with what it includes and excludes.
