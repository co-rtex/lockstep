# TradeGrid EngineX — Complete Project Specification and Build Handoff

**Status:** Proposed project; the specifications below describe what to build, not what already exists.  
**Specification version:** 1.0  
**Prepared:** October 25, 2026  
**Primary owner:** One developer  
**Primary objective:** Produce a convincing, working C++ systems-engineering artifact quickly for JPMorgan Chicago software-engineering internship team matching; then extend it into a technically rigorous portfolio project for Big Tech, quant-firm SWE, and fintech recruiting.  
**Time horizon:** Release 1 as soon as correctly implemented; releases 2–3 as follow-on work. An aggressive 10–14-day sprint is possible but not a guaranteed deadline.  
**Project type:** Standalone C++20 trading-system simulator, correctness/benchmarking platform, event journal, and eventually networked market-data service. **Not a real exchange, not investment software, not an automated live trader.**

---

## 0. Read this first: the complete decision in one page

Build **EngineX**, an independent C++ trading-systems laboratory. Its minimum useful output is a deterministic, price-time-priority limit-order-book matching engine with automated tests, reproducible benchmarks, and a README that demonstrates C++ ownership. The advanced version adds a versioned append-only write-ahead journal, deterministic replay, fail-stop recovery, a bounded concurrent input boundary, and a sequenced TCP market-data publisher with snapshot-based resynchronization.

EngineX must **never silently replace the existing Go TradeGrid matching or exchange service**. Development starts in an independent personal repository. Once stable, the owner may propose a self-contained `systems-lab/enginex/` subtree to the club. Integration with live TradeGrid is **optional, read-only first, and requires club design review**. Other student contributors must be able to finish TradeGrid without building or running EngineX.

### Shipping strategy

| Release | Must ship | Purpose | Ready for résumé? |
| --- | --- | --- | --- |
| **R1 — Core** | C++20 matcher, order entry/cancel, tests, shared-case comparison, repeatable benchmark, README, CI | Immediate JPMorgan team-matching signal | **Yes, once independently complete and verified** |
| **R2 — Durable** | Versioned command journal, validated append, synchronization policy, replay, duplicate request handling, crash and corruption tests | Demonstrate reliability and state-machine reasoning | Yes, update bullets after tests pass |
| **R3 — Networked** | TCP ingress/feed, bounded queues, global event ordering, snapshots/reconnect, backpressure tests, full benchmark suite | Broader infrastructure/quant SWE signal | Yes, once complete |
| **R4 — Optional** | Profiling-driven allocator or layout changes, advanced queue structures, Linux perf analysis, optional Go adapter | Deeper systems specialization | Only if evidence supports it |

**Nonnegotiable constraints:** Correctness before speed; concrete test evidence before résumé claims; no production financial trading; no external market-data dependencies; preserve club's Go backend and contracts; don't add distributed technologies solely for buzzwords; don't claim exact-once network delivery, crash durability, or performance improvements without tests establishing the precise guarantee.

---

## 1. Why this project exists

### 1.1 Immediate use case

The owner is preparing for JPMorgan's Summer 2027 Software Engineer internship matching, ideally in Chicago. Interests include CIB eTrading / low-latency C++, Securitized Products Group / fixed-income trading technology, Credit Pre-Trade, Cash Prime, CIB Go/platform engineering, Payments Performance Engineering, and distributed data/platform engineering. Team placement is not guaranteed and the project must not be presented as a promise of acceptance.

### 1.2 Longer-term use case

The same project supports learning and interviews for:

- Trading systems, market-data distribution, quant-firm SWE, performance-sensitive C++.
- General backend and infrastructure SWE through consistency, reliability, observability, messaging, concurrency and networking.
- Fintech systems through ordered processing, deterministic state transitions, auditability, retry/idempotency design and financial-data correctness.

It does **not** replace interview preparation for DSA, fundamentals, debugging, communication, or behavioral interviews. It is also not a direct substitute for ML, frontend, mobile, security-specialist or production SRE experience.

### 1.3 Intended demo

A reviewer should be able to clone the repository, build it using documented commands, run tests, execute a deterministic trading scenario, view the final book and fills, run a benchmark that produces machine-readable results, and—after R2—kill/restart a journaled session to demonstrate recovery. After R3, a second terminal can subscribe to a TCP feed and watch sequenced execution/book updates. A screenshot or polished dashboard is **not** required.

### 1.4 What success looks like

A completed R1 demonstrates personally written C++ with repeatable tests and measured performance. A completed R2 demonstrates understanding of durability and deterministic replay. A completed R3 demonstrates event processing under load, ordering, wire protocols, bounded buffers, and subscriber recovery. There should be defensible architecture notes explaining *why* each choice was made.

---

## 2. Relationship to the existing TradeGrid club project

The publicly accessible reference repository is **https://github.com/co-rtex/tradegrid**. As examined October 25, 2026, it contains:

- `backend/internal/matching/engine.go`: Go price-time-priority matcher.
- `backend/internal/matching/types.go`: accepted limit order and execution structures.
- `backend/internal/exchange/service.go`: serialized exchange service, state cloning and atomic state publication.
- `backend/internal/orders`, `risk`, `accounts`, `executions`: Go domain modules and tests.
- `docs/matching-engine.md`: language-independent behavior specification.
- `docs/api.md`: Sprint 2 API and exchange contract.
- `docs/fixtures/`: reproducible shared trading scenarios.
- `docs/architecture.md`, `docs/roadmap.md`: architecture principles and club roadmap.
- `.github/workflows/ci.yml`, `CONTRIBUTING.md`: current testing/review workflows.

The club's Sprint 2 goal is a simple **single-process Go exchange** with a React frontend, in-memory order processing, basic risk/reservations, and PostgreSQL schema work. Its published contract explicitly places order cancellation/amendment, WebSockets and runtime persistence in later milestones. The existence of a Go design **does not** mean those future milestones are already complete.

**Hard integration rule:** EngineX is an experiment/reference alternative, not a replacement for Go. Do not change Go matching semantics, risk/account code, API routes, database schema, frontend, or club CI until a separate approved integration issue requires it. Existing Go behavior must remain independent of EngineX.

