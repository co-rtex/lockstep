# EngineX wire protocol v1 (R3)

A pedagogical order-entry and market-data protocol over TCP. It is not an institutional
feed and makes no low-latency claims. Implementation: `include/enginex/protocol.hpp`,
`src/server.cpp`, `src/client.cpp`. Design rationale: [DECISIONS.md](DECISIONS.md) D13–D17.

## 1. Transport and framing

* TCP, IPv4. The default bind address is `127.0.0.1` (`--host` changes it). There is no
  authentication or encryption, so do not expose the server to untrusted networks.
* TCP is a byte stream, not a message stream. Every message is one frame:

  ```text
  [u32 big-endian payload_length][payload_length bytes of UTF-8 JSON]
  ```

* `payload_length` must be in `1..32768` for client→server frames and `1..16777216` for
  server→client frames. The server→client limit is larger because a full-depth subscribe
  snapshot can exceed 32 KiB. Each receiver checks the declared length from the 4-byte header
  alone, before buffering any payload.
* Both sides handle arbitrary segmentation: frames may arrive one byte at a time or many per
  read. Tests cover byte-by-byte delivery, several frames coalesced into one write, and splits
  inside a header.
* The listening socket is non-blocking. If accepting fails because descriptors are
  exhausted, the acceptor backs off for one tick instead of spinning.
* An invalid length (0 or over the limit) is a fatal stream error. The server replies with an
  `error` (`INVALID_REQUEST`, "protocol error: invalid frame length …") and closes the
  connection, because the byte stream cannot be resynchronized.
* A client that sends part of a frame and stalls longer than 10 s is disconnected the same
  way (`partial_frame_timeout`).

## 2. Common fields

Every JSON message has `"protocol_version": 1` and a `"type"`. A missing or different
`protocol_version`, an unknown `type`, an unknown field, or malformed JSON produces an
`error` with `INVALID_REQUEST`; the connection stays open. Field validation follows the
same rules and error codes as the CLI ([architecture.md](architecture.md)). Prices are
integer cents (1..100 000 000) and quantities are integers (1..1 000 000); numbers must be
exact JSON integers.

`request_id` (≤ 64 bytes UTF-8) correlates a reply with its request. For mutating commands it
is also the **idempotency key**; see §6.

## 3. Client → server

| type | fields | reply |
|---|---|---|
| `new_limit` | `request_id`, `symbol`, `side` (`BUY`/`SELL`), `price`, `quantity` | `ack` or `error` |
| `cancel` | `request_id`, `order_id` (e.g. `"ord-12"`) | `ack` or `error` |
| `snapshot_request` | optional `request_id`, optional `symbol` (default: all symbols), optional `depth` (0..1000, default 10, 0 = full) | `snapshot_response` or `error` |
| `subscribe` | optional `request_id` | `snapshot_response` (`subscribed: true`, full depth), then `book_update` events |

```json
{"protocol_version":1,"type":"new_limit","request_id":"c1-17","symbol":"AAPL","side":"BUY","price":10100,"quantity":10}
{"protocol_version":1,"type":"cancel","request_id":"c1-18","order_id":"ord-1"}
{"protocol_version":1,"type":"snapshot_request","request_id":"s1","symbol":"AAPL","depth":5}
{"protocol_version":1,"type":"subscribe","request_id":"feed"}
```

## 4. Server → client

### `ack`
Accepted command (including a duplicate retry, `duplicate: true`). Contains
`command_type`, `command_sequence`, `order_id`, `symbol`, `side`, `price`, `quantity`,
`remaining` (new orders) or `cancelled_quantity` (cancels), `order_status`, the
`executions` produced, and `state_watermark`.

```json
{"protocol_version":1,"type":"ack","request_id":"req-002","command_type":"new_limit","duplicate":false,
 "command_sequence":2,"order_id":"ord-2","symbol":"AAPL","side":"SELL","price":10000,"quantity":4,
 "remaining":0,"order_status":"FILLED","executions":[{"execution_id":"exe-1","execution_sequence":1,
 "command_sequence":2,"incoming_order_id":"ord-2","resting_order_id":"ord-1","symbol":"AAPL",
 "incoming_side":"SELL","price":10100,"quantity":4}],"state_watermark":2}
```

