# Journaling, recovery and idempotency (R2)

EngineX R2 makes the state machine durable with an append-only **command journal**. Only
accepted mutating commands are recorded. Executions, order ids and events are never stored:
replaying the commands in order re-derives them exactly, because matching is deterministic.

This document is the durability contract: what an acknowledgment means, what survives which
failure, how recovery classifies damage, and what is deliberately *not* guaranteed.
Design rationale: [DECISIONS.md](DECISIONS.md) D8–D12.

## 1. File format (v1)

```text
file    := header record*
header  := "EXJ1"  u16le format_version = 1  u16le flags = 0              (8 bytes)
record  := u32le payload_length  u32le crc32c(payload)  payload           (8 + n bytes)
payload := u8 payload_version = 1
           u8 command_type            1 = new_limit, 2 = cancel
           u64le command_sequence
           u8 len, request_id[len]    1..64 bytes
           new_limit: u8 len, symbol[len] (1..32) | u8 side (0 BUY, 1 SELL) | i64le price |
                      i64le quantity | u64le order_id (== command_sequence)
           cancel:    u64le target_order_id
```

* Every integer is encoded byte by byte in little-endian order; C++ structs are never written
  as raw memory. The encoding is pinned by a golden-bytes test
  (`JournalFormat.PayloadBytesArePinnedLittleEndian`).
* Payloads are at most 4096 bytes. Larger declared lengths are rejected *before* any
  allocation. (A valid v1 payload is at most about 130 bytes.)
* CRC-32C (Castagnoli) detects accidental corruption. **It is not authentication.** Anyone
  who can write the file can forge records.

## 2. Write path and acknowledgment semantics

```text
client ── request ──▶ prepare()  pure validation; rejected → reply now, nothing written
                      commit()   apply in memory (sequence assigned, fills computed)
                      encode     append framed record to the batch buffer
                      flush():   write(batch) → sync → release replies
client ◀── accepted ── (only after sync returned success)
```

| `--durability` | Synchronization before an accepted reply | Survives |
|---|---|---|
| `sync` (default) | Linux `fdatasync`; macOS `fcntl(F_FULLFSYNC)` (falls back to `fsync` if the filesystem rejects it) | process crash, OS crash, power loss *if the device honors flush/FUA* |
| `fsync` | POSIX `fsync` only. On macOS this does **not** flush the drive's write cache | process crash, OS crash; power loss not guaranteed on macOS |
| `none` | none; reply once `write()` returned | process crash only (the data is in the kernel page cache) |

In `sync` mode, invariant 12 holds by construction: a reply is never released before its
record has been fully written *and* synchronized. Applying the command in memory before the
write is safe because nothing is observable before the reply. Doing it first also means an
unexpected internal failure while applying happens *before* the record is written, so the
journal never contains a command that replay could not apply.

New journal files are created with `O_EXCL`; the header is synchronized and the parent
directory is `fsync`ed, so the file's directory entry is durable too. The file is held under
`flock(LOCK_EX | LOCK_NB)` for the life of the process, which makes a second writer fail
immediately.

Group commit: the same pipeline accepts several staged commands and writes and syncs them as
one batch (`DurableExchange::stage` + `flush`). Replies to the whole batch are released after
its single sync. The R3 server uses this. `enginex run` flushes after every command.

## 3. Failure behavior

### 3.1 Write or sync failure → fail-stop

```text
 client            DurableExchange                         journal file / OS
   │ new_limit r9 ──▶ prepare ✓ commit (in memory)
   │                 write(record r9) ───────────────────▶  ok (or EIO / ENOSPC / short write)
   │                 fdatasync() ────────────────────────▶  EIO
   │                 enter FAIL-STOP (memory may hold
   │                 a command of unknown durability)
   │ ◀── JOURNAL_IO_ERROR "outcome unknown; retry with the same request_id after restart"
   │ new_limit r10 ─▶ JOURNAL_IO_ERROR (every request refused, including retries and reads)
   ╳ operator restarts the process
                     open(): scan + replay the journal → state = exactly what is on disk
```

* A failed `fsync` is **never retried and then treated as success**. After a failed flush
  the kernel may have dropped the dirty pages, so a later "successful" fsync proves nothing.
  The process stops accepting work instead.
* `JOURNAL_IO_ERROR` means *the outcome is unknown*, not *rejected*. The record may be
  complete on disk, partially on disk, or absent.

