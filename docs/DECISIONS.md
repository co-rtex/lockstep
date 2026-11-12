# Architectural decisions

Each entry records a nontrivial choice, the alternatives considered, and the tradeoff.

## D1. Single authoritative sequencer owns all state
All mutations go through `ExchangeStateMachine`, which validates, assigns the command sequence,
and calls `MatchingEngine`. The engine is not thread-safe by design. In R3, network threads only
enqueue parsed requests; one sequencer thread drains the queue. *Why:* deterministic acceptance
order, no lock contention on book data, simple replay (journal order == acceptance order).
*Cost:* matching throughput is bounded by one core, which is acceptable for this scope.

## D2. Price levels: one `std::map<PriceCents, PriceLevel>` type for both sides
Bids use the same ascending map as asks; the best bid is `std::prev(end())`. Using one map type
lets the active-order index store a `LevelMap::iterator` plus a `std::list` iterator for every
resting order, so cancellation is an average O(1) hash lookup, O(1) list erase, and O(1)
amortized level erase, with no second price lookup. Map and list iterators stay valid until
their own element is erased; a level is only erased after its FIFO list is empty, and an order's
index entry is erased before its list node. *Alternative:* `std::greater<>` bid map (spec
suggestion) — needs different iterator types per side. This is a correctness baseline, not a
claimed optimum.

## D3. Numeric IDs internally, opaque strings at the boundary
Orders are identified by the command sequence that accepted them (`ord-<seq>`) and executions
by the global execution sequence (`exe-<seq>`). The core stores `uint64_t`; strings are produced
only by codecs. Cancels parse `ord-<n>`; anything else is `UNKNOWN_ORDER`.

## D4. Only accepted requests are remembered for idempotency
A retry with an accepted `request_id` and identical body returns the original result (marked
`duplicate: true`) without consuming a sequence. A different body under the same ID returns
`DUPLICATE_REQUEST_ID_CONFLICT`. Rejected requests are not recorded, because they are not
journaled and therefore could not be reconstructed after restart. A rejected request may be
retried and can succeed later if state changed. The original response is rebuilt from the
stored command, its sequence, and its contiguous execution range, so the map stays compact.

## D5. Capacity limit is checked conservatively
`LIMIT_EXCEEDED` is returned for a new order when the configured open-order limit is already
reached, even if the order would fully fill. This keeps validation a pure precondition check
without a matching preview. Aggregates use checked arithmetic; bounds (price ≤ 1e8,
quantity ≤ 1e6, open orders ≤ configured limit) make overflow unreachable, and a violation is an
internal error (fail-stop), not silent wraparound.

## D6. Canonical state is hand-written deterministic JSON in the core
The core library has no JSON dependency. Canonical bytes are produced by a small writer with
fixed key order, arrays in sequence/price/FIFO order, and no hash-map iteration. SHA-256
(implemented in-tree, verified with NIST vectors) identifies states for tests; it is not a
security mechanism.

## D7. JSON only at the boundary (nlohmann/json 3.11.3, pinned by SHA-256)
`enginex_codec` converts JSON lines/frames to typed commands and typed results to JSON. Field
errors map to specific codes (missing/non-integer price → `INVALID_PRICE`, etc.).

## D8. R2 write path: validate → apply in memory → journal → sync → acknowledge
For each accepted command, `DurableExchange` runs `prepare` (pure validation), then `commit`
(apply to the in-memory state), then encodes the record into a user-space batch buffer. On
`flush()` it `write()`s the batch and synchronizes it. Responses are released only after the
sync succeeds. Applying before writing is safe because nothing is observable before the
acknowledgment. It also means an unexpected application failure (an `InvariantViolation`)
happens *before* the record reaches the journal, so the journal never holds a command whose
replay would fail. Any write, sync or apply failure puts the process into **fail-stop**: the
in-memory state may contain non-durable effects, so every later request gets
`JOURNAL_IO_ERROR` until a restart rebuilds state from the journal. A failed `fsync` is never
retried as if it succeeded.

## D9. JOURNAL_IO_ERROR means "outcome unknown", not "rejected"
If the write or sync fails, the record may or may not be on disk (a complete record whose sync
failed can still survive). The client is told the outcome is uncertain and must retry with
the same `request_id` after restart. If the record survived, replay rebuilt the request map
and the retry returns the original result. If it did not, the retry executes the command for
the first time. Either way there is exactly one logical mutation per request id. This is
idempotent command application, not exactly-once delivery.