With a journal in `sync` mode, an `ack` is sent only after the command's journal record (and
everything in its group-commit batch) has been written and synchronized.

### `error`
`request_id`, optional `command_type`, a stable `code` and a human-readable `message`. Codes:
`INVALID_REQUEST`, `INVALID_SYMBOL`, `INVALID_SIDE`, `INVALID_PRICE`, `INVALID_QUANTITY`,
`DUPLICATE_REQUEST_ID_CONFLICT`, `UNKNOWN_ORDER`, `ORDER_NOT_OPEN`, `LIMIT_EXCEEDED`,
`SERVER_OVERLOADED` (not processed; safe to retry), `JOURNAL_IO_ERROR` (**outcome unknown**;
retry with the same `request_id` after the server restarts), `INTERNAL_ERROR`.

### `snapshot_response`
`request_id`, `event_watermark`, `command_sequence`, `execution_sequence`, `depth`,
`subscribed`, and `books`: `[{symbol, bids:[{price,quantity,order_count}], asks:[…]}]`,
with bids descending and asks ascending. The state shown is exactly the state after event
`event_watermark`.

### `book_update`
One per accepted, non-duplicate command, published only after that command is durable:

```json
{"protocol_version":1,"type":"book_update","event_sequence":2,"command_sequence":2,
 "command_type":"new_limit","symbol":"AAPL","order_id":"ord-2","side":"SELL","price":10000,
 "executions":[{"execution_id":"exe-1","execution_sequence":1,"incoming_order_id":"ord-2",
 "resting_order_id":"ord-1","incoming_side":"SELL","price":10100,"quantity":4}],
 "levels":[{"side":"BUY","price":10100,"quantity":6,"order_count":1}],
 "best_bid":{"price":10100,"quantity":6},"best_ask":null}
```

`levels` lists every price level the command touched, with its **new** aggregate quantity and
order count; `quantity: 0` means the level is gone. A subscriber that applies `levels` to the
subscribe snapshot maintains an exact aggregated book. `SubscribeHandoffUnderLoadHasNoGaps`
checks this against a fresh full snapshot.

### `resync_required`
`reason` and `last_event_sequence` (the last event queued to this subscriber). The feed
ends after it and the server closes the connection. Reasons:

* `slow_consumer`: the subscriber's outbound queue overflowed.
* `event_too_large`: an event could not be framed even in truncated form (see below), so
  every subscriber's feed ends rather than skipping it silently.

### Oversized acks and events
A single aggressive order can sweep tens of thousands of resting orders, and its execution
list would exceed the server's frame limit (16 MiB by default, configurable down to 1 KiB
for tests). In that case the `ack` and/or `book_update` is sent **without** its executions:
`"executions": []`, `"executions_truncated": true` and `"execution_count": N`. Every other
field (sequences, remaining quantity, changed `levels`, best bid/ask) is still present, so
book mirroring stays exact. If even the truncated event is too large (a sweep that touches an
enormous number of price levels), subscribers get `resync_required` with reason
`event_too_large`.

## 5. Ordering, watermarks and the snapshot handoff

* **Acceptance order** is the single sequencer's `command_sequence`, the same total order the
  journal records. Arrival order across different connections means nothing. Replies to one
  connection arrive in that connection's request order. The exception is `SERVER_OVERLOADED`
  rejections, which never entered the sequencer and may overtake earlier replies.
* **Event sequence.** Every accepted, non-duplicate command produces exactly one
  `book_update`, so `event_sequence == command_sequence`. That makes the numbering stable
  across restarts, because replay re-derives it.
* **Handoff.** A `subscribe` passes through the sequencer like a command. The sequencer first
  flushes and publishes everything staged before it, serializes the snapshot at
  `event_watermark = W`, queues it, and registers the subscriber, all in one step on the
  sequencer thread. The first `book_update` the subscriber receives is therefore exactly
  `W + 1`, and every later one is contiguous. A client that ever sees a gap has found a bug,
  and should resubscribe.
