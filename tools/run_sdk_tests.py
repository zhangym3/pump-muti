"""Execute extracted, patched SDK branches with host stubs; no radio or device IO.

This checks the actual project component, not a second implementation of its
tracked bridge. Core radio/allocator/RTOS dependencies remain controlled stubs.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def rejected(action, label):
    try:
        action()
    except RuntimeError:
        print("PASS pinned SDK rejects " + label, flush=True)
        return
    raise AssertionError("SDK unexpectedly accepted " + label)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--framework", type=Path, required=True)
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location("prepare_mesh_sdk", root / "tools/prepare_mesh_sdk.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    patch_material = ((root / "tools/mesh_sdk/patch.json").read_bytes() +
                      (root / "tools/mesh_sdk/bt-baseline.json").read_bytes())
    patch_id = hashlib.sha256(patch_material).hexdigest()[:16]
    output = root / ".pio/sdk-tests" / patch_id
    output.mkdir(parents=True, exist_ok=True)
    project = output / "materialized-project"
    project.mkdir(exist_ok=True)
    module.prepare(args.framework, project)
    component = project / ".mesh-sdk/bt/esp_ble_mesh"
    api = (component / "api/core/esp_ble_mesh_networking_api.c").read_text(encoding="utf-8")
    btc = (component / "btc/btc_ble_mesh_prov.c").read_text(encoding="utf-8")
    core = (component / "core/transport.c").read_text(encoding="utf-8")
    sar_markers = ("SAR_TX_START", "SAR_TX_RETRY", "SAR_TX_EXHAUST", "SAR_TX_DONE",
                   "SAR_ACK_RX", "SAR_ACK_TX", "SAR_ACK_UNMATCHED", "SAR_RX_ALLOC",
                   "SAR_RX_SEG", "SAR_RX_COMPLETE", "SAR_RX_DUP", "SAR_RX_TIMEOUT",
                   "SAR_RX_REPLACED")
    missing = [marker for marker in sar_markers if marker not in core]
    if missing:
        raise AssertionError("Patched SDK is missing SAR diagnostics: " + ", ".join(missing))
    sar_lines = "\n".join(line for line in core.splitlines() if "SAR_" in line)
    if "bt_hex(" in sar_lines or "SAR_DIAG_LOG" not in core:
        raise AssertionError("SAR diagnostics must not include packet payload bytes")
    if "#if defined(PUMP_SAR_DIAGNOSTICS)" not in core:
        raise AssertionError("SAR diagnostics must be restricted to diagnostic builds")
    print("PASS patched SDK exposes address/SeqAuth/segment/ACK diagnostics without payload", flush=True)
    pieces = [function(btc, "void btc_ble_mesh_model_arg_deep_copy("),
              function(btc, "void btc_ble_mesh_model_arg_deep_free("),
              function(btc, "static void pump_tracked_end("),
              "static const struct bt_mesh_send_cb pump_tracked_callbacks = { .end = pump_tracked_end };\n",
              function(core, "static uint32_t seg_tx_unacked_mask("),
              function(core, "static inline void seg_tx_complete("),
              function(core, "void bt_mesh_tx_reset(void)"),
              function(core, "void bt_mesh_tx_reset_single("),
              function(api, "static esp_err_t ble_mesh_model_send_msg("),
              function(api, "esp_err_t esp_ble_mesh_server_model_send_msg("),
              function(api, "esp_err_t esp_ble_mesh_server_model_send_msg_tracked(")]
    handler = btc[btc.index("void btc_ble_mesh_model_call_handler("):]
    server = handler[handler.index("case BTC_BLE_MESH_ACT_SERVER_MODEL_SEND:"):handler.index("case BTC_BLE_MESH_ACT_CLIENT_MODEL_SEND:")]
    pieces.append("static void handle_server(btc_ble_mesh_model_args_t *arg) { int err; switch (BTC_BLE_MESH_ACT_SERVER_MODEL_SEND) {\n" + server + "} }\n")
    generated = output / "sdk_extracted.inc"
    generated.write_text("\n".join(pieces), encoding="utf-8")
    # Keep a precise list of SDK bytes executed by this host test.
    (output / "extracted.json").write_text(json.dumps({
        "kind": "host stubs; not full SDK runtime or RF validation",
        "manifest_sha256": module.sha((project / ".mesh-sdk/manifest.json").read_bytes()),
        "extracted_sha256": module.sha(generated.read_bytes()),
        "functions": len(pieces) - 1,
    }, indent=2) + "\n", encoding="utf-8")
    compiler = [args.compiler] + (["cc"] if Path(args.compiler).stem.lower() == "zig" else [])
    executable = output / ("sdk_tracked.exe" if sys.platform == "win32" else "sdk_tracked")
    subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                              "-Wno-unused-function",
                              "-Wno-sign-compare", "-I", str(output), str(root / "test/sdk_tracked_harness.c"),
                              "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    settings = (component / "core/storage/settings.c").read_text(encoding="utf-8")
    net = (component / "core/net.c").read_text(encoding="utf-8")
    sequence = output / "sdk_sequence_extracted.inc"
    sequence.write_text("\n".join([
        function(settings, "static int seq_set("),
        function(settings, "static void store_pending_seq("),
        function(settings, "void bt_mesh_store_seq("),
        function(net, "uint32_t bt_mesh_next_seq(")]), encoding="utf-8")
    evidence = json.loads((output / "extracted.json").read_text(encoding="utf-8"))
    evidence["sequence_extracted_sha256"] = module.sha(sequence.read_bytes())
    evidence["sequence_functions"] = 4
    (output / "extracted.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    sequence_exe = output / ("sdk_sequence.exe" if sys.platform == "win32" else "sdk_sequence")
    subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(output),
                              str(root / "test/sdk_sequence_harness.c"), "-o", str(sequence_exe)], check=True)
    subprocess.run([str(sequence_exe)], check=True)
    with tempfile.TemporaryDirectory(prefix="pinning-", dir=output) as temp:
        temp = Path(temp)
        pin_project = temp / "project"
        pin_project.mkdir()
        module.prepare(args.framework, pin_project)
        module.prepare(args.framework, pin_project)  # Unchanged cached component works.
        target = pin_project / ".mesh-sdk/bt"
        added = target / "unreviewed.c"
        added.write_text("unreviewed", encoding="utf-8")
        rejected(lambda: module.prepare(args.framework, pin_project), "added cache file")
        added.unlink()
        probe = target / "CMakeLists.txt"
        original = probe.read_bytes()
        probe.write_bytes(original + b"\n# tampered\n")
        rejected(lambda: module.prepare(args.framework, pin_project), "changed cache file")
        probe.unlink()
        rejected(lambda: module.prepare(args.framework, pin_project), "missing cache file")
        probe.write_bytes(original)
        module.prepare(args.framework, pin_project)
        # Modify an isolated SDK copy, never the global PlatformIO package.
        framework = temp / "framework"
        shutil.copytree(args.framework / "components/bt", framework / "components/bt")
        for name in ("package.json", "version.txt"):
            shutil.copy2(args.framework / name, framework / name)
        module.prepare(framework, pin_project)
        probe = framework / "components/bt/CMakeLists.txt"
        original = probe.read_bytes()
        probe.write_bytes(original + b"\n# tampered\n")
        rejected(lambda: module.prepare(framework, pin_project), "changed baseline file outside patch inputs")
        probe.write_bytes(original)
        (framework / "components/bt/unreviewed.c").write_text("unreviewed", encoding="utf-8")
        rejected(lambda: module.prepare(framework, pin_project), "added baseline file")
    print("PASS SDK host harness and complete component pinning (no device/RF tests)", flush=True)


if __name__ == "__main__":
    main()