## D10. Durability modes
* `sync` (default): the strongest flush the platform offers before acknowledging. That is
  `fdatasync` on Linux and `fcntl(F_FULLFSYNC)` on macOS, falling back to `fsync` if the
  filesystem rejects it. Plain `fsync` on macOS does not flush the drive's write cache.
* `fsync`: POSIX `fsync` only, for comparison benchmarks.
* `none`: acknowledge after `write()` returns. Survives a process crash (the data is in the
  kernel page cache) but not an OS crash or power loss.
New journal files are created with `O_EXCL`, the header is synced, and the parent directory is
fsynced so the directory entry is durable as well.

## D11. Recovery classification
Complete, CRC-valid, sequence-contiguous records are replayed. An incomplete *tail* is
recoverable: a partial 8-byte frame header, a payload shorter than its declared length, or an
all-zero tail (a common power-loss artifact). Under `sync` mode such a tail can never be an
acknowledged command, because acknowledgment requires the whole record to be synced. The
default policy preserves the tail bytes in `<journal>.tail-<offset>.bin` and truncates the
journal to the last complete record. These are hard errors that refuse startup
(`JOURNAL_CORRUPT`): bad magic, version or flags; a declared length of 0 or >4096 with
non-zero bytes; a CRC mismatch on a complete record; an undecodable payload; a sequence gap or
regression; and a replay divergence (the state machine does not accept the record exactly as
logged). Nothing is ever silently truncated in the middle of a file.

## D12. Single writer
The journal file is held with `flock(LOCK_EX | LOCK_NB)` for the lifetime of the writer, so a
second server on the same journal fails fast instead of interleaving appends. `replay
--verify` reads without taking the writer lock and never modifies the file unless
`--repair-tail` is given, which needs the exclusive lock.

## D13. R3 threading model: thread per connection around one sequencer
Each client connection gets a reader thread (decode frames, parse JSON, enqueue) and a writer
thread (drain that connection's bounded outbound queue). One acceptor thread enforces the
client cap. One sequencer thread is the only code that touches the state machine; it drains the
bounded ingress queue in batches. With the documented 64-client cap that is at most ~130
threads. This is simpler to reason about and test than an event loop, and the throughput
bottleneck is the single sequencer and the flush anyway. *Alternative:* a
`poll`/`epoll` reactor would scale to more connections; it is not needed for this scope.

## D14. Group commit with per-command events
The sequencer stages every command of a batch (validate, apply, encode), then makes **one**
journal write and **one** sync, then releases replies and market-data events in sequence
order. Replies are never sent before the sync that covers them, so `sync`-mode acknowledgment
semantics are unchanged from R2. Each command's `book_update` (changed price levels, best
bid/ask) is computed right after that command is applied, because levels after the batch would
mix in later commands. It is buffered with the reply. If the batch's flush fails, every
accepted reply in it becomes `JOURNAL_IO_ERROR` (outcome unknown), no events are published,
and the server is fail-stop.

## D15. Snapshot/subscribe handoff through the sequencer
`snapshot_request` and `subscribe` travel through the same ingress queue as commands. When the
sequencer reaches one, it first flushes and publishes the current batch, so the visible state is
exactly the durable, published state. It then serializes the snapshot with `event_watermark =
last published event_sequence`, enqueues it to that connection, and (for subscribe) registers
the subscriber, all on the sequencer thread. No event can fall between the snapshot and the
subscription, so the subscriber's first `book_update` is exactly `watermark + 1`. Every
accepted, non-duplicate command produces exactly one event, so `event_sequence ==
command_sequence`. The numbering is therefore stable across restarts. Subscribers receive all
symbols; there are no symbol filters.

## D16. Bounded queues and slow consumers
Ingress queue (default 4096 requests): when it is full, the reader immediately answers
`SERVER_OVERLOADED`. The request was not sequenced and is safe to retry. Per-connection
outbound queue (default 1024 frames): the sequencer only ever `try_push`es, so it never blocks.
When a push fails, the connection is marked overflowed, one final control frame is queued in
a reserved slot (`resync_required` for subscribers, `SERVER_OVERLOADED` error otherwise), the
subscription is dropped, and the writer closes the socket after flushing what is queued or
after its send timeout (default 2 s). Queued events are not discarded before the notice, so a
subscriber never sees a silent gap: the feed simply ends with an explicit resync
notice. The client must reconnect and subscribe again for a fresh snapshot.

## D17. Asymmetric frame limits
Client→server frames are limited to 32 KiB (spec §10.1) and the declared length is checked
before any allocation. Server→client frames may be up to 16 MiB, because a full-depth
subscribe snapshot of a large book can exceed 32 KiB. Clients enforce that limit.