* Subscribers receive **all symbols**; there are no symbol filters in v1.
* No historical replay is offered. A subscriber that disconnects, or is cut off, recovers by
  reconnecting and subscribing again. It gets a new snapshot at a new watermark
  (*snapshot-based resynchronization*). EngineX does not claim lossless replay for network
  clients.

## 6. Retries and ambiguous outcomes

A closed connection does **not** mean a command was not committed: the server may have
committed it and failed to deliver the `ack`. Clients must keep the `request_id` and retry
with the same body. A command that was committed returns its original result with
`duplicate: true`; one that was not is executed now. A different body under the same id gets
`DUPLICATE_REQUEST_ID_CONFLICT`. This holds across server restarts when a journal is used,
because the request map is rebuilt from the journal (`RetryAfterDisconnectProducesOneMutation`,
`tools/fault_injection.py`).

## 7. Limits, backpressure and slow consumers

| Limit | Default | Behavior when exceeded |
|---|---|---|
| connections | 64 (`--max-clients`) | extra connection gets `error SERVER_OVERLOADED` and is closed |
| ingress queue (all connections) | 4096 requests (`--ingress-capacity`) | request answered immediately with `SERVER_OVERLOADED`, not processed |
| outbound queue per connection | 1024 frames (`--outbound-capacity`) | connection marked overflowed: a final `resync_required` (subscriber) or `SERVER_OVERLOADED` error (non-subscriber) goes into a reserved slot, the subscription is dropped, and the socket closes after the queue drains |
| socket send stall | 2 s (`--send-timeout-ms`) per `send()`, plus an overall per-frame budget of the timeout + 1 s per MiB | writer gives up and closes the connection |
| frame size (server→client) | 16 MiB | acks/events sent with `executions_truncated`; oversized snapshots answered with `INTERNAL_ERROR` ("request less depth") |
| group commit batch | 512 commands (`--max-batch`) | larger bursts span several batches |

The sequencer never blocks on a client. It only `try_push`es into outbound queues.
Readers never block on the sequencer either: they `try_push` into the ingress queue and
answer `SERVER_OVERLOADED` when it is full. One slow or stalled client can therefore not
delay order processing for anyone else. `SlowConsumerIsDisconnectedWithoutStallingTheSequencer`
checks this: a healthy subscriber and a trader keep running at full speed while a
non-reading subscriber is cut off.

## 8. Shutdown

On SIGINT/SIGTERM (`enginex serve`) or `Server::stop()`:

1. Stop accepting connections.
2. Stop reading (`shutdown(SHUT_RD)` on every connection). Requests still unread in kernel
   buffers are not processed. Their clients get no reply and must retry, like any ambiguous
   outcome.
3. The sequencer processes every request already in the ingress queue and makes the final
   group-commit flush and sync.
4. Writers deliver every queued reply and event (bounded by the send timeout), then close.
5. Every thread is joined. `serve` prints a `shutdown` line with statistics and the final
   state hash.

Restarting on the same journal restores the identical state, and `event_sequence` numbering
continues (`ShutdownDrainsQueuedWorkAndRestartRecovers`).

## 9. Command-line client

```bash
./build/release/enginex serve --port 9000 --journal data/demo.exj --durability sync     # terminal A
./build/release/enginex client subscribe --port 9000                                     # terminal B
./build/release/enginex client submit --port 9000 --input examples/demo_orders.jsonl     # terminal C
./build/release/enginex client snapshot --port 9000 --symbol AAPL --depth 5
./build/release/enginex client subscribe --port 9000 --slow-ms 50 --quiet               # slow consumer
```

`client submit` sends one request at a time and prints each reply. It accepts `simulate`-style
`snapshot` lines. `client subscribe` prints the snapshot and events, checks that sequences are
contiguous, and ends with a `subscriber_summary` line (`gaps` must be 0).