### 3.2 Ambiguous outcome → idempotent retry

```text
 client                         server (crashes after sync, before reply)
   │ cancel c7 (ord-12) ───────▶ record synced ✓ ... process killed ╳
   │ (timeout / connection lost: did it happen?)
   │                              restart: replay rebuilds request map {c7 → seq 31, ...}
   │ cancel c7 (ord-12) ───────▶ prepare: request id c7 known, same body → Duplicate
   │ ◀── accepted, duplicate:true, command_sequence 31, cancelled_quantity … (original result)
```

If the record did *not* survive, the restarted server has no `c7` entry and executes the
cancel for the first time. In both cases the request id produces **exactly one logical
mutation**. This is idempotent command application. It is not exactly-once network delivery,
and the client must reuse the same `request_id` with the same body. Reusing an id with a
different body returns `DUPLICATE_REQUEST_ID_CONFLICT`.

Only *accepted* requests are remembered. A rejected request id can be retried and may succeed
later if state changed (DECISIONS.md D4).

### 3.3 Crash matrix (process crash at each boundary)

All rows are automated in `recovery_test.cpp` (`CrashBoundary.*`, using `fork` and `_exit` at
an injected crash point) and end-to-end in `tools/fault_injection.py` (real `SIGKILL` of
`enginex run`).

| Crash point | Record on disk after restart? | Client saw reply? | Retry with same request id |
|---|---|---|---|
| before the record is written (`after_apply`) | no | no | executes now (first time) |
| after `write()`, before sync (`after_write`) | yes for a process crash (page cache); **not guaranteed** for OS crash/power loss | no | returns original (`duplicate`) if the record survived, otherwise executes |
| after sync, before reply (`after_sync`) | yes | no | returns original (`duplicate`) |
| after reply | yes | yes | returns original (`duplicate`) |

## 4. Recovery

`DurableExchange::open` and `enginex replay` run the same validation:

1. Read the whole file. Validate the header: magic `EXJ1`, version 1, flags 0.
2. For each record: the declared length must be in 1..4096, the CRC32C must match, the
   payload must decode exactly (no trailing bytes, side 0/1, order id == sequence), and the
   sequence must be exactly `previous + 1`.
3. Replay each record through `ExchangeStateMachine::prepare`/`commit`. Every record must be
   **accepted at exactly its logged sequence**; anything else is a *replay divergence*.
   Examples are a duplicated request id, a cancel of an order that was never open, or a
   symbol missing from the configured catalog.
4. Only then accept new commands. Sequences, order ids and execution ids continue from the
   replayed state.

### Classification

| Condition | Class | `run` / server start | `replay --verify` exit |
|---|---|---|---|
| ends exactly at a record boundary | clean | starts | 0 |
| incomplete file header (a prefix of `EXJ1…`, including an empty file) | incomplete tail | header rewritten | 3 |
| partial 8-byte frame header after the last record | incomplete tail | preserve + truncate | 3 |
| payload shorter than its declared length, **and** the visible bytes are a valid prefix of a record with exactly that length (or are all zero) | incomplete tail | preserve + truncate | 3 |
| payload shorter than its declared length, but the visible bytes contradict it (bad version/type, wrong sequence, or a declared length that differs from what the visible request-id/symbol lengths imply) | **corrupt** | refuses | 1 |
| all-zero bytes after the last record (power-loss artifact) | incomplete tail | preserve + truncate | 3 |
| CRC mismatch on a complete record (any position, including the last) | **corrupt** | refuses | 1 |
| declared length 0 or > 4096 with non-zero bytes | **corrupt** | refuses | 1 |
| bad magic / version / flags; non-prefix short header | **corrupt** | refuses | 1 |
| undecodable payload; sequence gap or regression | **corrupt** | refuses | 1 |
| replay divergence | **corrupt** | refuses | 1 |

**Tail policy.** By default an incomplete tail is repaired. The removed bytes are first
written to `<journal>.tail-<offset>.bin` (fsynced), then the journal is truncated to the last
complete record and synced. `--refuse-tail` (`run`) or `TailPolicy::Refuse` refuses instead
and leaves the file untouched. `replay --verify` never modifies anything; `replay
--repair-tail` performs the repair under the exclusive lock.

