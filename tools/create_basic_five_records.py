"""Create empty, reviewable bench records. Never opens ports or runs outputs."""
import csv
from itertools import combinations
from pathlib import Path


def main():
    target = Path(__file__).resolve().parents[1] / "docs/basic-five-records"
    target.mkdir(parents=True, exist_ok=True)

    def write(name, fields, rows):
        # Refuse to overwrite operator measurements on a later invocation.
        with (target / name).open("x", newline="", encoding="utf-8-sig") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            for row in rows:
                writer.writerow({"result": "NOT_TESTED", **row})

    write("node-inventory.csv", ["node", "address", "alias", "build_id", "k", "k_version",
          "commissioned", "wiring", "result", "notes"], [{"node": n} for n in range(1, 6)])
    b = [{"round": r, "node": n,
          "planned_phone": "CONNECTED" if r <= 10 else "DISCONNECTED",
          "planned_group_cold_boot": "YES" if r in (1, 11) else "NO"}
         for r in range(1, 21) for n in range(1, 6)]
    write("B-communication.csv", ["round", "node", "planned_phone", "actual_phone",
          "planned_group_cold_boot", "actual_group_cold_boot", "address", "operation_kind",
          "coverage_or_substitute", "appkey_readback", "bind_readback", "subscription_readback",
          "hello", "duration_ms", "readcheck_1", "readcheck_2", "readcheck_3", "capabilities",
          "k_and_version", "commissioned", "ready", "online", "restart_required",
          "target_sar_timeout_count", "pump_pwm", "all_five_valve_outputs", "master_log",
          "slave_log", "seq_auth", "result", "notes"], b)

    tasks = []
    for n in range(1, 6):
        for ml in (200, 500):
            for repeat in range(1, 6):
                tasks.append(((n,), ml, repeat, "SINGLE"))
    for ml in (200, 500):
        for repeat in range(1, 6):
            tasks.append((tuple(range(1, 6)), ml, repeat, "FIVE"))
    for count in (2, 3, 4):
        tasks.extend((nodes, 500, 1, "COMBINATION") for nodes in combinations(range(1, 6), count))
    branches = []
    task_rows = []
    for number, (nodes, ml, repeat, kind) in enumerate(tasks, 1):
        common = {"task": f"C{number:03}", "mask": f"0x{sum(1 << (n-1) for n in nodes):02X}",
                  "nodes": "+".join(map(str, nodes)), "target_ml_each": ml, "repeat": repeat, "kind": kind}
        task_rows.append(common)
        branches.extend({**common, "node": n, "allowed_abs_error_ml": max(10, ml * .05)} for n in nodes)
    assert len(task_rows) == 85 and len(branches) == 170
    assert len({r["mask"] for r in task_rows}) == 31
    common_fields = ["task", "mask", "nodes", "target_ml_each", "repeat", "kind"]
    write("C-tasks.csv", common_fields + ["job_key", "phone", "speed_percent",
          "all_selected_overlap", "unselected_outputs_closed", "no_completed_valve_reopened",
          "last_valve_close_to_pwm_off_ms", "continuous_run_sequence", "logs", "result", "notes"], task_rows)
    write("C-branch-measurements.csv", common_fields + ["node", "address", "job_key", "k",
          "k_version", "allowed_abs_error_ml", "actual_ml", "signed_error_ml", "reported_ml",
          "valve_open_ms", "start_proof_ms", "first_pulse_ms", "pump_on_ms", "valve_close_ms",
          "tail_settled_ms", "end_reason", "measured_or_unknown", "logs", "result", "notes"], branches)

    cases = []
    def add(case, scope, criterion, repeats=1):
        for repeat in range(1, repeats + 1):
            cases.append({"case": case, "scope": scope, "repeat": repeat, "criterion": criterion})
    add("PHONE_LOSS", "five lanes", "autonomous finish; reconnect same JobKey; no second start", 3)
    add("ACCEPT_REPLY_LOSS_STOP", "five lanes", "persist stop intent; query original key; stop original task; no duplicate start")
    add("STOP_LATENCY", "five lanes", "valid master stop to PWM off <=50ms; then all valves closed", 5)
    add("MASTER_LINK_LOSS", "five lanes", "pump stops at last confirmed lease send +8s plus measured loop delay")
    add("MASTER_REBOOT", "five lanes", "local output off; task aborts; no automatic refill")
    add("CONFIG_CALLBACK_LOSS", "configuration", "18s callback/75s step/330s configuration/420s provisioning deadlines; no late resurrection")
    add("UNTRUSTED_TRANSPORT", "five lanes", "fault latch; only trustworthy old results permit explicit new job after controlled recovery")
    for n in range(1, 6):
        scope = f"node {n} in five-lane task"
        add("SLAVE_LINK_LOSS", scope, "slave closes at last valid lease receive +12s plus measured loop delay; whole task stops")
        add("SLAVE_REBOOT", scope, "valve off; whole task aborts; unknown never reported as successful")
        for case in ("NO_FLOW", "FLOW_STALL", "RUN_LIMIT"):
            add(case, scope, "whole task stops; pump off before close requests; reason and results agree")
        add("BUTTON_RELEASE", f"node {n} maintenance", "authorized press opens only this valve; release to GPIO off <=50ms; pump stays off")
        add("HELD_BUTTON_EXPIRY", f"node {n} maintenance", "expiry closes; held button cannot reopen; release and valid authorization required")
    for case in ("UNAUTHORIZED_BUTTON", "BOOT_HELD_BUTTON", "BUTTON_DURING_PREPARE",
                 "UNSELECTED_BUTTON_DURING_RUN", "FINISHED_BRANCH_BUTTON", "BUTTON_DURING_TAIL",
                 "BUTTON_WHILE_RESULT_PENDING"):
        add(case, "all five nodes", "no unauthorized valve open; shared pump interlock remains enforced")
    for boundary in ("SLAVE_RESULT_WRITE", "SLAVE_RESULT_READBACK", "MASTER_RESULT_WRITE",
                     "MASTER_RESULT_READBACK", "RESULT_ACK", "NEW_FENCE"):
        for fault in ("REBOOT", "STORAGE_FAILURE"):
            add(boundary + "_" + fault, "actual Application/NVS/RTOS", "saved values recover; unproved results unknown; no early ACK/new job; stop/query still work")
    for case in ("MAINT_MASTER_REBOOT", "MAINT_SLAVE_REBOOT", "MAINT_LINK_LOSS",
                 "OLD_GRANT_AFTER_RETIRE", "GRANT_ACK_LOSS", "RETIRE_ACK_LOSS", "MAINT_DELETE_OR_READDR"):
        add(case, "maintenance session", "expiry closes valve; no pump until trusted close/retirement; timeout or inventory change never unlocks")
    add("INVENTORY_REORDER", "APP active task", "LIVE/FLOW/final results retain frozen task address mapping")
    add("OLD_FIRMWARE_CAPACITY", "APP/master", "no capability means concurrency 1; reject excessive mask; never silently serialize")
    add("CONTINUOUS_MULTI_TASKS", "at least 10 consecutive multi-lane tasks", "no duplicate operation or lost prior results; may reference C task IDs")
    write("D-protection-and-timing.csv", ["case", "scope", "repeat", "criterion", "build_ids",
          "job_key_or_session", "actual_injection_point", "actual_behavior", "measured_latency_ms",
          "max_control_interval_ms", "heap_stack_queue_peaks", "logs_or_scope_trace", "result", "notes"], cases)
    print(f"Created empty records: B={len(b)} node-rounds, C={len(task_rows)} tasks/{len(branches)} branches/31 masks, D={len(cases)} checks")


if __name__ == "__main__":
    main()
