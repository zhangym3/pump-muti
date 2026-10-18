"""Verify the static role boundary between the ESP32 master and ESP32-C3 slave."""
from pathlib import Path
import json
import sys

master = Path(__file__).resolve().parents[1]
slave = master.parent / "pump-muti-slave"
app = master.parent / "pumpmutiapp" / "app" / "src"

def files(root):
    return [p for folder in ("src", "include") for p in (root / folder).rglob("*") if p.is_file()] + [root / "platformio.ini"]

def text(root):
    return "\n".join(p.read_text(encoding="utf-8", errors="ignore") for p in files(root))

errors = []
master_text = text(master)
slave_text = text(slave)
if "PUMP_ROLE_MASTER" in master_text or "board::IsMaster" in master_text:
    errors.append("master still has a compile-time role switch")
if "PUMP_ROLE_MASTER" in slave_text or "board::IsMaster" in slave_text:
    errors.append("slave still has a compile-time role switch")
for marker in ("master_scheduler.cpp", "pump_safety_controller.cpp", "mesh_management.cpp"):
    if marker in (slave / "src" / "CMakeLists.txt").read_text(encoding="utf-8"):
        errors.append(f"slave target still compiles {marker}")
for marker in ("MasterScheduler", "PumpSafetyController", "scanSlaves", "joinSlave", "ESP_BLE_MESH_CFG_CLI"):
    if marker in slave_text:
        errors.append(f"slave product sources still contain master-only symbol {marker}")
for marker in ("ValveIO", "FlowReading", "SlaveInterlock", "BranchFlow", "void valve("):
    if marker in master_text:
        errors.append(f"master product sources still contain slave hardware symbol {marker}")
if "board = esp32dev" not in (master / "platformio.ini").read_text(encoding="utf-8"):
    errors.append("master board target is not esp32dev")
if "board = esp32-c3-devkitm-1" not in (slave / "platformio.ini").read_text(encoding="utf-8"):
    errors.append("slave board target is not esp32-c3-devkitm-1")
for relative in ("include/protocol.h", "src/protocol.cpp", "include/control_request.h", "docs/PROTOCOL_VECTORS.md"):
    left, right = master / relative, slave / relative
    if not left.is_file() or not right.is_file():
        errors.append(f"protocol contract file missing: {relative}")
    elif left.read_bytes() != right.read_bytes():
        errors.append(f"protocol contract drift: {relative}")
app_text = "\n".join(p.read_text(encoding="utf-8", errors="ignore") for p in app.rglob("*") if p.is_file()) if app.is_dir() else ""
if "meshNeedsMigration" in app_text or "mesh_migration_" in app_text:
    errors.append("APP still exposes the removed Mesh migration state")
pump_screens = app / "main" / "java" / "com" / "cczb" / "app" / "PumpScreens.kt"
if pump_screens.is_file() and "若 Master 保留旧 Mesh Node 角色" in pump_screens.read_text(encoding="utf-8", errors="ignore"):
    errors.append("APP settings still describe automatic legacy Mesh migration")
for root, role, chip, board in ((master, "pump_master", "esp32", "esp32dev"),
                                (slave, "valve_slave_c3", "esp32c3", "esp32-c3-devkitm-1")):
    manifest_path = root / "dist" / "manifest.json"
    if not manifest_path.is_file():
        errors.append(f"missing firmware manifest: {manifest_path}")
        continue
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if (manifest.get("role"), manifest.get("chip"), manifest.get("board")) != (role, chip, board):
            errors.append(f"manifest identity mismatch: {manifest_path}")
        if any(not str(item.get("path", "")).startswith(role + "/") for item in manifest.get("files", [])):
            errors.append(f"manifest contains an image from another role: {manifest_path}")
    except (OSError, ValueError) as exc:
        errors.append(f"invalid firmware manifest {manifest_path}: {exc}")
if errors:
    for error in errors:
        print(f"ERROR: {error}")
    raise SystemExit(1)
print("role boundary: PASS")
print("master: ESP32 Provisioner/pump only")
print("slave: ESP32-C3 Node/valve/flow/interlock only")
