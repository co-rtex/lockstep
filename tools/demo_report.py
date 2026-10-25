#!/usr/bin/env python3
"""Pretty-printers used by scripts/demo.sh (kept separate so the shell script stays readable)."""

import json
import sys


def load_lines(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def simulate(path):
    d = json.load(open(path))
    for r in d["responses"][:3]:
        ex = ", ".join("%s %d@%d vs %s" % (e["execution_id"], e["quantity"], e["price"],
                                             e["resting_order_id"]) for e in r["executions"])
        print("  %s: %s %d@%d -> seq %d %s %s [%s]" % (r["request_id"], r["side"], r["quantity"],
                                                       r["price"], r["command_sequence"],
                                                       r["order_id"], r["order_status"], ex))
    print("  final AAPL bids:", d["final_state"]["books"][0]["bids"])
    print("  state_hash:", d["final_state"]["state_hash"])


def replay(path):
    r = json.load(open(path))
    if r["status"] in ("corrupt", "io_error"):
        print("  replay: %s (%s)" % (r["status"], r["error"]["message"]))
    elif "preserved_tail_path" in r:
        print("  --repair-tail: kept %d records, cut bytes preserved in %s"
              % (r["records"], r["preserved_tail_path"].split("/")[-1]))
    else:
        print("  replay --verify: status %s, %d records, state_hash %s..."
              % (r["status"], r["records"], r["state_hash"][:16]))


def retry(run_path, sim_path):
    lines = load_lines(run_path)
    print("  recovery: %d records replayed" % lines[0]["records_replayed"])
    for r in lines[1:-1]:
        if r.get("type") == "new_limit" and r["status"] == "accepted":
            print("  %s: seq %d duplicate=%s" % (r["request_id"], r["command_sequence"],
                                                 r["duplicate"]))
    sim = json.load(open(sim_path))["final_state"]["state_hash"]
    print("  final state equals the uninterrupted in-memory run:", lines[-1]["state_hash"] == sim)


def port(path):
    print(load_lines(path)[0]["port"])


def subscriber(path):
    for m in load_lines(path):
        if m["type"] == "snapshot_response":
            print("  subscriber: snapshot at event_watermark %d" % m["event_watermark"])
        elif m["type"] == "book_update":
            levels = [(x["side"], x["price"], x["quantity"]) for x in m["levels"]]
            print("  subscriber: event %d %s %s levels=%s" % (m["event_sequence"],
                                                             m["command_type"], m["symbol"],
                                                             levels))
        elif m["type"] == "subscriber_summary":
            print("  subscriber: %d events, gaps=%d" % (m["events"], m["gaps"]))


def shutdown(path):
    bye = load_lines(path)[-1]
    s = bye["stats"]
    print("  shutdown: %d commands, %d group commits; slow readers cut off: %d by queue "
          "overflow, %d by send timeout"
          % (bye["last_command_sequence"], s["batches"], s["slow_consumer_disconnects"],
             s["send_timeout_disconnects"]))


COMMANDS = {"simulate": simulate, "replay": replay, "retry": retry, "port": port,
            "subscriber": subscriber, "shutdown": shutdown}

if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] not in COMMANDS:
        print("usage: demo_report.py {%s} FILE..." % "|".join(COMMANDS), file=sys.stderr)
        sys.exit(2)
    COMMANDS[sys.argv[1]](*sys.argv[2:])