## D18. Hardening after an independent review of the R3 server and journal
A read-only review found three defects, all fixed with regression tests:
1. **Idle subscriber leak.** Closed subscribers were pruned only when an event was published,
   so an idle market kept their sockets open. The sequencer now prunes on every loop
   iteration (`IdleDisconnectedSubscribersAreReleased`).
2. **Oversized frames.** An ack or event for a huge sweep could exceed the frame limit, and
   the resulting exception would terminate the process from the sequencer thread. Frames are
   now size-checked. Over the limit, they are sent without the execution list
   (`executions_truncated`), or the feeds are ended with `resync_required`
   (`event_too_large`). Any unexpected exception in the sequencer is caught and puts the
   server into fail-stop (`INTERNAL_ERROR`) instead of `std::terminate`.
3. **Length corruption near EOF.** This could be misclassified as a torn tail (see
   journaling.md §4). It is now rejected by validating the visible payload prefix.
Also adopted from the review's lower-confidence notes: an overall per-frame send deadline, so
a slowly draining peer cannot reset `SO_SNDTIMEO` forever; a non-blocking listener, so
`accept` cannot hang shutdown; and a back-off when descriptors run out.

## D19. Snapshot/subscribe failure paths (sprint P0)
A subscribe registers the connection only if its initial snapshot frame was actually built.
An oversized snapshot is an `INTERNAL_ERROR` reply ("request less depth") and the connection
stays unsubscribed. *Rejected alternative:* send a depth-limited baseline. A partial baseline
followed by full-depth increments diverges silently. Snapshot requests are refused after the
sequencer's internal fail-stop (`fatal_`), as subscribes already were. The in-memory state can
then contain staged commands that were never flushed, and a snapshot must not expose
non-durable state. Both were reproduced by failing tests first (`networking_test.cpp`), using
the test-only `ServerOptions::fault_internal_error_after_stage`.

## D20. ITCH is a separate system, not an input to the exchange
The ITCH path (`enginex_itch`, docs/itch.md) has its own types (`itch::Side`, `OrderRef`,
`Message`) and shares only SHA-256 with the core. *Rejected alternative:* convert ITCH adds
into `NewLimitOrder` commands and "replay the market" through the matching engine. That would
re-match orders the exchange already matched, invent executions and fills, and misrepresent
market data as order flow. Reconstruction applies the feed's own reported executions instead.

## D21. ITCH input framing; gap detection only where sequence numbers exist
Files use the 2-byte big-endian length-prefixed convention of Nasdaq's sample files. That
framing has no sequence numbers, and ITCH timestamps are not sequence numbers, so the file
path claims no gap detection. *(Updated in the sprint:)* MoldUDP64 sequencing was added later
as a separate layer (D25), and gap detection is claimed only there.
Framing errors are fatal (boundaries are lost after them); payload errors are per message.

## D22. ITCH anomaly policy: reject, count, never repair
Messages that contradict the reconstructed book (unknown or duplicate reference,
over-execution, zero shares, locate mismatch) are rejected with the book unchanged and
counted; `--strict` stops at the first. *Rejected alternatives:* clamping over-executions
(hides data errors), or auto-creating unknown orders (fabricates state). Locked and crossed
books are reported, not rejected, because historical feeds can show them legitimately.

## D23. ITCH order index: open addressing, chosen by measurement
`std::unordered_map` cost 0.48 heap allocations per message and dominated add/delete/replace
in a per-type cost breakdown (no sampling profiler can attach in this environment). An
open-addressing table (linear probing, Fibonacci hash, backward-shift deletion, load ≤ ½) cut
book apply from 52.5 to 32.2 ns/msg on the 1 M-message workload with identical state hashes.
It made the single worst latency worse (one full rehash during a doubling, 613 µs). An
optional capacity hint removes that (max 1.6 µs). The old index stays behind
`-DENGINEX_ITCH_STD_INDEX=ON` so the comparison is reproducible from one commit. Price levels
remain `std::map`: they account for the remaining 0.008 allocations/msg, and no measurement
has yet shown them to be the bottleneck.