**Approved first interaction:** Read the public spec and consume/copy test *expectations* using attribution. Implement EngineX logic independently. Translate common fixtures for differential tests; don't assume the Go and C++ services have identical domain scope.

**Potential future interaction:** Publish a narrowly specified optional adapter that **observes committed events** emitted by Go after its authoritative transaction succeeds; it must never consume or modify Go's live order-processing state. If Go has no stable committed-event feed, do not invent one or poll snapshots and claim it is lossless event streaming.

**Governance:** Personal repo first; submit an integration proposal and focused PR to add `systems-lab/enginex/` only after R1 works. Follow TradeGrid's issue, branch, PR, CI and review requirements. Be explicit about individual ownership and any club-derived specifications.

---

## 3. Scope, releases and exclusions

### 3.1 Functional scope

**R1**: Submit limit buy/sell orders; match at price-time priority; handle partial/full fills; rest unfilled remainder; cancel outstanding orders; inspect top-of-book/full depth; inspect executions; reject malformed commands without state changes; deterministic order sequencing; generate synthetic workloads; correctness and performance tests.

**R2**: Durable append-only log of accepted mutating commands; recovery from a journal; per-request idempotency; reproducible command/event identities; startup verification and explicit handling of incomplete/truncated tails; fault-injection/crash tests; document durability limits.

**R3**: TCP length-framed protocol; multiple clients; ordered request sequencing; asynchronously published committed market-data events; snapshot/watermark handshake; bounded queues and defined slow-consumer disconnect; reconnect/resync; network/failure benchmarks.

**R4 optional**: Compare alternative structures, perform allocation tuning, Linux `perf`, flamegraphs, network optimizations, and a read-only Go adapter.

### 3.2 Explicit exclusions

Do not build: live brokerage connectivity, real money, exchange regulatory features, margin/credit, short selling, a production risk engine, institutional market feeds, sophisticated pricing models, auth/multi-tenant security, a React dashboard, HFT kernel-bypass networking, FPGA, multi-node consensus, Kafka/Kubernetes/cloud orchestration, distributed exactly-once processing, or a commercial-grade matching exchange. These are separate projects, not R1–R3 criteria.

**Compatibility note:** TradeGrid's Go exchange currently applies account/risk/settlement rules. EngineX R1 is specifically a **matching core**, not a drop-in replacement for the complete Go exchange. Do not conflate the two in performance comparisons or marketing.

### 3.3 Explicit invariants

1. One logical sequence of accepted mutating commands determines outcomes; timestamps never determine FIFO priority.
2. All prices and quantities are fixed-width signed integers; never store price as floating point.
3. For each book, bids are ordered descending by price and asks ascending; within one price level, resting orders are FIFO.
4. An incoming buy trades against the lowest eligible ask; an incoming sell against the highest eligible bid.
5. A match executes at the **resting order's** limit price and at the minimum of remaining quantities.
6. Each execution has strictly positive integer quantity; no order's remaining quantity becomes negative.
7. A partial resting order retains its original FIFO position.
8. No executable crossed bid/ask combination remains after a command completes.
9. Rejected commands do not advance accepted-command sequence and do not mutate the book.
10. Idempotent request retries cannot produce duplicate accepted commands or fills.
11. Replay of accepted commands from an empty state reproduces the same canonical state, execution IDs and sequence numbers.
12. A failure in persistence never results in an acknowledged-but-unrecorded durable command under `sync` mode, assuming the documented storage guarantees and healthy device behavior.
13. Slow network clients cannot block the matching core indefinitely.
14. No code outside the authoritative sequencer mutates order-book state.

---

## 4. Technology choices and toolchain

| Area | Default decision | Rationale |
| --- | --- | --- |
| Language | **C++20** | Modern language facilities, ownership and performance work |
| Development | macOS supported; **Linux CI and durability validation required** | Local convenience plus credible OS/perf testing |
| Compiler | Clang or GCC with C++20 support | Portability and toolchain comparisons |
| Build | CMake >= 3.24 + Ninja recommended | Repeatable builds |
| Testing | GoogleTest through pinned FetchContent or vendored dependency; CTest | Widely used C++ tests, easily automated |
| Core data structures | `std::map` price levels, `std::list` FIFO queues, `std::unordered_map` order lookup | Clear complexity and iterator ownership |
| Benchmarks | In-project `std::chrono::steady_clock` harness; optional Google Benchmark | Minimal dependencies and reproducible workloads |
| Network | POSIX TCP sockets for R3; length-prefixed JSON framing | Explicit framing without misleading low-latency claims |
| Journal | Binary framed records with checksum, deterministic command payload | Validate corruption and replay |
| Dependencies | JSON parser only at command/transport boundaries (e.g., pinned nlohmann/json) | Keep the hot-path domain layer dependency-light |
| CI | GitHub Actions Ubuntu: Debug/Release, tests, sanitizer jobs, benchmark smoke test | Public verification |
| Formatting | clang-format, warnings-as-errors in owned code | Consistency |
| Static/dynamic checks | AddressSanitizer + UndefinedBehaviorSanitizer; ThreadSanitizer in a separate build where supported | Memory and concurrency defects |

Pin dependency **versions or commit hashes**; do not fetch random moving branches in CI. Avoid arbitrary claimed optimization flags such as `-march=native` in portable CI binaries; allow it in explicitly identified local benchmarks.

Useful documentation: https://cmake.org/cmake/help/latest/guide/tutorial/ , https://google.github.io/googletest/quickstart-cmake.html , https://clang.llvm.org/docs/AddressSanitizer.html , https://clang.llvm.org/docs/ThreadSanitizer.html .

---

## 5. Repository layout and build artifacts

Start as `enginex/` in the owner's GitHub account. Proposed layout:

