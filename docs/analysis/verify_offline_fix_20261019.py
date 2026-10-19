"""Recheck saved offline-fix software artifacts; never opens a device port."""
import hashlib
import importlib.util
import json
from pathlib import Path

master = Path(__file__).resolve().parents[2]
slave = master.parent / "pump-muti-slave"
spec = importlib.util.spec_from_file_location("candidate", master / "tools/package_mesh_candidate.py")
candidate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(candidate)
packages = Path.home() / ".platformio/packages"

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

for name in ("mesh_tx_governor.h", "mesh_tx_diagnostics.h"):
    candidate.require((master / "include" / name).read_bytes() == (slave / "include" / name).read_bytes(),
                      "Shared header mismatch: " + name)
builds = {
    "master": candidate.firmware(master, "pump_master", "esp32",
        packages / "toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf-nm.exe", "offline-fix"),
    "slave": candidate.firmware(slave, "valve_slave_c3", "esp32c3",
        packages / "toolchain-riscv32-esp/bin/riscv32-esp-elf-nm.exe", "offline-fix"),
}
candidate.require(builds["master"]["build_id"] == "4c3a56d9" and builds["slave"]["build_id"] == "80ba7f64",
                  "Build IDs changed; update the reviewed report and deployment gate")
candidate.require("0x80ba7f64U" in (master / "include/mesh_deployment.h").read_text(), "Deployment gate mismatch")
checks = [
    (master, "host", ["28 test groups passed", "PASS recovery:", "PASS integrated fault recovery:",
        "PASS 1 nodes x 1000 batches", "PASS 2 nodes x 1000 batches", "PASS 5 nodes x 1000 batches",
        "PASS simulated 8h/5 nodes: 3340 completed batches"]),
    (slave, "host", ["PASS transport fault locks", "slave flow tests passed"]),
    (master, "sdk", ["PASS SDK host harness and complete component pinning"]),
    (master, "log-tools", ["Ran 7 tests", "OK"]),
]
evidence = []
for root, label, markers in checks:
    path = root / f".pio/{label}-offline-fix.log"
    log = path.read_text(encoding="utf-8")
    candidate.require(all(marker in log for marker in markers), "Missing evidence: " + str(path))
    evidence.append({"path": str(path), "sha256": digest(path), "required_markers": markers})
for root in (master, slave):
    for name in ("build-offline-fix.log", "compile_commands-offline-fix.json", "envdump-offline-fix.log"):
        path = root / ".pio" / name
        evidence.append({"path": str(path), "sha256": digest(path)})
record = {
    "date": "2026-10-19", "kind": "offline_fix_software_evidence_not_hardware_acceptance",
    "overall": "SOFTWARE_PASS_HARDWARE_PENDING", "firmware_uploaded": False,
    "serial_ports_opened": False, "pump_or_valve_actuated": False, "nvs_erased": False,
    "production_max_concurrent": 1, "business_recovery": "controlled restart required; no old task replay or automatic top-up",
    "diagnostic_recovery": "all real tickets settled, 1/2/4/8s backoff, one in flight, explicit non-authorizing allowlist",
    "missing_completion": "6000ms sticky restart requirement; late callbacks cannot clear it",
    "first_fault_evidence": "immutable RAM snapshot; retained through trace drain; lost on restart",
    "historical_first_trigger": "not captured; cannot distinguish SAR error, API/core rejection and LeaseAck expiry",
    "builds": builds, "evidence": evidence,
    "simulations": {"nodes": [1, 2, 5], "batches_each": 1000, "target_ml": 500,
        "target_pulses": 2940, "virtual_hours": 8, "virtual_batches": 3340, "rf_or_water": False,
        "master_and_slave_fault_integration": "PASS"},
    "app": {"changed": False, "tests_rerun_this_repair": False,
        "reference": "mesh-candidate-20261019-e0950038-495431ba",
        "reuse_requirement": "packager verifies archived APK against candidate hash and exact current source/config/test-log/XML equality with prior snapshot"},
    "pending": ["device fault injection and callback matrix", "1/2/5-node RF", "20 batches of 500mL per scale",
        "agreed measured-volume tolerance", "8-hour real bench", "runtime heap/stack peaks"],
    "report_sha256": digest(master / "docs/SLAVE_OFFLINE_FIX_20261019.md"),
}
path = master / "docs/SLAVE_OFFLINE_FIX_20261019.json"
path.write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"verification": "PASS", "report": str(path),
                  "master": builds["master"]["build_id"], "slave": builds["slave"]["build_id"]}, indent=2))