## D24. Price-level nodes from a free-list arena; index capacity is a cache decision
A symbol-count sweep showed `std::map` level nodes costing 0.30 heap calls per message at
5000 symbols. A per-builder fixed-size free-list arena (block size fixed by the first
allocation, i.e. the map's node type; other sizes fall back to `operator new`) removed them:
70.6 → 56.1 ns/msg at 5000 symbols, no change at 8. Kept behind
`-DENGINEX_ITCH_STD_LEVEL_ALLOC=ON` for comparison (now effective only with
`-DENGINEX_ITCH_MAP_LEVELS=ON`, since levels default to sorted vectors). *Rejected for now:* replacing `std::map`
with a flat or array-indexed level structure. The remaining cost is cache misses that only a
real symbol and price distribution could justify optimizing for. The same sweep showed that
over-sizing the open-addressing index (32 MiB) costs more than it saves. The capacity hint
should track expected peak live orders.

## D25. MoldUDP64 sequencer: buffer, re-request, or stop; never skip
The sequencer releases messages strictly in sequence order. Duplicates and overlaps are
dropped. Packets ahead of a hole wait in a bounded reorder buffer while `missing()` ranges
are re-requested. *Rejected alternative:* skip the hole and continue, as a "best effort" feed
handler might. A reconstructed order book after a skipped message is silently wrong, which is
worse than stopping. When the buffer bound is exceeded the sequencer stops (`Failed`), and the
consumer must rebuild from a snapshot or the start of the session. Packet validation is
all-or-nothing, so a malformed packet cannot partially advance the sequence. There is no
socket receiver or wire re-request protocol; channels are simulated in tests. (Superseded in
part by D29: a loopback UDP transport now exists.)

## D26. Matching core: std::pmr pool for book nodes (optional R4, measured)
An allocation probe showed 2.0–3.5 heap calls per command, mostly list, index and level nodes
that are freed again on fill or cancel. The book now uses `std::pmr::list`, `std::pmr::map`
and `std::pmr::unordered_map`, all on one `std::pmr::unsynchronized_pool_resource` per
`MatchingEngine`. `PriceLevel` is allocator-aware, so levels created in a map inherit its
resource. *Why pmr rather than the ITCH path's fixed-size arena:* one resource serves every
node size and propagates through nested containers without custom allocator plumbing.
*Lifetime rule:* the pool lives on the heap, declared before the containers.
`MatchingEngine` may be move-constructed (containers keep pointing at the same pool) but not
move-assigned (the target's pool would die under its live containers). The compiler then
deleted `ExchangeStateMachine`'s unused move assignment. Result: +14–28 % throughput where
nodes are recycled, unchanged without recycling, identical state hashes, +5–6 MiB retained
memory. The pool never shrinks: freed blocks are kept for reuse until the engine is destroyed.
`-DENGINEX_CORE_STD_ALLOC=ON` restores `new_delete_resource` for comparisons. `OrderBook`
copies are deleted, because a copy would silently use the default resource. On macOS,
`std::pmr` pool resources need a libc++ deployment target new enough to ship them. The
default target here (the build host's SDK, macOS 26) is fine, but setting
`CMAKE_OSX_DEPLOYMENT_TARGET` much lower would fail to link. An independent review of this
change (lifetime, uses-allocator construction, iterator stability, determinism, exception
safety) found no defects.

## D27. ITCH book layout chosen on real data: sorted-vector levels, paged direct index
The synthetic workload had suggested the book was close to done. A 256 MiB prefix of a public
Nasdaq sample showed otherwise (docs/itch.md § Real Nasdaq sample). (1) 43 % of adds and
replaces open a new level (synthetic: 1.6 %) in books thousands of levels deep. Price levels
became a contiguous vector per side, sorted with the best price at the back, with Levels in a
stable deque pool. This is fast to search and cheap to modify near the top. (2) With 1.5 M
live orders, the hash index (64 MiB) costs a DRAM miss per lookup. A flat-array experiment
(not shipped) proved this: replay went 91 → 60 ns/msg. Order references are dense and mostly
increasing, so the shipped index pages them directly. Each page covers 4 096 references
(16 KiB), pages are created on demand and freed when empty, the page count is capped
(256 MiB), and an open-addressing table handles references ≥ 2³² or arriving over budget.
*Rejected:* a flat array (unbounded memory for large or adversarial references); a larger
hash load factor (still random DRAM access). Result on real data: replay 99.3 → 62.1 ns/msg
with an identical book. The earlier variants stay behind `-DENGINEX_ITCH_MAP_LEVELS=ON` and
`-DENGINEX_ITCH_HASH_INDEX=ON`. *Data handling:* the sample is used locally only, never
committed, and no build or test requires it (the real-data test skips unless
`ENGINEX_ITCH_REAL_FILE` is set).

## D28. Near-top linear scan before binary search for price levels (measured)
A temporary histogram on two real sample days showed that ≈ 70 % of level lookups (add,
replace, level removal) land within 4 entries of the best price and ≈ 80 % within 8.
`SideLevels::position()` scans those 8 entries linearly from the back of the sorted vector
(two cache lines) and binary-searches the remainder only on a miss. Interleaved A/B: real
replay −8 % on both days tested, synthetic 5000 symbols −6 %, 8 symbols unchanged. *Rejected:*
galloping search from the top (more branches for the same coverage), and a separate hot-top
array (a second structure to keep consistent). A mutation of the scan's comparison fails
many book tests.

## D29. MoldUDP64 over UDP: one sender that also answers re-requests; loopback by default
`itch mold-send` / `itch mold-recv` carry MoldUDP64 over real UDP sockets. The sender packs a
file's frames (byte for byte, no re-encoding) into MTU-bounded packets (`PacketStore`, shared
with the simulator), paces the live feed, repeats end-of-session, and answers 20-byte
requests from the same store. The receiver feeds the real `Sequencer`, re-requests
`missing()` ranges and rebuilds the books. Decisions, each driven by a measurement or a
failure:
* **Retry timer per requested range.** The first receiver re-requested every hole every round. Answers
  in flight were requested again, the answers snowballed, and a lossless 300 k packets/s
  run took 305 s and failed with 1.8 M requests and 90 M duplicate messages (console run). A requested range
  is now asked again only after `retry_after` (50 ms). An independent review then found that
  keying this by hole start let a partly answered hole look new. `RequestPlanner` keys it by
  requested range. Retransmitted packets at 50 k packets/s with 5 % / 10 % loss on 2019-03-27
  went from 53 311 (`7c2ae17`) to 42 503 (`bfd47a1`) in `mold-udp-loopback-macos-m3.csv`
  (single runs).
* **Connected sockets on hot paths.** Here an unconnected `sendto()` costs ≈ 69 µs and a
  connected `send()` ≈ 1.7 µs (`enginex_udp_syscall_bench`). The live feed and the
  receiver's requests use connected sockets. Answers go out on per-peer connected sockets
  bound to the request port (`SO_REUSEPORT`), so the receiver sees them come from the
  address it asked.
* **No reflection, filtered sources** (from a security review). Answers go only to the
  receiver's IP, within a token-bucket budget, and capped per request. The receiver accepts
  feed datagrams only from the feed's IP. `--allow-remote` requires an explicit `--session`.
  MoldUDP64 has no authentication, and neither does this.
* **Configured session.** Without it, the first datagram fixes the session, so a stray
  packet that arrives first would lock out the real feed (a test shows this).
*Rejected / not done:* multicast and a separate re-request server (the realistic topology, but
loopback unicast exercises the same sequencing and recovery logic), SoupBinTCP, and kernel
bypass (out of scope; see the mandate).

## D30. Reorder buffer: sequence-indexed window over packet copies, holes kept explicitly
The sequencer used to buffer each out-of-order message as its own `std::map` node with a
`std::vector` copy. `enginex_mold_bench` measured 121–147 ns/msg behind a hole against 4 ns
in order. A UDP receiver under steady loss is almost always behind a hole, so it fell behind
the feed and failed (5 % loss: exact at 20 k packets/s, failure at 50 k; console runs). Now a `std::deque`
of 12-byte slots, indexed by `sequence − next`, points into one copy per packet. Copies are
recycled with their capacity, and the capacity a copy retains counts against the byte bound
(a security review found the first version could exceed it). Missing ranges live in a map of
holes that changes only when a hole opens or a retransmission fills one, so `missing()` is
O(holes). Interleaved A/B (`mold-sequencer-macos-m3.csv`): 20–21 ns/msg behind holes, in order
unchanged. The semantics are unchanged: all MoldUDP64 tests pass as before, and the set-model
differential test reports identical counts. *Rejected:* storing whole packets keyed by first
sequence, because overlapping retransmissions with different packet boundaries would need
interval logic in the duplicate check.

## D31. Exchange core: request-id index as a keyed flat table over a record deque
Idempotency keeps every accepted command's record for the life of the process (retries must
return the original result, also after a restart). Measured: the per-command cost of `mixed`
grew from 108 ns at 100 k commands to 296 ns at 3 M, with a tiny book. The harness already
reserves capacity before timing, so rehashing was never in the measured region. An isolated
probe put a miss in the node-based `unordered_map` at ≈ 75 ns
against ≈ 23 ns for a flat table. Records now sit in a `std::deque` indexed by sequence.
Stable references remove the separate `by_sequence_` pointer vector and the duplicated key
string. `request_id` → record is an open-addressing table of 8-byte slots
`{32-bit fingerprint, index + 1}` at load ≤ ½. Probes start from the fingerprint, so growth
copies slots without re-hashing strings, and a full string compare confirms every match. The
index grows before a record is appended, and placing a slot cannot fail, so no record is ever
left unindexed. A security review then noted that client-chosen ids with an unkeyed hash
allow probe-cluster flooding (the old map had the same exposure). The fingerprint is
SipHash-2-4 with a random key per instance. Placement depends on the key, but results never
do: a test runs two machines with different keys in lockstep, and the state hashes are
identical across all benchmark rows. Result: 1 M commands 198 → 119 ns/cmd, 3 M 296 → 196,
p99 roughly halved, RSS −15 % (benchmarks.md). Index growth is a stop-the-world doubling
inside one command, as the old map's rehash was. The core benchmark pre-reserves (harness
`reserve()`), so its tails exclude growth in both builds. Recovery replay and the server do
not reserve, so their timings include it. *Rejected:* a plain `std::hash` fingerprint
(floodable), and evicting old ids (would break idempotency across the session, which the spec
requires). `reserve()` was never an alternative: the benchmark already used it.