```text
enginex/
  README.md
  LICENSE
  CMakeLists.txt
  CMakePresets.json
  .clang-format
  .gitignore
  .github/workflows/ci.yml
  docs/
    SPEC.md                      # This contract, copied/linked
    architecture.md              # Components and interaction diagrams
    protocol.md                  # R3 wire format and connection model
    journaling.md                # R2 durability contract / recovery
    benchmarks.md                # Hardware, flags, workloads, raw results
    verification.md              # Test matrix, known limitations
    tradegrid-compatibility.md   # References and behavior differences
  include/enginex/
    types.hpp
    engine.hpp
    order_book.hpp
    state.hpp
    journal.hpp
    protocol.hpp
    metrics.hpp
  src/
    engine.cpp
    order_book.cpp
    state.cpp
    journal.cpp
    protocol.cpp
    main.cpp
  tests/
    matching_test.cpp
    cancel_test.cpp
    invariants_test.cpp
    fixtures_test.cpp
    journal_test.cpp
    recovery_test.cpp
    idempotency_test.cpp
    networking_test.cpp
    testdata/
      no_match.jsonl
      full_fill.jsonl
      partial_fill.jsonl
      fifo.jsonl
      reconnect.jsonl
  benchmarks/
    bench_main.cpp
    scenarios.cpp
    README.md
  tools/
    generate_workload.py
    compare_go_fixtures.py
    plot_benchmarks.py
    fault_injection.py
  examples/
    demo_orders.jsonl
    expected_demo_results.json
  scripts/
    check.sh
    demo.sh
```

**Build targets** (keep libraries independently testable):

- `enginex_core`: pure matching and state transitions.
- `enginex_journal`: persistence and replay (R2).
- `enginex_net`: TCP I/O and wire encoding (R3).
- `enginex`: command-line executable.
- `enginex_tests`: test executable.
- `enginex_bench`: benchmark executable.

Keep generated files, binary journals, massive benchmark datasets, `.env` secrets, and local build directories out of Git. Commit small representative fixtures and summarized benchmark CSVs only.

### 5.1 Developer commands (the implementation must support equivalent commands)

```bash
# Prerequisites on development machine: C++20-capable compiler, CMake, Ninja, Python 3
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENGINEX_ENABLE_TESTS=ON
cmake --build build/debug --parallel
ctest --test-dir build/debug --output-on-failure

cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DENGINEX_ENABLE_TESTS=ON
cmake --build build/release --parallel

./build/release/enginex simulate --input examples/demo_orders.jsonl --output demo-results.json
./build/release/enginex benchmark --seed 42 --orders 100000 --scenario mixed --out benchmarks/results.csv

# After R2:
./build/release/enginex replay --journal data/demo.exj --verify

# After R3:
./build/release/enginex serve --port 9000 --journal data/demo.exj --durability sync
```

**Note:** These are the required eventual CLI contracts, **not commands that already work**. Implement and verify them as each release is completed. If a CLI implementation differs, update its tests, usage text and examples consistently.

---

## 6. Public domain model and types

Use strong integer aliases, documented ownership and clear API boundaries.

```cpp
using PriceCents = std::int64_t;
using Quantity   = std::int64_t;
using CommandSeq = std::uint64_t;
using ExecSeq    = std::uint64_t;
using EventSeq   = std::uint64_t;

enum class Side { Buy, Sell };
enum class OrderStatus { Open, PartiallyFilled, Filled, Cancelled };

struct NewLimitOrder {
    std::string request_id;   // caller-supplied idempotency key
    std::string symbol;       // initially AAPL or MSFT
    Side side;
    PriceCents price;
    Quantity quantity;
};

struct CancelOrder {
    std::string request_id;
    std::string order_id;    // assigned during prior accepted NewLimitOrder
};

struct Execution {
    std::string execution_id; // globally deterministic, e.g., exe-1
    std::string incoming_order_id;
    std::string resting_order_id;
    std::string symbol;
    PriceCents price;
    Quantity quantity;
    CommandSeq command_sequence;
    ExecSeq execution_sequence;
};

struct BookLevel {
    PriceCents price;
    Quantity aggregate_quantity;
    std::uint32_t resting_order_count;
};
```

A command may produce zero or multiple execution records. A returned response includes request ID, success/error, committed command sequence when applicable, generated order ID when applicable, fills, remaining quantity, and canonical state watermark. **No wall-clock timestamp determines matching order**; optional wall-clock metadata is kept outside replay-critical state.

### 6.1 Constraints for R1

