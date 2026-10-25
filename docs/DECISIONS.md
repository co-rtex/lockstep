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