## D32. Bounded level vector with a deep map tier (adversarial worst case)
The sorted-vector price levels (D27) were known to have no adversarial bound. Measured with
`tools/itch_adversarial.py` on one 200 k-level side whose every add opens a level worse than
all others: 25 130 ns/msg, against 141 ns for the `std::map` variant (`itch-level-bound-macos-m3.csv`).
Each side's vector now holds at most `level_vector_limit` best levels (default 8 192; the
deepest side in the real samples has 3 831). Worse levels live in a `std::map`. Every deep rank
is below every vector rank, so best-first iteration is the vector, then the map. Overflow
moves the vector's worst half to the map in one batch; an emptied vector takes back the map's
best half. Any operation is O(limit + log n), amortized. Hostile pattern 25 130 → 96 ns/msg; a
200 k-level monotone-improving book 17 → 54 ns/msg (it keeps spilling). On real data the
change measured as "unchanged within noise" at first. A later 20-pair gate (benchmarks.md) put
it at ≈ 3 % slower in book apply, with median ratio 1.031, slower in 14 of 20 pairs, p = 0.12:
not significant alone, but consistent with the ≈ 7 % end-to-end regression found since
`562c56d` at that load (≈ 5 % on a quiet host, D35). On a quiet host it measured 1.021 (17 of 20, p = 0.003; D35's bisect). That cost
is accepted for the bound. Moving the map behind a pointer did not
recover it. The bound's own worst case was found and measured later. 8 191 levels with an
empty map, then alternating add and delete just below the worst level, moves the whole
vector on every message: 1 583 ns/msg (`itch_adversarial.py N churn`). That is O(limit) per
message, as designed. Correctness: the reference-model differential at limits 2, 3 and 8 (constant
spill/refill), a hostile deep-book test, and identical state hashes on a full real day at
limits 2, 16 and 8 192. An independent review found no defect. Its remaining notes concern
allocation failure in the middle of a spill or insert, which falls under the existing
`apply()` contract (after `bad_alloc` the builder must be discarded). *Rejected:* switching the default to `std::map` (on real data the sorted vector measured
8 % faster, D27 and benchmarks.md); a balanced tree or B-tree for all levels (more code for no real-data gain);
switching a whole side to a map once it gets deep (the top of a deep book would lose the
contiguous fast path).

## D33. Repeated JSON keys are malformed input

**Problem.** A hostile-input pass sent a command that named `request_id` twice. The parser
(`nlohmann::ordered_json`) kept the last value without comment, and the command was accepted.
Readers of such a message can legitimately disagree: a proxy, a logger or another
implementation may keep the first value while EngineX keeps the last. For the idempotency
key, that means two parties attribute the same bytes to different requests.

**Decision.** `codec::parse_json` uses the parser's callback to track the keys of each open
object. Any repeat makes the message `INVALID_REQUEST` ("repeated field"), both on the wire
and in `simulate`/`run` input. The error echoes no `request_id`, because which one is meant is
the ambiguity. Tests: `JsonCodec.RepeatedKeysAreRejected` and `Protocol.RejectsBadMessages`;
both failed before the change.

**Cost.** `simulate` on 200 k commands took a median of 1.510 s against 1.451 s for the same
commit without the check (5 runs each, interleaved, same state hash). That is about 4 %, or
≈ 0.3 µs per command, small next to a TCP round trip. *Rejected:* first-value-wins (still
ambiguous to anyone who keeps the last); a custom SAX parser (more code than the callback
for the same result).

*Addendum (key count).* The complexity audit behind D34 also found that `ordered_json`
inserts each key with a linear scan, so parsing one object with k keys costs O(k²): 40 000
keys took 1.39 s in one `simulate` line, and a 32 KiB TCP frame can hold a few thousand. The
same callback now refuses a 65th key in any object, and returning `false` discards the pair
before insertion. The message is rejected with "too many fields in one object". Valid
messages have at most 8 fields. Measured after the change: 40 000 keys 0.012 s, 1 000 000 keys
(an 11 MB line) 0.10 s. Test: `JsonCodec.ObjectsWithTooManyKeysAreRejected`.

## D34. Keyed hash for the ITCH order-reference fallback index

**Problem.** An independent complexity audit pointed out that `OrderIndex` placed keys by the
top bits of `ref × 2⁶⁴/φ`. That multiplier is odd and public, so it can be inverted.
References `(C + i)·φ⁻¹ mod 2⁶⁴` all share one home bucket at every table size. They are
≥ 2³², so the default paged index sends them to its `OrderIndex` fallback. Linear probing
then turns every insert, and every miss that falls through to the fallback, into a scan of
the cluster.

Measured with `tools/itch_adversarial.py N hashflood` (book apply, 1 trial):

| crafted adds | before | after |
|---:|---:|---:|
| 5 000 | 5.4 µs/msg | 83 ns/msg |
| 40 000 | 43 µs/msg | 84 ns/msg |
| 1 000 000 | not run (≈ 1 ms/msg extrapolated) | 114 ns/msg |

Before the change the cost per message doubled with N, so total work was quadratic.

**Decision.** Same as D31: SipHash-2-4 of the reference's little-endian bytes under a random
per-instance key (`std::random_device`, fail-closed). Placement never affects results:
nothing iterates the table for output, and the state hash of a real day is unchanged. Real
data never pays for it. Its references are below 2³² and fit the page budget, and the paged
index does not consult an empty fallback. Tests that build collision chains pass a fixed
key. `OrderIndex.CraftedReferencesDoNotShareAHomeBucket` checks that the crafted references
spread over the table.

*Rejected:* a random odd multiplier for multiply-shift. It is as cheap as the original, but
linear probing needs more independence than a 2-universal family gives, so structured key
sets could still cluster. *Not changed:* the `-DENGINEX_ITCH_STD_INDEX` A/B baseline
(`std::unordered_map` with the identity hash) is still floodable. It is a benchmark
comparison build, not the default.

*Correction (2026-11-11).* "Real data never pays for it" was wrong. On a quiet host (load
≈ 2), the 20-pair apply gate on 2019-03-27 put D34 alone (`eb2442f` → `9c8f48a`) at a median
ratio of 1.028 (p = 0.04), and 1.027 on a repeat (p = 0.012). The SipHash code never runs on
real data, but it was inlined into the paged index's hot paths. D35 removes that cost.

## D35. The hash fallback is out of line (cold)

**Problem.** A quiet-host bisect of the ≈ 5 % real-data apply regression since `562c56d` (all
binaries built the same way from `git archive` exports, 20 pairs per step, identical state
hashes) attributed it as follows:

| step | median ratio | slower in | p |
|---|---:|---:|---:|
| total, `562c56d` → `12c661a` | 1.050 | 19/20 | 4e-5 |
| lazy node construction (`dd78a8d`) | 1.001 | 11/20 | 0.82 |
| D32 bounded level vector (`b7f750e`) | 1.021 | 17/20 | 0.003 |
| bounds commits up to `eb2442f` | 0.997 | 4/20 | 0.01 (effect below 2 %) |
| D34 keyed fallback (`9c8f48a`), two runs | 1.028 / 1.027 | 15/20, 16/20 | 0.04 / 0.012 |
| memory accounting and later, up to `12c661a` | 1.006 | 14/20 | 0.12 |

The steps multiply to ≈ 1.05. D34's share is surprising, because real data never reaches
the fallback: `find` returns before consulting an empty fallback. The cost came from inlining
SipHash into `PagedIndex::find`, `insert` and `erase`. That made the hot paths bigger, though
the branch is never taken.

**Decision.** The three fallback calls go through private helpers marked
`[[gnu::noinline, gnu::cold]]` (a macro, empty on other compilers). Same semantics, same hash.
Paired gates against the `12c661a` archive build (20 pairs each):

| workload | median ratio | IQR | candidate faster in | ns/msg |
|---|---:|---:|---:|---|
| apply, 2019-03-27 | 0.969 | 0.961–0.992 | 17/20 | 43.2 → 41.9 |
| apply, 2019-03-27, repeat | 0.964 | 0.956–0.966 | 19/20 | 43.3 → 41.7 |
| apply, 2020-01-30 | 0.970 | 0.952–0.975 | 20/20 | 46.2 → 44.7 |
| full replay, 2019-03-27 | 0.969 | 0.966–0.976 | 19/20 | 55.7 → 54.1 |
| adversarial `hashflood-lifecycle` 200 k (fallback in use) | **1.045** | 1.042–1.054 | 0/20 | 69.6 → 72.7 |

Net, against the `562c56d` archive build (apply, 2019-03-27): median ratio 1.012, IQR
0.991–1.026, slower in 14 of 20, p = 0.12. That is **no measurable difference**: the
regression that remains is below what the gate resolves, consistent with D32's ≈ 2 %.

*Tradeoff accepted:* the attack input, whose cost D34 bounded, is now ≈ 4.5 % slower, because
the fallback is a real call. It stays O(1) per message at ≈ 73 ns, against 5.4–43 µs before
D34. Real data is the workload that matters, and it gains ≈ 3 %, recovering D34's cost.
*Tried and rejected:* the same treatment for D32's deep tier. The `std::map` branches of
`get_or_create` and `erase`, plus `spill` and `refill`, went into cold out-of-line helpers.
Against `b50a73f`: apply 1.006 (p = 0.50) on 2019-03-27 and 0.998 (p = 0.82) on 2020-01-30,
so no measurable difference. D32's ≈ 2 % has some other cause, so the change was reverted
(patch `itch-d32-cold-deep-tier-rejected.patch`, pairs
`itch-d32-cold-deep-tier-rejected-macos-m3.csv`). It is the accepted price of the worst-case
bound.

## D36. Release CI fixes and GCC flow-sensitive warnings

**Context.** The sprint was written without GCC, Linux sanitizers or access to CI results.
When the logs could be read, every sprint run had failed. The causes and fixes are listed in
[linux-validation.md](linux-validation.md#how-it-got-there). Three of the fixes change how
the build checks the code, so they are recorded here.

**Decisions.**

* *GCC flow-sensitive warnings print but do not fail the build* (`3625d9a`,
  `cmake/Warnings.cmake`). Under GCC only, `maybe-uninitialized`, `stringop-overflow`,
  `array-bounds`, `null-dereference` and `dangling-reference` are passed as
  `-Wno-error=…`. They stay enabled and appear in the log. These warnings depend on the
  optimizer, and GCC 13 at `-O3` reported two `-Wnull-dereference` false positives in test
  code (`tests/itch_codec_test.cpp`, a vector sized by `*message_length(t)` and then
  indexed at 0). Every other warning is still an error under GCC and Clang, and Clang
  builds keep `-Wnull-dereference` fatal. *Cost:* a real GCC-only finding of these five
  kinds would not stop CI. So the GCC Release log is read at release time. The passing
  run (43115996592) contains exactly those two warnings and no others.
* *Sanitizer builds use a 600 s per-test timeout, other builds 120 s* (`ae20847`).
  `ProtocolFuzz.HostileRequestsNeverBreakTheCommandPath` took 165 s under ASan+UBSan on
  the hosted runner. Inputs and assertions are unchanged.
* *TSan builds skip `enginex_core_alloc_probe` and `enginex_itch_bench`* (`ae20847`). Both
  replace the global `operator new`/`delete` to count allocations, which does not link
  against TSan's interceptors. They are measurement tools, not tests, and every test target
  still builds and runs under TSan.

**Rejected:** dropping the slow test from sanitizer jobs, lowering its input count further,
or turning off `-Werror` for GCC.

## D37. The partial-frame deadline is per frame

**Problem.** An independent review during the release audit found that the slowloris
deadline (`partial_frame_timeout`) was measured from the first read that left a partial
frame, and reset only when a read left none. A client that kept completing frames, but
whose reads each ended inside the next frame, was disconnected after one deadline. This
happens with a pipelining client on a network that splits frames at arbitrary points. A
regression test reproduced it: with a 600 ms deadline and one frame completed every 50 ms,
the connection was closed at the 14th write, one deadline after the first partial read.

**Decision.** The deadline restarts whenever a read completes at least one frame, so it
bounds each frame from the read in which that frame started. A frame that never completes
(the slowloris pattern) is still cut off at the deadline. Tests:
`Server.PipelinedFramesSplitAcrossReadsAreNotTimedOut` (new) and
`Server.TrickledFrameIsDisconnectedAtDeadline` (unchanged). *Tradeoff:* a client can keep a
connection open by completing one frame per deadline. That is ordinary low-rate traffic.
Connections are capped (`--max-clients`), and each partial frame is bounded by the frame
limit.
