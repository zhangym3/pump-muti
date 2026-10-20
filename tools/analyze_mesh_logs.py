"""Summarize tracked Mesh timing from raw text or capture_mesh_logs JSONL.

This never issues a hardware acceptance verdict. Unresolved tickets at the end
of a capture and lost log lines are not proof of a missing SDK callback.
"""
import argparse
from collections import Counter, OrderedDict
import json
import math
from pathlib import Path
import re

ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
TAG = re.compile(r"\b(mesh_tx|mesh_diag|lease_rx|cap_rx|build|output|schedule):\s*(.*)")
FIELDS = re.compile(r"(\w+)=([^\s]+)")
OLD_SEGMENTED_OPS = {"c0", "c2", "c3", "c5", "c6", "c7", "c8", "c9", "ca", "cd", "ce", "cf", "d1"}


def statistics(values):
    values = sorted(values)
    if not values:
        return {"count": 0, "p50": None, "p95": None, "p99": None, "max": None}
    result = {"count": len(values), "max": values[-1]}
    for p in (50, 95, 99):
        result["p" + str(p)] = values[max(0, math.ceil(len(values) * p / 100) - 1)]
    return result


def analyze(paths):
    sessions, boots = {}, Counter()
    capture_errors = []
    device_failures, reset_events = [], []
    for path in paths:
        path = Path(path)
        saw_capture_start = saw_capture_end = False
        # Stream long bench captures instead of loading all serial text.
        for raw in _lines(path):
            source, line = str(path), raw
            if raw.startswith("{"):
                try:
                    record = json.loads(raw)
                except json.JSONDecodeError:
                    capture_errors.append({"file": str(path), "error": "Invalid JSONL record"})
                    continue
                if not isinstance(record, dict):
                    capture_errors.append({"file": str(path), "error": "JSONL record is not an object"})
                    continue
                if record.get("kind") == "capture_start":
                    saw_capture_start = True
                if record.get("kind") == "capture_end":
                    saw_capture_end = True
                    capture_errors.extend(record.get("errors", []))
                if record.get("kind") == "error":
                    capture_errors.append({"file": str(path), "source": record.get("source", record.get("port")),
                                           "error": record.get("text", "Capture error")})
                if record.get("kind") != "serial":
                    continue
                source += ":" + record.get("source", "unknown")
                line = record.get("text", "")
            line = ANSI.sub("", line)
            if any(marker in line for marker in ("***ERROR*** A stack overflow", "Guru Meditation Error",
                                                  "abort() was called", "Brownout detector was triggered",
                                                  "Task watchdog got triggered")):
                device_failures.append({"source": source, "line": line.rstrip()})
            if "rst:0x" in line:
                reset_events.append({"source": source, "line": line.rstrip()})
            found = TAG.search(line)
            if not found:
                # ESP32 boot banners delimit ticket numbering even if build log is missing.
                if "rst:0x" in line:
                    boots[source] += 1
                continue
            tag, body = found.groups()
            fields = dict(FIELDS.findall(body))
            required = {"mesh_tx": ("ticket", "event", "issued", "age", "peer", "op"),
                        "lease_rx": ("peer", "seq", "age", "result", "matched", "applied")}.get(tag, ())
            numeric = ("t", "ticket", "event", "issued", "age", "wait", "err")
            if (tag == "lease_rx" and not ("job" in fields or "epoch" in fields)) or any(name not in fields for name in required) or any(
                    name in fields and not re.fullmatch(r"-?\d+", fields[name]) for name in numeric):
                capture_errors.append({"file": str(path), "error": "Incomplete/malformed " + tag + " line"})
                continue
            key = (source, boots[source])
            if tag == "build" and key in sessions and sessions[key]["lines"]:
                boots[source] += 1
                key = (source, boots[source])
            session = sessions.setdefault(key, {"lines": 0, "tickets": {}, "completed": OrderedDict(), "sar": [], "wait": [],
                                                "acks": {}, "proofs": {}, "counts": Counter(),
                                                "maxima": {}, "last_t": None, "build": None})
            session["lines"] += 1
            if "t" in fields:
                session["last_t"] = int(fields["t"])
            if tag == "build":
                session["build"] = fields
            elif tag == "mesh_diag":
                for name, value in fields.items():
                    if value.isdecimal():
                        session["maxima"][name] = max(session["maxima"].get(name, 0), int(value))
                if fields.get("healthy") == "0":
                    session["counts"]["unhealthy_snapshots"] += 1
            elif tag == "mesh_tx":
                ticket = fields["ticket"]
                event = int(fields["event"])
                if ticket in session["completed"]:
                    state = session["completed"][ticket]
                    if event in state["events"]:
                        session["counts"]["duplicate_events"] += 1
                    state["events"].add(event)
                    continue
                state = session["tickets"].setdefault(ticket, {"events": set(), "fields": fields})
                if event in state["events"]:
                    session["counts"]["duplicate_events"] += 1
                    continue
                state["events"].add(event)
                state["fields"] = fields
                if int(fields.get("err", "0")):
                    session["counts"]["error_events"] += 1
                if event == 0 and "wait" in fields:
                    session["wait"].append(int(fields["wait"]))
                segmented = fields.get("seg") == "1" if "seg" in fields else fields["op"].lower() in OLD_SEGMENTED_OPS
                if event == 3 and segmented:
                    session["sar"].append(int(fields["age"]))
                if 2 in state["events"] or (1 in state["events"] and (not segmented or 3 in state["events"])):
                    session["completed"][ticket] = state
                    del session["tickets"][ticket]
                    if len(session["completed"]) > 2048:
                        session["completed"].popitem(last=False)
            elif tag == "lease_rx":
                # V4 uses a boot-global sequence within epoch; legacy logs
                # include the full job instead. Never merge different epochs.
                scope = ("job", fields["job"]) if "job" in fields else ("epoch", fields["epoch"])
                ack_key = (fields["peer"], scope, fields["seq"])
                session["counts"]["lease_rx"] += 1
                if fields.get("matched") == "1" and fields.get("result") == "OK":
                    session["acks"].setdefault(ack_key, int(fields["age"]))
                    if fields.get("applied") == "1":
                        session["proofs"].setdefault(ack_key, int(fields["age"]))
                elif fields.get("result") != "OK":
                    session["counts"]["lease_non_ok"] += 1
            elif tag == "cap_rx" and fields.get("applied") == "1":
                session["counts"]["capability_responses_applied"] += 1
        if saw_capture_start and not saw_capture_end:
            capture_errors.append({"file": str(path), "error": "Capture has no end record"})
    reports = []
    for (source, boot), session in sessions.items():
        unresolved = []
        for ticket, state in session["tickets"].items():
            fields, events = state["fields"], state["events"]
            segmented = fields.get("seg") == "1" if "seg" in fields else fields["op"].lower() in OLD_SEGMENTED_OPS
            needs_end = segmented and 2 not in events and 3 not in events
            needs_accept = not ({1, 2} & events)
            if 0 in events and (needs_end or needs_accept):
                age = None if session["last_t"] is None else (session["last_t"] - int(fields["issued"])) & 0xffffffff
                unresolved.append({"ticket": ticket, "peer": fields["peer"], "op": fields["op"],
                                   "needs_stack_result": needs_accept, "needs_end": needs_end,
                                   "observed_age_ms": age, "overdue_6000ms": age is not None and age >= 6000})
        sar, ack = statistics(session["sar"]), statistics(list(session["acks"].values()))
        reports.append({"source": source, "boot_index": boot, "build": session["build"], "parsed_lines": session["lines"],
                        "sar_ms": sar, "queue_wait_ms": statistics(session["wait"]), "matched_ok_lease_ack_ms": ack,
                        "directly_applied_ok_lease_ack_ms": statistics(list(session["proofs"].values())),
                        "suggested_sar_p99_200ms_met": None if not sar["count"] else sar["p99"] <= 200,
                        "suggested_lease_p99_800ms_met": None if not ack["count"] else ack["p99"] <= 800,
                        "counters": dict(session["counts"]), "diagnostic_maxima": session["maxima"],
                        "unresolved_at_capture_end": unresolved})
    return {"kind": "log timing summary; not hardware acceptance", "sessions": reports, "capture_errors": capture_errors,
            "device_failures": device_failures, "reset_events": reset_events,
            "notes": ["Unresolved at capture boundary is not proof of a missing callback; verify complete capture and trace_lost.",
                      "ACK applied=0 may be staged before StackAccepted; the later proof application has no separate receive line.",
                      "No samples means no evidence. Percentiles do not override per-task authorization deadlines.",
                      "SAR age runs from governor commit before API to end; it includes BTC scheduling delay."]}


def _lines(path):
    with path.open(encoding="utf-8", errors="replace") as stream:
        yield from stream


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", type=Path, nargs="+")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = json.dumps(analyze(args.logs), indent=2, ensure_ascii=False) + "\n"
    if args.output:
        args.output.write_text(result, encoding="utf-8")
    else:
        print(result, end="")
