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
