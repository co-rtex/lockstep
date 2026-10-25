# TradeGrid compatibility and attribution

EngineX is an independent C++ project. It does not modify, replace, link to or run alongside
the club's Go exchange at <https://github.com/co-rtex/tradegrid>. The Go service remains the
club's authoritative implementation. Nobody needs EngineX to build or run TradeGrid.

## What was used, and how

| TradeGrid artifact (commit `bb2e124cf3cd18a3a0ac8087644d81a72fc51d60`) | Use in EngineX |
|---|---|
| `docs/matching-engine.md` (behavior spec, worked examples 1–7, invariants) | Behavioral reference. Examples 1–7 are re-expressed as EngineX fixtures in `tests/testdata/tradegrid/example-*.json`, with dollar prices converted to integer cents. |
| `docs/fixtures/01-no-fill.json`, `02-full-fill.json`, `03-partial-fill.json` | Matching-relevant expectations (order/execution ids and sequences, prices, quantities, remaining, aggregated book) re-expressed in `tests/testdata/tradegrid/fixture-*.json`. |
| `docs/fixtures/00-seed.json`, `04-risk-rejection.json` | Not translated: they test account state and an `INSUFFICIENT_CASH` risk rejection, which are outside EngineX's matching-only scope. `tools/compare_go_fixtures.py` reports them as SKIPPED. |

No TradeGrid source code was copied. The TradeGrid repository had no license file when it
was examined. EngineX therefore stores only its own re-expression of the published expected
outcomes (facts such as "BUY 10 @ 19100 against SELL 10 @ 19000 executes 10 @ 19000"), with
source attribution in every fixture file, and does not vendor TradeGrid files.

## Differential check

`tools/compare_go_fixtures.py --fixtures <tradegrid>/docs/fixtures` runs the order-placement
fixtures through `enginex simulate` and compares the matching-only subset. Result at the
commit above:

```text
SKIPPED  00-seed.json            no request (seed/initial-state fixture)
PASS     01-no-fill.json
PASS     02-full-fill.json
PASS     03-partial-fill.json
SKIPPED  04-risk-rejection.json  expected HTTP 422 (INSUFFICIENT_CASH): outside matching scope
3 passed, 0 failed, 2 skipped
```

## Common semantics (asserted equal)

* Price-time priority: best price first, then FIFO by acceptance order. Partial fills keep
  priority.
* Execution at the resting order's limit price, quantity = min of the two remaining
  quantities, one execution per incoming/resting pair.
* Remainders rest at their own limit behind earlier orders at that price.
* Ids: `ord-1`, `ord-2`, … in acceptance order and `exe-1`, … in execution order. These match
  TradeGrid's fixtures as long as no cancels occur (see below).

## Known differences (not parity claims)

| Topic | TradeGrid (Sprint 2 Go) | EngineX |
|---|---|---|
| Scope | Full exchange service: orders, risk, cash/position reservations, accounts, HTTP API | Matching core + its own CLI/journal/TCP layers; **no accounts, risk, users or settlement** |
| Cancellation | Out of scope for the Sprint 2 matching spec | Implemented; verified only against EngineX's own tests and reference model, **not** cross-language parity |
| Sequence numbers | Order `sequence` counts orders | `command_sequence` counts every accepted mutating command, cancels included; `ord-<n>` therefore skips numbers used by cancels |
| Order status names | `NEW`, `PARTIALLY_FILLED`, `FILLED` | `OPEN`, `PARTIALLY_FILLED`, `FILLED`, `CANCELLED` |
| Execution fields | `buyOrderId`, `sellOrderId`, user ids, `executedAt` timestamp | `incoming_order_id`, `resting_order_id`, `incoming_side`; no wall-clock timestamps in replay-relevant state |
| Errors | HTTP status + codes such as `INSUFFICIENT_CASH` | Stable codes from spec §6.2 (`INVALID_PRICE`, `UNKNOWN_ORDER`, …) |
| Validation limits | Defined by the Go API contract | Price 1…100 000 000 cents, quantity 1…1 000 000, symbols `AAPL`/`MSFT` by default |

## Performance comparisons

EngineX's benchmark numbers time an in-process C++ matching core only. They must not be
compared with TradeGrid's end-to-end HTTP latency, because the Go service also performs
request handling, risk checks, reservations, state cloning and publication. No Go-vs-C++
performance claim is made anywhere in this repository.

## Possible future interaction (not implemented)

Any integration would be a separate, club-reviewed proposal. At most it would be a read-only
adapter that observes committed events from the Go service, and only if a stable
committed-event feed exists. EngineX never consumes or modifies Go's live order-processing
state.