- `request_id`: nonempty UTF-8 string, max 64 bytes; must be unique per logical mutating request.
- `symbol`: initially exact uppercase `AAPL` or `MSFT` (consistent with club's current demo symbol catalog); configure extension later without breaking fixtures.
- `price`: integer US cents, 1 through 100,000,000.
- `quantity`: integer whole units, 1 through 1,000,000.
- Check multiplication and aggregation with safe, overflow-checked arithmetic. Reject invalid data before mutations.
- `order_id` is generated from the accepted command sequence, for example `ord-<sequence>`; `execution_id` from global monotonically increasing execution sequence. Keep ID rules internal; expose opaque strings to clients.
- No auth, trading accounts, portfolio, cash balance or position constraints in R1. These are *not* implemented merely because the club's Go service has related modules.
- All executable inputs use LIMIT orders; no MARKET, STOP, IOC/FOK, iceberg, replacement/amendment or hidden liquidity.
- Cancellation is allowed only for currently resting orders. A fresh cancellation request for an unknown/filled/already-cancelled ID fails without changing state. A retry with the *same* accepted `request_id` returns its original result without repeating the cancellation.

### 6.2 Error codes

Use stable machine-readable codes: `INVALID_REQUEST`, `INVALID_SYMBOL`, `INVALID_SIDE`, `INVALID_PRICE`, `INVALID_QUANTITY`, `DUPLICATE_REQUEST_ID_CONFLICT`, `UNKNOWN_ORDER`, `ORDER_NOT_OPEN`, `LIMIT_EXCEEDED`, `JOURNAL_IO_ERROR`, `JOURNAL_CORRUPT`, `SERVER_OVERLOADED`, `INTERNAL_ERROR`. Add a human-readable description but never parse it in clients.

---

## 7. Exact matching semantics

The primary reference is the club's public `docs/matching-engine.md`, not incidental implementation details of Go.

### 7.1 Matching algorithm

For accepted incoming **BUY**: repeatedly inspect lowest-price resting ask. If best ask price is greater than incoming limit, stop. Otherwise trade `min(incoming_remaining, resting_remaining)` at the **resting ask's** price. For accepted incoming **SELL**: inspect highest-price resting bid. If best bid price is less than incoming limit, stop. Otherwise execute at **resting bid's** price. Record each distinct incoming/resting pair as one execution, in processing order. Remove fully filled resting orders; preserve priority of partially filled ones. If incoming remainder is positive, append it at the tail of its price level on the own side. Do not rest a fully filled order.

For orders at equal price, FIFO comes from their accepted command sequence, not timestamps, string IDs or thread scheduling. A resting order does not lose time priority after being partially filled.

### 7.2 Example

Start with an empty `AAPL` book:

1. Request `req-001`: BUY 10 @ 10100 cents; accepted as `ord-1`, sequence 1; no fill, 10 bid units rest at 10100.
2. Request `req-002`: SELL 4 @ 10000; accepted as `ord-2`, sequence 2; trades 4 at **10100** cents because `ord-1` is resting. Execution `exe-1`; `ord-1` retains 6 remaining.
3. Request `req-003`: SELL 2 @ 10100; accepted as `ord-3`, sequence 3; trades 2 at **10100** cents. Execution `exe-2`; `ord-1` retains 4.

Final book: 4 AAPL bids at 10100; no asks. Both executions have deterministic IDs and sequences. Replaying these three accepted commands yields byte-for-byte equivalent **canonical output** after excluding explicitly non-deterministic presentation metadata.

### 7.3 Suggested initial structures

Maintain separate `SymbolBook` objects for each symbol and side:

- Bid price levels: `std::map<PriceCents, PriceLevel, std::greater<>>`.
- Ask price levels: `std::map<PriceCents, PriceLevel, std::less<>>`.
- Each `PriceLevel`: FIFO `std::list<RestingOrder>` plus aggregate quantity.
- Active order index: `std::unordered_map<OrderId, OrderLocation>` holding symbol, side, price and a stable list iterator. Remove/update references before erasing list nodes or price levels.
- Global command, execution, and (R3) market-data sequence counters within the authoritative state object.

Insertion into a price level is O(log L) to locate a level and O(1) to append an order. Cancellation lookup is average O(1) followed by O(1) list erase and possible O(log L) level removal. Matching visits executable resting orders and affected price levels; there is no universal O(1) guarantee. `std::list` can be cache-unfriendly; this implementation is a **clear correctness baseline**, not presumed optimal. Profile alternatives after R1.

### 7.4 Ownership/concurrency design

The `Engine` class owns state and is not itself thread-safe. All mutations enter through a single `Sequencer`/`ExchangeStateMachine` owner. R1 runs this sequentially on one thread. R3 allows multiple input threads to enqueue **requests** into a bounded queue, but a single sequencer selects acceptance order and mutates the book. External clients must not assume network arrival order between different connections is globally meaningful; the assigned command sequence defines acceptance.

### 7.5 API separation

Recommended logical interfaces:

```cpp
class MatchingEngine {
 public:
    MatchResult submit(const AcceptedOrder& order);
    CancelResult cancel(const std::string& order_id);
    BookSnapshot snapshot(const std::string& symbol, std::size_t depth) const;
    CanonicalState canonical_state() const;
};

class ExchangeStateMachine {
 public:
    CommandOutcome validate_and_preview(const Command& cmd) const;
    CommandOutcome commit(const Command& cmd); // owning sequencer only
    CanonicalState canonical_state() const;
};
```

Treat these as architectural sketches: exact class signatures may evolve. Ensure there is one authoritative validation/transition rule set and no hidden mutation during validation. R2 persistence integration must preserve fail-stop consistency: a previously durably logged command must eventually be applied (or replayed) before later commands are processed.

---

## 8. Command-line and data contracts

### 8.1 R1 input and output

Use one JSON command per line for `simulate`. Example:

```json
{"type":"new_limit","request_id":"req-001","symbol":"AAPL","side":"BUY","price":10100,"quantity":10}
{"type":"new_limit","request_id":"req-002","symbol":"AAPL","side":"SELL","price":10000,"quantity":4}
{"type":"new_limit","request_id":"req-003","symbol":"AAPL","side":"SELL","price":10100,"quantity":2}
{"type":"snapshot","symbol":"AAPL","depth":10}
```

`simulate` starts with an empty state, accepts the file or standard input, processes one command at a time and emits one structured JSON response per input plus a final state summary (output file or stdout as documented). Snapshot is read-only and does not consume an accepted command sequence.

Example conceptual response for the second new order:

```json
{
  "request_id":"req-002",
  "status":"accepted",
  "command_sequence":2,
  "order_id":"ord-2",
  "remaining":0,
  "executions":[{
    "execution_id":"exe-1",
    "execution_sequence":1,
    "incoming_order_id":"ord-2",
    "resting_order_id":"ord-1",
    "symbol":"AAPL",
    "price":10100,
    "quantity":4
  }]
}
```

All JSON numeric values stay exact within documented integer bounds; fail rather than serialize invalid large values unsafely. JSON is an interface convenience; do not put JSON parsing in the matching hot path.

### 8.2 Canonical state

Define a deterministic canonical serializer for comparisons and replay tests. It contains:

- Last accepted command sequence and last execution sequence.
- Per-symbol bid levels descending and ask levels ascending, with individual resting orders in FIFO order and their remaining quantities.
- Optional order lifecycle summary sorted by acceptance sequence.
- Execution records sorted by execution sequence.
- Request-id -> original accepted command mapping/response needed for idempotency (R2).

Sort unordered containers before serialization; exclude wall-clock times, addresses, allocation layout, thread IDs and unspecified hash iteration order. Compute `state_hash = SHA-256(canonical_bytes)` for validation/recovery demonstrations (SHA-256 is for test identity, not a security guarantee). For R1, you can use deterministic JSON serialization for canonical state; don't accidentally rely on unordered-map iteration order.

---

## 9. R2 persistence: append-only command journal and replay

**Objective:** After restarting a process, reconstruct exactly the committed in-memory state using a durable sequence of commands. Demonstrate explicit acknowledgment and retry semantics, not the misleading slogan "exactly once".

### 9.1 Durability contract

The default R2 `--durability sync` mode must not return an **accepted** acknowledgment until the command's journal record has been fully appended **and successfully synchronized** to the underlying OS/filesystem, as specified by the supported platform. The in-memory effects must be applied before the accepted response is returned. On startup, replay all valid committed journal records in order before accepting new commands.

If a process crashes **after durable append but before response**, the command is committed and must be applied by replay; the client may not know, so a retry using the same `request_id` must return the original committed result rather than performing the command again. This is **idempotent command application**, not exactly-once transport delivery.

If a journal write or sync fails, stop accepting subsequent mutations, report `JOURNAL_IO_ERROR`, and transition to a fail-stop state requiring restart/recovery. Never proceed as though the durable result is known when it is uncertain.

**Filesystem caveat:** `fsync`/`fdatasync` behavior depends on OS, filesystem, drive and hardware. On Linux, syncing a newly created journal file does not automatically guarantee durability of its directory entry; consider directory synchronization at creation/rotation. On macOS, stronger flush semantics may require platform-specific handling. Document supported failure assumptions; do not claim protection from every power-loss scenario.

### 9.2 Journal file layout (v1)

Use a single-writer binary file:

- Fixed header: 4 ASCII magic bytes `EXJ1`, little-endian `uint16 format_version = 1`, little-endian `uint16 flags = 0`.
- Record: little-endian `uint32 payload_length`, little-endian `uint32 payload_crc32c`, then `payload_length` bytes of a versioned, canonical serialized accepted command.
- Maximum accepted payload length 4096 bytes; reject unreasonable lengths before allocation. Write explicit byte-order conversion; do not serialize C++ structs using raw memory layouts.
- Payload must include `command_sequence`, `request_id`, command type, generated order ID if applicable, and full validated command parameters. A `cancel` payload records its target. Assign deterministic execution/event IDs during replay, not from runtime clocks or random sources.
- CRC32C detects accidental record corruption. It is **not cryptographic authentication**.
- Journal commands are accepted mutations, not rejected requests and not read-only snapshots. Include enough information to reconstruct outcomes deterministically.

**Recovery rule:** Detect EOF precisely at the end of a valid record as clean. If the final record has an incomplete header or payload, report an incomplete *tail* and, with explicitly documented recovery policy, truncate to the last complete record (with exclusive ownership of the file). A **checksum mismatch on a full record or corruption in the middle is a hard error**; do not silently truncate arbitrary corruption. Preserve original journal before destructive repair when practical.

**Atomicity rule:** Validate the command and check all state-dependent preconditions *before* attempting the append. Once it is durably logged, application must be deterministic; on unexpected post-log application failure, fail-stop and recover from journal rather than accepting new work. Test and document process-crash boundaries. R1's order-book-only contract makes this tractable because account/risk/settlement transactions are explicitly outside scope.

### 9.3 Replay and snapshots

`enginex replay --journal path --verify` opens journal, validates header/records/CRC/sequence, replays accepted mutations from an empty state, prints the state hash and summary, and returns nonzero on integrity failure.

Replaying a command must recreate:

- Exact `ord-<seq>` assignment and chronological execution IDs.
- Exact matched quantities/prices and resting-order FIFO state.
- Idempotency lookup mapping of request IDs to original accepted commands and outcomes.
- The same canonical final state hash.

**Optional optimization:** A separately versioned checkpoint/snapshot file with journal offset and hash, created atomically. Defer until full replay works. A snapshot must be tied to a known journal watermark; startup must verify consistency and continue replaying subsequent records.

### 9.4 R2 demonstration

1. Start journaled service, submit a known sequence, receive accepted responses.
2. Stop/kill process after selected acknowledgment/crash points.
3. Restart and replay; state and fills match expected canonical hash.
4. Retry a previously committed request ID; the service returns its original result without duplicate execution.
5. Truncate the last record deliberately; report tail recovery and prove no previously acknowledged command was discarded under the stated sync/crash assumptions.
6. Mutate a complete record's byte; detect checksum mismatch and refuse unsafe startup.

---

## 10. R3 networking and market-data distribution

**Scope:** A pedagogical networked event-processing service, not an institutional low-latency production feed.

### 10.1 Transport

Use TCP over localhost by default. **TCP is a byte stream, not a message stream**, so frame all messages:

`[uint32 big-endian payload_length][UTF-8 JSON payload]`

Set a maximum frame size (e.g., 32 KiB), enforce timeouts and limits, handle short reads/writes, and never trust client-declared lengths. Protect the listener from unlimited connections: start with a documented cap such as 64 clients and a bounded input queue.

### 10.2 Wire message types

Required:

- `new_limit`, `cancel`: mutating client command carrying `request_id` and command body.
- `ack`, `error`: request-correlated response containing accepted command sequence or error code.
- `snapshot_request`, `snapshot_response`: depth-limited book snapshot plus an `event_watermark`.
- `subscribe`: begin receiving events after a stable snapshot handoff.
- `book_update`: sequenced change for a symbol, including market-data sequence, command sequence, executed trades and best/depth information.
- `resync_required`: optional explicit overload or gap notice; server may disconnect if it cannot enqueue the notice safely.

Keep schema version in every message: `"protocol_version":1`. Define all JSON fields, valid ranges, and examples in `docs/protocol.md`. Document whether subscribers receive all instruments or named symbol filters.

### 10.3 Event ordering and consistent handoff

Only committed commands generate market-data events. The sequencer assigns a monotonically increasing global `event_sequence`; events are placed in publication queues in that order. Don't let journal I/O, per-client socket writes, or callbacks reorder authoritative state mutations.

A subscriber must never receive a snapshot plus a later feed that silently misses an event. Implement the snapshot/subscribe handoff atomically with respect to the sequencer: register subscription at a captured state/event watermark, enqueue snapshot, then deliver events with sequence > watermark (or use an equivalent protocol with explicit gap detection and replay/resync). If a subscriber's queue overflows, terminate its feed and require a fresh snapshot; do not block the sequencer for the slow subscriber.

**Do not claim lossless replay for network clients** unless you also retain/reconstruct a defined event history covering the requested range. For R3, *snapshot-based resynchronization* is sufficient and clearer.

### 10.4 Concurrency model

- Thread(s) read network input and validate framing/JSON into bounded requests.
- One authoritative sequencer thread validates state-dependent conditions, assigns command sequences, journals (R2), applies state transitions, and emits ordered event envelopes.
- Per-client writer loops drain bounded output queues; readers/writers never directly mutate books.
- Shutdown stops ingress, drains or rejects pending work according to documented policy, completes committed journal synchronization, then closes connections.
- Start with mutex/condition-variable queues. A custom lock-free queue is **only** an optional benchmarked optimization after a correct implementation.

### 10.5 Failure behavior

Test invalid frames, oversized lengths, malformed JSON, unexpected disconnects, slow readers, queue overflow, duplicate requests, reconnect, snapshots under load, and process restart. A closed connection **does not imply an uncommitted command**. Clients must use stable request IDs to retry ambiguous operations.

---

## 11. Test strategy: required test matrix

### 11.1 R1 unit behavior

- Empty book; noncrossing BUY/SELL; exact-price crossing; better-price crossing.
- Single fill; partial incoming; partial resting; full fill; multi-level sweep.
- FIFO across three same-price orders; different-price priority over age.
- Resting execution price (both directions); partially filled resting retains priority.
- Independent symbols; top-N aggregation and sorting.
- Reject missing/invalid symbol, zero/negative/too-large price/quantity and unknown side.
- Overflow-safe aggregation and multiplication; no state changes on reject.
- Cancel first/middle/last order; cancel only order at a price level; cancel unknown, filled or already-cancelled order; ensure no dangling iterator/index.
- Rejected commands do not consume accepted sequence; acceptance order deterministic.

### 11.2 Property/invariant tests

For each generated sequence (seeded and reproducible), after *every command* assert nonnegative remaining, valid FIFO, no crossed book, correct aggregate quantities, index equivalence with active orders, unique IDs and monotonically increasing sequences. Each execution must preserve quantities: `executed = min(current incoming, current resting)` and use the preexisting resting price. Use a deliberately simple independent reference implementation to cross-check small random books.

### 11.3 Differential testing against TradeGrid

Translate the club's public matching fixtures into EngineX commands. Assert equal **matching outcomes and order-book state under matching-only common semantics**. Don't compare end-to-end Go exchange latency with EngineX pure matching latency as though they implement identical work: Go's service includes order validation, state copying, execution metadata and account/risk boundary behavior. Cancellation may not yet exist in the club's Go Sprint 2 implementation and must be verified against EngineX-specific or independent reference cases, not falsely claimed as cross-language parity.

### 11.4 R2 journal/recovery

- Empty journal startup and valid header creation.
- Two sessions: append then restart, compare canonical hash and next sequence IDs.
- Idempotent retry of accepted new order and cancel; reuse request ID with changed body is rejected.
- Crash before durable record; crash after durable record but before reply; crash after reply.
- Truncated payload and truncated header at EOF; safe tail handling.
- Full-record checksum mismatch; bad version/magic; gap or regression in logged sequence.
- Simulated failed `write`, failed `fsync`, disk-full handling and fail-stop state.
- Confirm no state/log changes after a rejected command.
- Restart repeatedly; canonical state and fills remain identical.

### 11.5 R3 network/concurrency

- TCP fragmentation/coalescing, short reads/writes, invalid length and JSON.
- Concurrent submissions: each accepted command gets unique global sequence; single sequencer produces consistent results.
- Subscriber snapshot handoff during live mutation without an unreported sequence gap.
- Slow consumer queue overflow -> explicit disconnect/resync; sequencer remains responsive.
- Reconnect gets fresh snapshot and later events in order.
- Duplicate command on timeout/retry produces one logical mutation.
- Clean shutdown with outstanding clients, queue saturation and journal flush.

### 11.6 Dynamic analysis

ASan/UBSan builds must pass matching and recovery suites. Run ThreadSanitizer on supported CI jobs once threading is added; do not combine incompatible sanitizer modes. Treat observed races and undefined behavior as release blockers. A disabled sanitizer test must be documented, not quietly reported as passing.

---

## 12. Benchmark specification and scientific integrity

### 12.1 Distinct measurements

**Never mix these into a single number:**

1. **Pure matching latency**: in-process call around core processing only; no I/O, JSON or durability.
2. **Durable accepted-command latency**: validation + record write/sync + apply + response creation, excluding network as specified.
3. **Network round-trip latency**: TCP framing/serialization/queueing/journal/matching/response (end-to-end local client).
4. **Recovery time**: time to validate/replay N records and become ready.

Report throughput and p50/p95/p99 for each relevant measurement; do not assert a "low-latency" SLA without defining context. For durable commands, fsync overhead can dominate. Do not hide this by benchmarking the in-memory core and presenting its result as durable end-to-end processing.

### 12.2 Required workloads

- `no-cross`: mostly resting orders, steadily growing book.
- `high-cross`: frequent executions against existing liquidity.
- `same-price-burst`: many orders at identical price, stresses FIFO/cancellation.
- `multi-level-sweep`: large incoming orders cross multiple levels.
- `cancel-heavy`: realistic mix of order insertions and cancellations.
- `mixed`: seeded blend of submit/cancel/partial-fill across 2–N symbols.

Each workload specifies seed, count, price distribution, symbol distribution, cancellation ratio and initial book population. Include at least small smoke runs and larger ≥100k-command runs where practical. Validate workloads actually produce the intended hit rates; don't compare unlike mixes.

### 12.3 Benchmark procedure

- Compile Release with recorded compiler/toolchain version, flags and Git commit SHA.
- Record machine model, CPU architecture, memory, OS/kernel and power mode.
- Warm up before measuring; run multiple trials; provide median/dispersion, not only the best run.
- Generate input outside the timed code section for pure matching measurements.
- Measure clocks consistently (`steady_clock`), note timer overhead and sampling effects; prevent unrealistic per-command print/log overhead.
- Record peak RSS, allocation rates if available, and CPU utilization.
- For p99, collect sufficient independent observations and report exact methodology.
- On Linux optionally use `perf`, flamegraphs, cache-miss counters, thread pinning and controlled CPU governor. These are optional evidence, not compulsory requirements for R1.
- Store raw or summarized data in CSV and an interpretation in `docs/benchmarks.md`. If results regress, report it.
- Compare Go only on equivalent matcher operations and comparable workloads; explain any differences in transaction/persistence scope, compiler/runtime, memory and hardware.

### 12.4 Optimization experiment

After recording baseline, form one hypothesis (e.g., order lookup or book-level allocation is costly); change one design variable; rerun identical seeds; verify tests still pass; report before/after latency and throughput with variance. Revert changes that create undue correctness risk or do not improve intended workloads. No invented percentage improvements.

---

## 13. CI, documentation and quality gates

### 13.1 Required GitHub Actions workflow

- Linux GCC Debug build, unit/invariant/fixture tests.
- Linux Clang Debug + ASan/UBSan tests.
- Linux Release build, CLI smoke test and a short seeded benchmark (no performance threshold that causes noisy CI failures).
- `clang-format` check and compiler warnings (e.g., `-Wall -Wextra -Wpedantic`; errors for owned code).
- R2 recovery suite with fault injection (isolated temp directories).
- R3 socket/concurrency suite; ThreadSanitizer separate supported job.

Do not claim the project has CI until a workflow actually passes on the current commit.

### 13.2 README structure

1. A plain-English two-paragraph explanation of EngineX.
2. Explicit feature status table **Implemented / Planned**, updated per release.
3. Architecture diagram distinguishing existing Go TradeGrid from standalone EngineX.
4. Supported operating systems and prerequisite tools.
5. Exact build/test/demo/benchmark commands verified on a clean clone.
6. A small real CLI example with actual output.
7. Guarantees and deliberate non-goals, including no production usage.
8. Benchmark results and measurement methodology with date/hardware.
9. Testing strategy, CI badge if real, known bugs/limitations.
10. Individual contribution/relationship to the club; link to public TradeGrid spec with attribution.

### 13.3 Required architecture documentation

Explain why a single sequencer owns state, how cancellation indexes avoid dangling iterators, which operations require a sync, how retry IDs prevent duplicate committed commands, and why subscriber queues are bounded. Document the difference between matching-engine p99 and end-to-end durable acknowledgment p99. Include failure-sequence diagrams for `fsync` failure and ambiguous client retry.

---

## 14. Milestone plan and priority

The user is a fast developer with substantial available time, but testing correctness, durability and tail latency takes effort that raw coding speed cannot eliminate. Prioritize **a small polished release over a sprawling unfinished implementation**. Suggested focused sprint:

| Window | Work | Release gate |
| --- | --- | --- |
| Day 0 | Create repository, write skeleton README, pin toolchain/dependencies, set up CMake/CTest/CI | Builds a test executable |
| Days 1–2 | Domain types, validation, deterministic sequencer, price levels, matching, fills | Core behavior tests pass |
| Day 3 | Cancellation index, invariants, edge cases, canonical state | No known correctness failures; sanitizer pass |
| Day 4 | Translate Go matching fixtures, randomized reference tests, CLI demo | Verified matching parity within common scope |
| Days 5–6 | Benchmark harness, profiling baseline, README, architecture and measured results | **R1 shippable and résumé-eligible** |
| Days 7–9 | Journal format, replay, sequence/idempotency, error handling | R2 functional and deterministic |
| Days 10–11 | Fault injection, restart/crash testing, durable sync semantics, R2 documentation | **R2 shippable** |
| Days 12–14+ | TCP protocol, sequenced feed, subscribers, bounded queues, recovery tests | **R3 shippable when validated** |
| After R3 | Optional benchmarks/optimizations, optional club subtree integration | R4 only where justified |

These day counts are **aggressive planning blocks, not promises**. If placement documents are due before R1, submit honest current achievements: existing Go work, C++ coursework, and only actually implemented EngineX work (clearly labeled in-progress if appropriate). Do not delay the résumé indefinitely for optional networking or a custom allocator.

### 14.1 Explicit stop rules

- R1 is not shippable without passing correctness tests and a reproducible demo.
- R2 is not shippable without restart/replay tests and documented sync/ack behavior.
- R3 is not shippable without bounded queues, ordering checks and subscriber resync tests.
- Do not begin custom optimizations before baseline tests and benchmark evidence.
- Don't modify club's Go exchange unless a separate review approves integration.

---

## 15. End-to-end demo script and expected reviewer experience

**R1 reviewer flow:**

1. Clone repo and follow documented build.
2. Run `ctest`, observe passing match, cancellation and invariant tests.
3. Run `enginex simulate --input examples/demo_orders.jsonl --output demo-results.json`.
4. Inspect responses: `req-002` and `req-003` execute at the resting buy price; final AAPL bid has four shares at 10100 cents.
5. Run `enginex benchmark --seed 42 --orders 100000 --scenario mixed --out ...`.
6. Read actual, clearly labeled benchmark methodology and results; no fabricated metrics.
7. Inspect a short design note explaining price-level representation and complexity.

**R2 reviewer flow:**

1. Run a journaled session; submit commands with request IDs.
2. Crash/restart or replay a captured journal; see matching canonical state hash.
3. Retry an accepted command after restart; confirm no duplicate execution.
4. Run corruption test; see explicit refusal rather than a silently wrong book.

**R3 reviewer flow:**

1. Start TCP server in terminal A.
2. Connect a client in terminal B; request snapshot and subscribe.
3. Submit orders from terminal C; terminal B receives ordered updates.
4. Disconnect and reconnect; obtain a new snapshot/watermark.
5. Simulate a slow consumer; see bounded buffering and a clean resync requirement, without stalling the sequencer.

A polished GUI is optional and should not block any of these flows.

---

## 16. Exact acceptance criteria by release

### Release 1 acceptance

- [ ] Clean-clone C++20 build succeeds on documented compiler and Linux CI.
- [ ] At least two symbols and both sides are handled independently.
- [ ] Price-time matching, partial/full fills and resting execution prices are correct.
- [ ] Cancellations update queues, totals and order index without invalid references.
- [ ] Invalid requests are rejected atomically.
- [ ] Canonical state is deterministic across repeated runs.
- [ ] Automated unit, invariant and selected Go common-fixture tests pass.
- [ ] ASan/UBSan job passes.
- [ ] `simulate` CLI works on included demo and produces expected outputs.
- [ ] Benchmark harness outputs real measured results and methodology.
- [ ] README distinguishes completed from planned features.

### Release 2 acceptance

- [ ] All R1 acceptance remains true.
- [ ] Versioned append-only journal with explicit record framing and checksum.
- [ ] Sync-mode acknowledgment is gated on durable append and application.
- [ ] Restart/replay reconstructs canonical state and stable IDs.
- [ ] Same request ID retries do not duplicate accepted effects.
- [ ] Truncated tail, corruption and I/O errors produce documented behavior.
- [ ] Crash/kill and error-injection tests pass; uncertainty behavior is documented.

### Release 3 acceptance

- [ ] All R2 acceptance remains true.
- [ ] TCP framing handles arbitrary read/write segmentation and rejects oversized messages.
- [ ] Concurrent clients cannot mutate authoritative state out of sequence.
- [ ] Snapshot and live feed handoff has correct event watermark/gap policy.
- [ ] Bounded queues and slow-consumer disconnect/recovery are tested.
- [ ] Server cleanup and restart produce correct state and no observed races on supported platforms.
- [ ] End-to-end latency is reported separately from pure matching latency.

---

## 17. Résumé, interview and placement strategy

### 17.1 R1 résumé content — only once verified

**EngineX — C++ Trading Engine and Performance Benchmark Suite**  
*C++20, CMake, Linux, Data Structures, Automated Testing*

- Implemented a deterministic C++ limit-order matching engine supporting price-time priority, partial fills, order cancellations and multi-symbol books.
- Built automated correctness and workload benchmarks measuring order-processing throughput and tail latency; documented data-structure design and reproducibility.

Replace a generic performance bullet with a **real, qualified result** (workload/hardware) if measured. Never cite invented throughput/latency numbers. If written during a club project, describe the personal subsystem accurately without implying the Go platform was rewritten.

### 17.2 R2 résumé extension — only once verified

- Designed an append-only write-ahead journal with validated command replay, deterministic state restoration and idempotent retries; tested restart/crash and corruption scenarios.

### 17.3 R3 résumé extension — only once verified

- Developed a sequenced TCP market-data publisher with bounded subscriber queues, snapshot-based recovery and backpressure handling.

### 17.4 Short recruiter/placement description

> I’m interested in backend and systems engineering, particularly electronic trading, distributed infrastructure, and performance-sensitive services. My experience includes Go backend and trading-platform work, and I independently built a C++ trading-engine lab with reproducible benchmarks and [only the completed reliability/network features]. I’d be interested in Chicago eTrading, Markets technology, or core backend/platform teams where I can contribute to complex production systems.

The finished artifact helps **demonstrate fit**; it does not ensure placement or prove intern openings exist on any particular team.

### 17.5 Be ready to answer

- Why a single-threaded sequencer instead of concurrent writes to the book?
- Why price-level map + FIFO list instead of one sorted vector?
- When do iterator invalidations occur?
- Why execute at resting price?
- What happens if process crashes between journal sync and acknowledgment?
- How is a duplicate retry identified after restart?
- What does `p99` measure in your benchmark and what does it exclude?
- Why not compare full Go transaction latency to isolated C++ matcher latency?
- How is slow TCP subscriber backpressure handled without blocking matching?
- Which engineering assumptions differ from a regulated production exchange?

---

## 18. Instructions for a developer or coding agent starting with zero context

You are implementing **TradeGrid EngineX** from scratch. Treat this specification as the primary product/engineering contract. The project does not exist yet unless the repository already contains files created during this task. Never assume a feature is implemented because the document mentions it.

**Required workflow:**

1. Inspect the repository contents and git status; don't overwrite existing work.
2. Create a concise implementation plan from this specification and identify R1 as the immediate ship target.
3. Scaffold CMake/CTest, pinned dependencies, directory layout, sample fixtures and a compileable test binary.
4. Implement the deterministic matching model and validation independently of the networking/storage layers.
5. Test each behavior and document actual outputs as it is implemented.
6. Build CLI simulation, benchmark harness, shared Go-matcher fixture tests and CI.
7. Mark R1 only when its acceptance criteria pass; create a git tag/release if appropriate.
8. Implement journaling and idempotency **after** R1 tests pass; explicitly design ack and crash semantics before coding.
9. Implement networking **after** R2 replay works; include bounded queues and snapshot-handshake correctness before claiming R3.
10. Keep implementation modular and maintainable; do not produce a fake UI, simulated test output, fabricated metrics or unsupported claims.
11. For each completed stage, report code changed, commands actually run, exact tests/results, known limitations and remaining acceptance criteria.
12. Do not modify TradeGrid's existing Go backend, club docs, database or CI without a separate explicitly approved integration issue.

**Implementation autonomy:** Choose sensible local class/file decomposition, tests and helper utilities consistent with this spec. If a nontrivial architecture change becomes necessary, document the tradeoff and retain the core invariants. If time is constrained, **deliver the highest-quality completed release possible**, not a broken feature-complete skeleton.

**Required handoff on completion:** source code, buildable repository, current-feature status, test and CI evidence, README, benchmark CSV/results with hardware/flags, architecture and journal/protocol docs as applicable, and an honest short résumé-ready summary of what actually shipped.

---

## 19. Reference links

**Club repository and contracts**

- https://github.com/co-rtex/tradegrid
- https://github.com/co-rtex/tradegrid/blob/main/docs/matching-engine.md
- https://github.com/co-rtex/tradegrid/blob/main/docs/api.md
- https://github.com/co-rtex/tradegrid/blob/main/docs/architecture.md
- https://github.com/co-rtex/tradegrid/tree/main/docs/fixtures
- https://github.com/co-rtex/tradegrid/blob/main/backend/internal/matching/engine.go
- https://github.com/co-rtex/tradegrid/blob/main/backend/internal/exchange/service.go
- https://github.com/co-rtex/tradegrid/blob/main/CONTRIBUTING.md

**C++ engineering references**

- https://www.learncpp.com/
- https://cmake.org/cmake/help/latest/guide/tutorial/
- https://google.github.io/googletest/quickstart-cmake.html
- https://clang.llvm.org/docs/AddressSanitizer.html
- https://clang.llvm.org/docs/ThreadSanitizer.html
- https://man7.org/linux/man-pages/man2/fsync.2.html

**Final guiding principle:** The strongest result is a small, correct, fully tested, clearly measured C++ system that can be explained under questioning. Add advanced features only after each prior release is demonstrably complete.
