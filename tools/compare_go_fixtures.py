#!/usr/bin/env python3
"""Differential check of EngineX against TradeGrid's published exchange fixtures.

Reads fixture files from a local checkout of https://github.com/co-rtex/tradegrid
(`docs/fixtures/*.json`), translates every order placement into an EngineX `new_limit`
command, runs `enginex simulate`, and compares only the *matching-engine* subset of the
expected results:

  * order id, order sequence, remaining quantity of the response order
  * execution id, execution sequence, price, quantity, buy/sell order ids
  * aggregated order book after the request (price, quantity, orderCount per level)

Account cash/position reservations, user ids, statuses and timestamps belong to the Go
exchange's risk/account layer, which EngineX intentionally does not implement; fixtures whose
expected response is not a successful order placement (e.g. a risk rejection) are reported as
SKIPPED, never as passing.

Usage:
  tools/compare_go_fixtures.py --fixtures ~/src/tradegrid/docs/fixtures \
      [--enginex build/release/enginex]

This script only reads the TradeGrid files; it never modifies that repository.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ORDER_PATH = "/api/v1/orders"


def to_command(body: dict, request_id: str) -> dict:
    if body.get("type", "LIMIT") != "LIMIT":
        raise ValueError(f"unsupported order type {body.get('type')}")
    return {
        "type": "new_limit",
        "request_id": request_id,
        "symbol": body["symbol"],
        "side": body["side"],
        "price": body["price"],
        "quantity": body["quantity"],
    }


def run_simulate(enginex: str, commands: list[dict]) -> dict:
    with tempfile.TemporaryDirectory() as tmp:
        inp = Path(tmp) / "in.jsonl"
        inp.write_text("".join(json.dumps(c) + "\n" for c in commands))
        out = subprocess.run(
            [enginex, "simulate", "--input", str(inp), "--output", "-", "--compact"],
            check=True,
            capture_output=True,
            text=True,
        )
        return json.loads(out.stdout)


def compare_fixture(path: Path, enginex: str) -> tuple[str, list[str]]:
    doc = json.loads(path.read_text())
    request = doc.get("request")
    if not request:
        return "SKIPPED", ["no request (seed/initial-state fixture)"]
    if request.get("path") != ORDER_PATH or request.get("method") != "POST":
        return "SKIPPED", [f"request {request.get('method')} {request.get('path')} is not an order"]
    if doc["response"]["status"] != 201:
        code = doc["response"]["body"].get("error", {}).get("code")
        return "SKIPPED", [f"expected HTTP {doc['response']['status']} ({code}): account/risk "
                           "behavior outside EngineX's matching-only scope"]

    steps = [s for s in doc.get("setup", []) if s.get("path") == ORDER_PATH]
    commands = [to_command(s["body"], f"setup-{i}") for i, s in enumerate(steps, 1)]
    commands.append(to_command(request["body"], "request"))
    sim = run_simulate(enginex, commands)
    responses = sim["responses"]
    problems: list[str] = []
    if any(r["status"] != "accepted" for r in responses):
        problems.append("EngineX rejected a command the fixture expects to succeed")
        return "FAIL", problems
    got = responses[-1]
    want_order = doc["response"]["body"]["order"]

    def check(label, expected, actual):
        if expected != actual:
            problems.append(f"{label}: expected {expected!r}, EngineX {actual!r}")

    check("order id", want_order["id"], got["order_id"])
    check("order sequence", want_order["sequence"], got["command_sequence"])
    check("remaining", want_order["remaining"], got["remaining"])

    want_execs = doc["response"]["body"]["executions"]
    check("execution count", len(want_execs), len(got["executions"]))
    for i, (we, ge) in enumerate(zip(want_execs, got["executions"]), 1):
        buy, sell = ((ge["incoming_order_id"], ge["resting_order_id"]) if ge["incoming_side"] == "BUY"
                     else (ge["resting_order_id"], ge["incoming_order_id"]))
        check(f"exec {i} id", we["id"], ge["execution_id"])
        check(f"exec {i} sequence", we["sequence"], ge["execution_sequence"])
        check(f"exec {i} price", we["price"], ge["price"])
        check(f"exec {i} quantity", we["quantity"], ge["quantity"])
        check(f"exec {i} buy order", we["buyOrderId"], buy)
        check(f"exec {i} sell order", we["sellOrderId"], sell)

    books = {b["symbol"]: b for b in sim["final_state"]["books"]}
    for key, want in doc.get("after", {}).items():
        if not key.startswith("GET /api/v1/orderbook"):
            continue
        book = books.get(want["symbol"])
        if book is None:
            problems.append(f"symbol {want['symbol']} missing from EngineX state")
            continue
        for side in ("bids", "asks"):
            expected = [(l["price"], l["quantity"], l["orderCount"]) for l in want[side]]
            actual = [(l["price"], l["quantity"], l["order_count"]) for l in book[side]]
            check(f"{want['symbol']} {side}", expected, actual)
        check(f"{want['symbol']} asOfSequence", want["asOfSequence"],
              sim["final_state"]["last_command_sequence"])
    return ("PASS" if not problems else "FAIL"), problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--fixtures", required=True, type=Path,
                        help="path to tradegrid/docs/fixtures")
    parser.add_argument("--enginex", default="build/release/enginex")
    args = parser.parse_args()

    files = sorted(args.fixtures.glob("*.json"))
    if not files:
        print(f"no fixtures found in {args.fixtures}", file=sys.stderr)
        return 2
    counts = {"PASS": 0, "FAIL": 0, "SKIPPED": 0}
    for f in files:
        status, notes = compare_fixture(f, args.enginex)
        counts[status] += 1
        print(f"{status:8} {f.name}")
        for n in notes:
            print(f"         - {n}")
    print(f"\n{counts['PASS']} passed, {counts['FAIL']} failed, {counts['SKIPPED']} skipped")
    return 1 if counts["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main())
