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
`-DENGINEX_ITCH_STD_LEVEL_ALLOC=ON` for comparison. *Rejected for now:* replacing `std::map`
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
socket receiver or wire re-request protocol; channels are simulated in tests.

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
