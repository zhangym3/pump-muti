"""Offline arithmetic supporting the 2026-10-19 Mesh optimization proposal.

This is a message-size and scheduling budget, not an RF/airtime simulation.
Only the Python standard library is required. No device is accessed.
Usage: python docs/analysis/mesh_volume_budget_20261019.py --output PATH
"""
import argparse
import json
from pathlib import Path


def segment_count(payload_bytes):
    # Vendor opcode = 3 bytes. Current SDK path uses a short, 4-byte TransMIC.
    # Access PDUs of at most 11 bytes can use unsegmented transport.
    access_bytes = payload_bytes + 3
    return 1 if access_bytes <= 11 else (access_bytes + 4 + 11) // 12


def calculate():
    payloads = {
        "Prepare": 8 + 5 * 4,
        "Start": 8,
        "Lease": 8 + 4,
        "Close": 8 + 1,
        "StatusGet": 8,
        "Status": 8 + 4 * 4 + 3,
        "Summary": 8 + 4 + 4 + 1 + 1,
        "EventAck": 8 + 4,
        "Hello": 4 + 4 + 1,
        "HelloReply": 4 + 1 + 1,
        "LeaseAck": 8 + 4 + 1,
        "CommandReply": 8 + 1 + 1,
        "Request_uniform": 1 + 4 + 8 + 4 + 4 + 4 + 4 + 8,
        "Request_individual": 1 + 4 + 8 + 4 + 4 + 4 + 4 + 32,
        "Response": 8 + 4 + 1 + 8 + 1 + 4 * 4 + 4 + 4,
    }
    messages = [
        {"message": name, "payload_bytes": size,
         "access_bytes": size + 3,
         "segmented": size + 3 > 11,
         "lower_transport_data_pdus": segment_count(size)}
        for name, size in payloads.items()
    ]
    # t=0 is the first START submission, which follows the Opening transition
    # by at least a control-loop iteration. Ignore all queues, loss and ACKs.
    opening = []
    for nodes in (1, 4, 5, 8, 16, 24, 27, 28, 32):
        last_start = (nodes - 1) * 150
        opening.append({
            "active_nodes": nodes,
            "ideal_last_start_submission_ms": last_start,
            "margin_to_master_opening_4000_ms": 4000 - last_start,
            "margin_to_first_slave_lease_3000_ms": 3000 - last_start,
        })
    # Base steady-state application data PDUs, before network repeats/ALL SAR
    # repeats/ACKs/beacons. Assume each Hello triggers one StatusGet and
    # one Status/EventAck round-trip, no additional status retries, no motion
    # phase transitions, and that active nodes still have their valve ON.
    # All online, subscribed group receivers ACK a valid Lease, even when the
    # job is not theirs (STALE). Assume all registered nodes are online here.
    hello_cycle = sum(segment_count(payloads[k]) for k in
                      ("Hello", "HelloReply", "StatusGet", "Status", "EventAck"))
    loads = []
    for registered, active in ((1, 1), (4, 1), (5, 1), (8, 1), (16, 1), (32, 1),
                               (4, 4), (5, 5), (8, 8), (16, 16), (24, 24),
                               (32, 5), (32, 32)):
        group_receivers = registered
        lease = segment_count(payloads["Lease"])
        ack = group_receivers * segment_count(payloads["LeaseAck"])
        summary = active * segment_count(payloads["Summary"]) / 2
        hello = registered * hello_cycle / 10
        total = lease + ack + summary + hello
        # Local IDF 5.4 Mesh v1.0 group SAR: initial send plus four repeats,
        # even with a healthy link. Unicast extra retransmits remain excluded.
        group_sar_extra = lease * 4
        with_fixed_group_repeats = total + group_sar_extra
        loads.append({
            "registered_nodes": registered, "active_nodes": active,
            "online_lease_group_receivers": group_receivers,
            "lease_data_pdus_per_second": lease,
            "lease_ack_data_pdus_per_second": ack,
            "summary_data_pdus_per_second": summary,
            "hello_query_data_pdus_per_second": hello,
            "total_nominal_data_pdus_per_second": round(total, 2),
            "after_network_count_1_nominal_repeats": round(total * 2, 2),
            "fixed_group_sar_extra_data_pdus_per_second": group_sar_extra,
            "including_fixed_group_sar_data_pdus_per_second": round(with_fixed_group_repeats, 2),
            "including_fixed_group_sar_after_network_count_1": round(with_fixed_group_repeats * 2, 2),
        })
    optimized = []
    # Proposed: one unicast Lease/ACK per active node per second; suppress
    # routine Hello-triggered full queries, retaining Hello and HelloReply.
    for registered, active in ((5, 1), (5, 5), (32, 5)):
        lease = active * segment_count(payloads["Lease"])
        ack = active * segment_count(payloads["LeaseAck"])
        summary = active * segment_count(payloads["Summary"]) / 2
        hello = registered * sum(segment_count(payloads[k]) for k in
                                 ("Hello", "HelloReply")) / 10
        total = lease + ack + summary + hello
        optimized.append({
            "registered_nodes": registered, "active_nodes": active,
            "unicast_lease_data_pdus_per_second": lease,
            "lease_ack_data_pdus_per_second": ack,
            "summary_data_pdus_per_second": summary,
            "hello_reply_data_pdus_per_second": hello,
            "total_data_pdus_per_second_without_extra_unicast_retransmits": round(total, 2),
            "after_network_count_1": round(total * 2, 2),
        })
    # Little's-law necessary load checks. Common illustrative mean lifetime S
    # is NOT a measured distribution, a worst-case bound or a schedulability proof.
    utilization = []
    for lifetime_ms in (100, 200, 300, 400, 550, 1000, 2200):
        lifetime_s = lifetime_ms / 1000
        master = 5 * lifetime_s / 2
        slave = 1.6 * lifetime_s
        utilization.append({
            "illustrative_mean_sar_lifetime_ms": lifetime_ms,
            "master_required_segmented_rate_per_second": 5,
            "master_window_2_necessary_utilization": round(master, 3),
            "slave_required_segmented_rate_per_second": 1.6,
            "slave_window_1_necessary_utilization": round(slave, 3),
            "master_has_strict_mean_capacity_margin": master < 1,
            "slave_has_strict_mean_capacity_margin": slave < 1,
        })
    # Delayed ACK counterexample: use this job's actual issuedAt, never rxAt,
    # as the conservative authorization origin. All times are hypothetical.
    ack_examples = []
    issued_at = 0
    slave_accepts_at = 100
    for ack_at in (900, 1100, 1800, 2500):
        ack_examples.append({
            "issued_at_ms": issued_at,
            "hypothetical_slave_accepts_at_ms": slave_accepts_at,
            "ack_received_at_ms": ack_at,
            "accepted_under_proposed_2400_ms_proof_window": ack_at - issued_at < 2400,
            "conservative_master_proof_expires_at_ms": issued_at + 2400,
            "incorrect_rx_time_based_expiry_ms": ack_at + 2400,
            "hypothetical_slave_lease_expires_at_ms": slave_accepts_at + 3000,
            "incorrect_master_expiry_after_slave_ms": max(0, ack_at + 2400 - (slave_accepts_at + 3000)),
        })
    return {
        "analysis_date": "2026-10-19",
        "reference_heads": {"master": "ad43f28", "slave": "56821a3", "app": "4c8ea4a"},
        "scope": "Offline arithmetic only; not measured throughput, airtime or supported concurrency.",
        "assumptions": {"vendor_opcode_bytes": 3, "short_transmic_bytes": 4,
                        "unsegmented_access_limit_bytes": 11,
                        "segmented_data_bytes_per_pdu": 12,
                        "master_start_global_interval_ms": 150,
                        "master_opening_timeout_ms": 4000,
                        "slave_lease_timeout_ms": 3000,
                        "current_max_concurrent": 1,
                        "network_transmit_count": 1,
                        "local_group_sar_total_rounds": 5,
                        "local_group_sar_retransmit_timer_ms": 50,
                        "group_timer_is_not_total_lifetime_bound": True,
                        "extra_unicast_sar_retransmits_excluded": True,
                        "group_receiver_assumption": "All registered nodes are online and subscribed; all ACK a valid Lease, including STALE replies.",
                        "steady_state_formula": "2 + 2*group_receivers + 1.5*active + 0.9*registered",
                        "including_fixed_group_sar_formula": "10 + 2*group_receivers + 1.5*active + 0.9*registered",
                        "proposed_unicast_optimized_formula": "5.5*active + 0.3*registered",
                        "bearer_assumption": "Advertising data only, network count=1; excludes relays, proxy forwarding, SAR ACKs, beacons, management and phase events."},
        "messages": messages,
        "ideal_opening_first_sweep": opening,
        "hello_query_cycle_data_pdus": hello_cycle,
        "steady_state_nominal_load": loads,
        "proposed_unicast_optimized_load": optimized,
        "necessary_window_capacity_check": {
            "scope": "Illustrative mean-load check only; passing is necessary, not sufficient. Queue bursts, deadlines and nonpreemptible SAR still matter.",
            "master_mean_sar_lifetime_must_be_less_than_ms": 400,
            "slave_mean_sar_lifetime_must_be_less_than_ms": 625,
            "steady_state_rates_exclude_phase_event_bursts": True,
            "scenarios": utilization,
        },
        "lease_ack_freshness_counterexamples": ack_examples,
        "five_channel_assessment": {
            "scope": "Arithmetic scenarios, not measured peaks or a probability of failure.",
            "ideal_start_submission_times_ms": [i * 150 for i in range(5)],
            "startup_first_exchange_data_pdus": {
                "assumption": "One Prepare/Status/EventAck and one Start/Status/EventAck per node; no retries, Hello, Summary or additional phase events.",
                "preparing": 5 * sum(segment_count(payloads[k]) for k in
                                     ("Prepare", "Status", "EventAck")),
                "opening": 5 * sum(segment_count(payloads[k]) for k in
                                   ("Start", "Status", "EventAck")),
                "preparing_master_segmented_message_submissions": 5 * 2,
                "submission_count_is_not_simultaneous_occupancy": True,
            },
            "steady_state_five_online_five_active": {
                "master_segmented_messages_per_second": 1 + 5 / 10,
                "each_slave_segmented_messages_per_second": 1 + 1 / 2 + 2 / 10,
                "master_data_pdus_per_second": 2 + 5 * (1 + 1 + 2) / 10,
                "each_slave_data_pdus_per_second": 2 + 3 / 2 + (2 + 3) / 10,
            },
            "single_remaining_unconfirmed_node": {
                "assumption": "A new segmented Status for every 150 ms retry; no context released, no other traffic, first submission at t=0.",
                "tx_contexts_per_device": 8,
                "eighth_submission_ms": 7 * 150,
                "ninth_submission_ms": 8 * 150,
                "interpretation": "The ninth submission can fail with EBUSY under this no-release condition; not a prediction that every run fails at this time.",
            },
        },
        "duplicate_start_amplification": {
            "assumption": "One forced Status and one EventAck for each 150 ms retry; all delivered.",
            "status_and_event_ack_data_pdus_per_second": round(
                (segment_count(payloads["Status"]) + segment_count(payloads["EventAck"])) / .15, 3),
            "including_start_data_pdus_per_second": round(
                sum(segment_count(payloads[k]) for k in ("Start", "Status", "EventAck")) / .15, 3),
        },
        "metering": {"k_pulses_per_litre": 5880,
                     "target_500_ml_pulses": (500 * 5880 + 500) // 1000,
                     "nominal_ml_per_pulse": round(1000 / 5880, 6)},
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    rendered = json.dumps(calculate(), ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
        print(f"Wrote {args.output}")
    else:
        print(rendered, end="")
