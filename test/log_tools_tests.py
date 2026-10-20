"""Verify acceptance summaries handle capture boundaries, reboot and disorder."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("analyze_mesh_logs", Path(__file__).resolve().parents[1] / "tools/analyze_mesh_logs.py")
logs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(logs)


def tx(at, ticket, event, issued=10, segmented=1):
    return f"I ({at}) mesh_tx: t={at} ticket={ticket} peer=0002 op=c2 event={event} job=1:1 seq=4 issued={issued} age={(at-issued)&0xffffffff} wait=5 seg={segmented} err=0\n"


class LogTests(unittest.TestCase):
    def summarize(self, text):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.log"
            path.write_text(text, encoding="utf-8")
            return logs.analyze([path])

    def test_real_end_disorder_and_duplicate(self):
        result = self.summarize(tx(10, 1, 0) + tx(110, 1, 3) + tx(115, 1, 1) + tx(120, 1, 3))
        session = result["sessions"][0]
        self.assertEqual(session["sar_ms"]["count"], 1)
        self.assertEqual(session["sar_ms"]["max"], 100)
        self.assertEqual(session["unresolved_at_capture_end"], [])
        self.assertEqual(session["counters"]["duplicate_events"], 1)

    def test_capture_boundary_is_not_missing_callback_verdict(self):
        session = self.summarize(tx(10, 1, 0) + tx(20, 1, 1))["sessions"][0]
        self.assertFalse(session["unresolved_at_capture_end"][0]["overdue_6000ms"])
        self.assertIsNone(session["suggested_sar_p99_200ms_met"])

    def test_overdue_and_clock_wrap(self):
        issued = 0xfffffff0
        session = self.summarize(tx(issued, 1, 0, issued) + tx(6100, 2, 0, 6100, 0))["sessions"][0]
        self.assertTrue(session["unresolved_at_capture_end"][0]["overdue_6000ms"])

    def test_reboot_separates_reused_ticket(self):
        text = "I (1) build: id=12345678 protocol=2\n" + tx(10, 1, 0)
        text += "rst:0x1\nI (1) build: id=12345678 protocol=2\n" + tx(10, 1, 0) + tx(20, 1, 1) + tx(30, 1, 3)
        sessions = self.summarize(text)["sessions"]
        self.assertEqual(len(sessions), 2)
        self.assertEqual(len(sessions[0]["unresolved_at_capture_end"]), 1)
        self.assertEqual(sessions[1]["unresolved_at_capture_end"], [])

    def test_jsonl_multi_device_and_capture_error(self):
        lines = [{"kind": "serial", "source": "COM5", "text": tx(10, 1, 0)},
                 {"kind": "serial", "source": "COM6", "text": tx(10, 1, 0)},
                 {"kind": "capture_end", "errors": [{"error": "queue full"}]}]
        result = self.summarize("\n".join(json.dumps(x) for x in lines))
        self.assertEqual(len(result["sessions"]), 2)
        self.assertEqual(len(result["capture_errors"]), 1)

    def test_ack_staging_dedup_and_no_proof_claim(self):
        result = self.summarize("I (100) lease_rx: t=100 peer=0002 job=1:1 seq=1 result=OK matched=1 issued=10 age=90 applied=0\n" * 2)
        session = result["sessions"][0]
        self.assertEqual(session["matched_ok_lease_ack_ms"]["count"], 1)
        self.assertEqual(session["directly_applied_ok_lease_ack_ms"]["count"], 0)
        self.assertEqual(session["sar_ms"]["count"], 0)

    def test_partial_line_and_interrupted_capture(self):
        result = self.summarize('{"kind":"capture_start"}\nI (3) mesh_tx: t=3 ticket=1\n')
        self.assertEqual(len(result["capture_errors"]), 2)
        self.assertEqual(result["sessions"], [])

    def test_v4_epoch_acks_and_repeated_sequence_after_epoch_change(self):
        def ack(epoch, age):
            return f"I (100) lease_rx: t=100 peer=0002 epoch={epoch} seq=1 result=OK matched=1 issued=10 age={age} applied=1\n"
        result = self.summarize(ack(144, 46) * 2 + ack(145, 72))
        self.assertEqual(result["capture_errors"], [])
        session = result["sessions"][0]
        self.assertEqual(session["matched_ok_lease_ack_ms"]["count"], 2)
        self.assertEqual(session["directly_applied_ok_lease_ack_ms"]["max"], 72)
        self.assertEqual(session["counters"]["lease_rx"], 3)

    def test_missing_ack_scope_and_serial_capture_error_are_reported(self):
        text = "I (100) lease_rx: t=100 peer=0002 seq=1 result=OK matched=1 age=20 applied=1\n"
        text += json.dumps({"kind": "error", "port": "COM5", "text": "device disconnected"})
        result = self.summarize(text)
        self.assertEqual(len(result["capture_errors"]), 2)
        self.assertEqual(result["sessions"], [])

    def test_stack_overflow_before_build_and_reset_are_retained(self):
        text = "***ERROR*** A stack overflow in task BTC_TASK has been detected.\n"
        text += "rst:0xc (SW_CPU_RESET),boot:0x13\nI (1) build: id=12345678 protocol=4\n"
        text += tx(10, 1, 0, segmented=0) + tx(20, 1, 1, segmented=0)
        result = self.summarize(text)
        self.assertEqual(len(result["device_failures"]), 1)
        self.assertIn("BTC_TASK", result["device_failures"][0]["line"])
        self.assertEqual(len(result["reset_events"]), 1)
        self.assertEqual(result["capture_errors"], [])
        self.assertEqual(result["sessions"][0]["unresolved_at_capture_end"], [])


if __name__ == "__main__":
    unittest.main()