**Why repairing the tail cannot drop an acknowledged command (in `sync` mode):** a reply is
released only after the *entire* record has been written and synchronized. A record that is
incomplete on disk therefore never got an accepted reply. The test
`TailRepair.TruncatedLastRecordIsPreservedAndTruncated` checks this. It cuts the final record
at several byte offsets, recovers, and shows that the retry executes the command exactly
once.

**Why the prefix check matters.** The CRC covers the payload but not the length field. Without
the check, a bit flip that raised the length of one of the last records (anything within
4 KiB of EOF) would make complete, acknowledged records look like a torn tail, and repair
would truncate them. A genuine torn write always leaves a prefix of the correct bytes, so
EngineX checks every visible byte for consistency before treating the tail as incomplete
(`JournalScan.CorruptedLengthNearEofIsCorruptNotATornTail`, mutation-checked). The finding
came from an independent code review. A future format version could put the CRC over the
length as well.

**Corruption is never "fixed" automatically.** A damaged complete record in the middle could
hide any number of later acknowledged commands, so EngineX refuses to start. The operator must
then decide, for example by restoring a backup. Tests confirm the refused file is left
byte-for-byte unchanged.

**No silent divergence, tested as a property.**
`Recovery.RandomDamageNeverYieldsAStateOutsideThePrefixes` runs 400 trials of random
truncation, random bit flips anywhere in the file, or both, on a random 80-command
workload. Every recovery must reproduce *exactly* the state after some journaled prefix
(hashes from an independent in-memory run). The only other allowed outcome is refusal with
`JOURNAL_CORRUPT`, and a pure truncation must always recover. Result: 138 recoveries, all to
a prefix, and 262 refusals. With the record CRC check disabled (mutation), the test finds a
divergent state.

## 5. Assumptions and limits (read before trusting it)

* **Storage honesty.** `sync` mode assumes that when `fdatasync`/`F_FULLFSYNC` returns
  success, the device has persisted the data. Drives or virtual disks that acknowledge
  flushes without persisting (some consumer SSDs, some VM and container storage
  configurations) break this, and no software above them can detect it.
* **Not tested: real power loss.** The tests cover process crashes (SIGKILL, `_exit` at
  injected points), injected `write`/`fsync` failures, `ENOSPC`, short writes, truncation and
  bit flips. They do not cut power or crash the kernel. Behavior under those events rests on
  the documented OS guarantees above.
* **macOS:** plain `fsync` does not flush the drive cache. That is why the default `sync` mode
  uses `F_FULLFSYNC` (about 3 ms per acknowledgment on the test machine; see
  [benchmarks.md](benchmarks.md)).
* **Linux:** `fdatasync` covers file data and the size change. The directory entry is made
  durable once, at creation. There is no rotation, so no other renames happen.
* **No checkpoints.** Recovery replays the whole journal: about 4 million records/s from a
  warm cache on the test machine. Snapshot/checkpoint files (spec §9.3, optional) are not
  implemented.
* **No compaction or rotation.** The journal and the in-memory idempotency map grow without
  bound.
* **Single host, single writer.** There is no replication. A disk failure loses everything
  since the last backup.
* **Symbol catalog.** It is not stored in the journal. Replaying with a different `--symbols`
  list is detected as replay divergence and refused.
* **CRC32C is not authentication**, and the journal is not encrypted.

## 6. Commands

```bash
# Journaled session: one JSON response line per command, each written only after its sync.
./build/release/enginex run --journal data/demo.exj --durability sync --input examples/demo_orders.jsonl

# Restart: the recovery line reports records replayed, tail state and the state hash.
./build/release/enginex run --journal data/demo.exj --input /dev/null

# Offline verification (read-only); exit 0 clean, 1 corrupt, 3 incomplete tail.
./build/release/enginex replay --journal data/demo.exj --verify
./build/release/enginex replay --journal data/demo.exj --dump          # records as JSON lines
./build/release/enginex replay --journal data/demo.exj --repair-tail   # preserve + truncate tail

# Crash and corruption demonstration (SIGKILL rounds, flipped byte, truncated tail).
python3 tools/fault_injection.py --enginex build/release/enginex --rounds 8

# Deterministic fault injection (tests and demos only): crash points, failed writes/syncs.
ENGINEX_FAULT=crash=after_sync@5 ./build/release/enginex run --journal /tmp/x.exj --input cmds.jsonl
ENGINEX_FAULT=fail_sync=3 ./build/release/enginex run --journal /tmp/y.exj --input cmds.jsonl
```
