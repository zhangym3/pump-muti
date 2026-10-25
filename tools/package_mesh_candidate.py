"""Verify and package the current software candidate. Never opens a device port.

Requires completed builds/tests and envdump logs from both firmware projects.
The package records pending hardware gates; packaging cannot approve deployment.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import xml.etree.ElementTree as ET
import zipfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(root, *args):
    if args[0] == "git":
        args = ("git", "-c", "safe.directory=" + root.as_posix(), *args[1:])
    return subprocess.check_output(list(args), cwd=root, encoding="utf-8").strip()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def partition_table(path):
    data = path.read_bytes()
    rows = []
    for at in range(0, len(data), 32):
        entry = data[at:at + 32]
        magic, = struct.unpack_from("<H", entry)
        if magic in (0xEBEB, 0xFFFF):  # MD5 record or end marker
            break
        require(magic == 0x50AA, "Invalid binary partition table: " + str(path))
        _, kind, subtype, offset, size, label, flags = struct.unpack("<HBBII16sI", entry)
        rows.append(dict(name=label.split(b"\0")[0].decode(), type=kind, subtype=subtype,
                         offset=hex(offset), size=size, flags=flags))
    return rows


def firmware(root, env, chip, nm, evidence_label, max_concurrent=None):
    build = json.loads((root / ".mesh-sdk/build.json").read_text(encoding="utf-8"))
    for name, digest in build["inputs"].items():
        if name == "@environment":
            require(digest == env, "Build environment identity mismatch")
            continue
        require(sha(root / name) == digest, "Source changed since build: " + str(root / name))
    identity = hashlib.sha256(json.dumps(build["inputs"], sort_keys=True).encode()).hexdigest()
    require(build["source_sha256"] == identity and build["build_id"] == identity[:8],
            "Invalid build identity: " + str(root))
    manifest = json.loads((root / ".mesh-sdk/manifest.json").read_text(encoding="utf-8"))
    actual_files = {p.relative_to(root / ".mesh-sdk/bt").as_posix()
                    for p in (root / ".mesh-sdk/bt").rglob("*") if p.is_file()}
    require(actual_files == set(manifest["outputs"]), "SDK component file set changed")
    for name, digest in manifest["outputs"].items():
        require(sha(root / ".mesh-sdk/bt" / name) == digest, "SDK cache changed: " + name)
    folder = root / ".pio/build" / env
    config = (folder / "config/sdkconfig.h").read_text(encoding="utf-8")
    require(f'#define CONFIG_IDF_TARGET "{chip}"' in config, "Build target mismatch: " + str(root))
    require((folder / "firmware.bin").read_bytes()[0xB0:0xD0] ==
            hashlib.sha256((folder / "firmware.elf").read_bytes()).digest(),
            "Firmware binary does not match ELF")
    symbols = run(root, str(nm), str(folder / "firmware.elf"))
    required = ("esp_ble_mesh_tracked_send_event", "esp_ble_mesh_server_model_send_msg_tracked")
    for symbol in required:
        require(re.search(r"\bT " + symbol + r"$", symbols, re.M), "Missing strong symbol " + symbol)
    # IDF's auxiliary compile database lacks SCons-added flags. Use PlatformIO's
    # compiledb target, captured separately, which includes PUMP_BUILD_ID.
    commands = json.loads((root / f".pio/compile_commands-{evidence_label}.json").read_text(encoding="utf-8"))
    require(any("PUMP_BUILD_ID=0x" + identity[:8] + "U" in row.get("command", "") for row in commands),
            "Compile command build identity mismatch")
    if max_concurrent is not None:
        app_commands = [row.get("command", "") for row in commands
                        if Path(row["file"]).name == "application.cpp"]
        require(app_commands, "Missing application compile command")
        for command in app_commands:
            defines = re.findall(r"-DPUMP_MAX_CONCURRENT=(\d+)", command)
            require((defines == [str(max_concurrent)]) if max_concurrent != 1 else
                    (not defines or defines == ["1"]), "Effective concurrency mismatch")
    dump = (root / f".pio/envdump-{evidence_label}.log").read_text(encoding="utf-8")
    require("[SUCCESS]" in dump, "envdump did not finish")
    app_offset = re.search(r"'ESP32_APP_OFFSET': '(0x[0-9a-fA-F]+)'", dump).group(1)
    boot_offset = "0x1000" if chip == "esp32" else "0x0"
    for offset, filename in ((boot_offset, "bootloader.bin"), ("0x8000", "partitions.bin")):
        require(re.search(re.escape("('" + offset + "', '$BUILD_DIR") +
                          r"[\\/]+" + re.escape(filename) + r"'\)", dump),
                "Unexpected PlatformIO extra image: " + filename)
    table = partition_table(folder / "partitions.bin")
    app_partition = next(row for row in table if row["name"] == "factory" and row["type"] == 0)
    require(int(app_offset, 16) == int(app_partition["offset"], 16) == 0x20000,
            "Application upload address differs from binary partition table")
    require((folder / "firmware.bin").stat().st_size <= app_partition["size"], "App exceeds partition")
    log = (root / f".pio/build-{evidence_label}.log").read_text(encoding="utf-8")
    require("[SUCCESS]" in log, "Firmware build has no success evidence")
    sizes = {name: int(re.search(name + r":.*used (\d+) bytes", log).group(1))
             for name in ("RAM", "Flash")}
    return {"build_id": identity[:8], "source_sha256": identity, "environment": env, "chip": chip,
            "sdk_patch_identity": manifest["patch_sha256"], "static_size_bytes": sizes,
            "strong_symbols": list(required), "partitions": table,
            "flash_files": {boot_offset: "bootloader.bin", "0x8000": "partitions.bin", app_offset: "firmware.bin"},
            "firmware_sha256": sha(folder / "firmware.bin"), "elf_sha256": sha(folder / "firmware.elf")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--master", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--slave", type=Path, required=True)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--platformio-home", type=Path, default=Path.home() / ".platformio")
    parser.add_argument("--output", type=Path, required=True, help="New directory, must not exist")
    parser.add_argument("--evidence-label", default="implementation", help="Firmware build/test evidence filename suffix")
    parser.add_argument("--master-env", choices=("pump_master", "pump_master_five_candidate"), default="pump_master")
    parser.add_argument("--master-evidence-label", help="Master build/envdump/compiledb suffix; tests use --evidence-label")
    parser.add_argument("--app-test-log", type=Path, help="Log from the current APP test run")
    parser.add_argument("--report", type=Path, action="append", default=[], help="Additional report to include")
    parser.add_argument("--app-evidence-reference", type=Path, help="Prior candidate whose unchanged APP evidence is reused")
    args = parser.parse_args()
    master, slave, app, target = [p.resolve() for p in (args.master, args.slave, args.app, args.output)]
    reference = args.app_evidence_reference.resolve() if args.app_evidence_reference else None
    # A reusable APK belongs to the archived candidate, not the mutable Gradle
    # output directory (which may have been cleaned by unrelated UI checks).
    app_apk = reference / "app/app-debug.apk" if reference else app / "app/build/outputs/apk/debug/app-debug.apk"
    app_metadata = reference / "app/output-metadata.json" if reference else app / "app/build/outputs/apk/debug/output-metadata.json"
    label = args.evidence_label
    master_label = args.master_evidence_label or label
    app_test_log = args.app_test_log.resolve() if args.app_test_log else app / "implementation-tests.log"
    candidate_concurrency = 5 if args.master_env == "pump_master_five_candidate" else 1
    require(re.fullmatch(r"[a-z0-9-]+", label), "Invalid evidence label")
    require(re.fullmatch(r"[a-z0-9-]+", master_label), "Invalid master evidence label")
    for report in args.report:
        require(report.is_file(), "Missing report: " + str(report))
    if label == "basic-five":
        for name in ("README.md", "node-inventory.csv", "B-communication.csv", "C-tasks.csv",
                     "C-branch-measurements.csv", "D-protection-and-timing.csv"):
            require((master / "docs/basic-five-records" / name).is_file(), "Missing bench record template: " + name)
    require(not target.exists() and not target.with_suffix(".zip").exists(), "Output already exists")
    for root in (master, slave, app):
        require(not target.is_relative_to(root / "src") and target != root, "Invalid output path")
    tools = args.platformio_home / "packages"
    info = {
        "master": firmware(master, args.master_env, "esp32c3", tools / "toolchain-riscv32-esp/bin/riscv32-esp-elf-nm.exe", master_label, candidate_concurrency),
        "slave": firmware(slave, "valve_slave_c3", "esp32c3", tools / "toolchain-riscv32-esp/bin/riscv32-esp-elf-nm.exe", label),
    }
    board = (master / "include/board_profile.h").read_text(encoding="utf-8")
    require(re.search(r"#define\s+PUMP_MAX_CONCURRENT\s+1\b", board) and
            re.search(r"MaxConcurrent\s*=\s*PUMP_MAX_CONCURRENT\s*;", board),
            "Production default concurrency must stay 1")
    for name in ("protocol.h", "mesh_tx_governor.h", "mesh_tx_diagnostics.h", "maintenance_session.h", "result_commit.h"):
        require((master / "include" / name).read_bytes() == (slave / "include" / name).read_bytes(),
                "Master/slave shared header differs: " + name)
    protocol = (master / "include/protocol.h").read_text(encoding="utf-8")
    revision = re.search(r"ProtocolRevision\s*=\s*(\d+)\s*[,;]", protocol)
    features = re.search(r"RequiredFeatures\s*=\s*(0x[0-9a-fA-F]+)\s*;", protocol)
    require(revision and features, "Missing protocol capabilities")
    revision, features = int(revision.group(1)), int(features.group(1), 16)
    if label == "basic-five":
        require(revision == 6 and features == 0x15ff, "Expected basic-five V6 capabilities")
    require(info["master"]["sdk_patch_identity"] == info["slave"]["sdk_patch_identity"], "SDK patch mismatch")
    expectations = {
        master / f".pio/host-{label}.log": ["PASS governor", "PASS simulated 8h/5 nodes"] +
            [f"PASS {n} nodes x 1000 batches" for n in (1, 2, 5)],
        master / f".pio/sdk-{label}.log": ["PASS SDK host harness and complete component pinning"],
        master / f".pio/log-tools-{label}.log": ["OK"],
        slave / f".pio/host-{label}.log": ["slave flow tests passed"],
        app_test_log: ["BUILD SUCCESSFUL"],
    }
    host_log = (master / f".pio/host-{label}.log").read_text(encoding="utf-8")
    groups = re.search(r"^(\d+) test groups passed$", host_log, re.M)
    require(groups and int(groups.group(1)) >= (33 if label == "basic-five" else 30), "Missing core test groups")
    if label == "basic-five":
        expectations[master / f".pio/host-{label}.log"] += [
            "PASS durable result commit failure latch", "PASS boot-bound maintenance tokens",
            "PASS all 31 five-node masks"]
        expectations[slave / f".pio/host-{label}.log"] += [
            "PASS unsaved terminal measurement survives newer CLOSE and PREPARE",
            "PASS maintenance expiry closes held valve"]
        expectations[master / f".pio/architecture-{label}.log"] = ["role boundary: PASS"]
    if label == "v4":
        expectations[master / ".pio/host-v4.log"] += ["PASS V4 early StartOK/rejection", "1000 randomized five-node batches"]
        expectations[master / ".pio/architecture-v4.log"] = ["PASS"]
        expectations[master / ".pio/v3-field-replay.log"] = ["PASS archived V3 replay:"]
        expectations[slave / ".pio/host-v4.log"] += ["PASS prepared 19999/20000ms boundary"]
    if label == "offline-fix":
        expectations[master / f".pio/host-{label}.log"] += ["PASS recovery:", "PASS integrated fault recovery:"]
        expectations[slave / f".pio/host-{label}.log"] += ["PASS transport fault locks"]
    for path, markers in expectations.items():
        log = path.read_text(encoding="utf-8")
        require(all(marker in log for marker in markers), "Missing test evidence: " + str(path))
    log_tools = (master / f".pio/log-tools-{label}.log").read_text(encoding="utf-8")
    count = re.search(r"^Ran (\d+) tests? in ", log_tools, re.M)
    require(count and int(count.group(1)) >= 7 and re.search(r"^OK\s*$", log_tools, re.M)
            and not re.search(r"^FAILED\b", log_tools, re.M), "Log tool regressions did not pass")
    xml_files = sorted((app / "app/build/test-results/testDebugUnitTest").glob("TEST-*.xml"))
    tests = failures = errors = skipped = 0
    for path in xml_files:
        suite = ET.parse(path).getroot()
        tests += int(suite.get("tests", 0))
        failures += int(suite.get("failures", 0))
        errors += int(suite.get("errors", 0))
        skipped += int(suite.get("skipped", 0))
    require(tests >= (136 if label == "basic-five" else 78) and failures == errors == skipped == 0, "APP test result mismatch")
    info["app"] = {"variant": "debug", "tests": tests, "failures": failures, "errors": errors,
                   "apk_sha256": sha(app_apk)}
    if args.app_evidence_reference:
        prior = json.loads((reference / "candidate.json").read_text(encoding="utf-8"))
        require(prior["builds"]["app"]["apk_sha256"] == info["app"]["apk_sha256"], "Reused APP APK changed")
        app_paths = ["app/src", "app/build.gradle.kts", "build.gradle.kts", "settings.gradle.kts", "gradle.properties",
                     "gradle", "gradlew", "gradlew.bat"]
        current = {name: sha(app / name) for name in run(app, "git", "ls-files", "--cached", "--others",
                   "--exclude-standard", "--", *app_paths).splitlines() if (app / name).is_file()}
        previous = {p.relative_to(reference / "source/app").as_posix(): sha(p)
                    for p in (reference / "source/app").rglob("*") if p.is_file()}
        require(current == previous and current, "Reused APP sources/configuration changed")
        require(sha(app_test_log) == sha(reference / "app/build-tests.log"), "Reused APP test log changed")
        require({p.name: sha(p) for p in xml_files} ==
                {p.name: sha(p) for p in (reference / "app/test-results").glob("TEST-*.xml")}, "Reused APP test XML changed")
        info["app"]["evidence"] = {"reused": True, "reference": reference.name,
            "candidate_sha256": sha(reference / "candidate.json"), "sources_unchanged": True,
            "archived_apk_hash_verified": True}
    target.mkdir(parents=True)

    def copy(source, name):
        dest = target / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, dest)

    for role, root in (("master", master), ("slave", slave)):
        folder = root / ".pio/build" / info[role]["environment"]
        build_label = master_label if role == "master" else label
        for name in ("firmware.bin", "firmware.elf", "bootloader.bin", "partitions.bin"):
            copy(folder / name, role + "/" + name)
        for source, name in ((root / ".mesh-sdk/build.json", "build.json"),
                             (root / ".mesh-sdk/manifest.json", "sdk-manifest.json"),
                             (root / ("sdkconfig." + info[role]["environment"]), "sdkconfig.effective"),
                             (folder / "config/sdkconfig.json", "sdkconfig.json"),
                             (root / f".pio/compile_commands-{build_label}.json", "compile_commands.json"),
                             (root / f".pio/envdump-{build_label}.log", "envdump.log"),
                             (root / f".pio/build-{build_label}.log", "build.log"),
                             (root / f".pio/host-{label}.log", "host-tests.log")):
            copy(source, role + "/" + name)
    for name in ("sdk", "log-tools"):
        copy(master / (".pio/" + name + "-" + label + ".log"), "evidence/" + name + "-tests.log")
    copy(master / ".pio/sdk-tests/extracted.json", "evidence/sdk-extracted.json")
    copy(app_test_log, "app/build-tests.log")
    if label == "basic-five":
        copy(master / f".pio/architecture-{label}.log", "evidence/architecture-tests.log")
    if label == "v4":
        copy(master / ".pio/architecture-v4.log", "evidence/architecture-tests.log")
        copy(master / ".pio/v3-field-replay.log", "evidence/v3-field-replay.log")
        copy(master / ".pio/diagnostics/v3-field-replay.cpp", "evidence/v3-field-replay.cpp")
        baseline = master / ".pio/diagnostics/v4-implementation-baseline/pump-muti"
        for path in sorted(baseline.rglob("*")):
            if path.is_file() and path.relative_to(baseline).parts[0] in ("include", "src", "test"):
                copy(path, "evidence/v3-baseline/" + path.relative_to(baseline).as_posix())
    copy(app_apk, "app/app-debug.apk")
    copy(app_metadata, "app/output-metadata.json")
    for path in xml_files:
        copy(path, "app/test-results/" + path.name)
    if label not in ("v4", "basic-five"):
        for path in (master / "docs").glob("MESH_*ACCEPTANCE*20261019.*"):
            copy(path, "docs/" + path.name)
        for path in (master / "docs").glob("MESH_FIELD_*20261019.*"):
            copy(path, "docs/" + path.name)
        for name in ("MESH_FIVE_CHANNEL_IMPLEMENTATION_PLAN_20261019.md",):
            copy(master / "docs" / name, "docs/" + name)
    for report in args.report:
        copy(report, "docs/" + report.name)
    if label == "basic-five":
        for name in ("COMMISSIONING.md", "PROTOCOL.md", "ARCHITECTURE_REASSESSMENT_20261025.md",
                     "MESH_SAR_ROOT_CAUSE_FIX_PLAN_20261024.md"):
            copy(master / "docs" / name, "docs/" + name)
        for path in sorted((master / "docs/basic-five-records").rglob("*")):
            if path.is_file():
                copy(path, "docs/basic-five-records/" + path.relative_to(master / "docs/basic-five-records").as_posix())
        for role, root in (("master", master), ("slave", slave), ("app", app)):
            copy(root / "README.md", "docs/" + role + "-README.md")
    for role, root in (("master", master), ("slave", slave), ("app", app)):
        state = {"head": run(root, "git", "rev-parse", "HEAD"),
                 "branch": run(root, "git", "branch", "--show-current"),
                 "status": run(root, "git", "status", "--short"), "snapshot_is_dirty": True}
        info[role]["git"] = state
        # Export only build sources/configuration/tests/tools, never local.properties, caches or credentials.
        paths = ["app/src", "app/build.gradle.kts", "build.gradle.kts", "settings.gradle.kts", "gradle.properties",
                 "gradle", "gradlew", "gradlew.bat"] if role == "app" else [
                     "include", "src", "test", "tools", "platformio.ini", "CMakeLists.txt", "partitions.csv",
                     "sdkconfig.defaults", "sdkconfig.master.defaults", ".gitignore"]
        files = set(run(root, "git", "ls-files", "--cached", "--others", "--exclude-standard", "--", *paths).splitlines())
        for name in sorted(files):
            if (root / name).is_file():
                copy(root / name, "source/" + role + "/" + name)
    record = {
        "kind": "software_candidate_not_hardware_accepted",
        "created_utc": datetime.now(timezone.utc).isoformat(), "production_max_concurrent": 1,
        "candidate_max_concurrent": candidate_concurrency, "available_slave_devices": 5 if label == "basic-five" else None,
        "hardware_testing_available": False if label == "basic-five" else None,
        "protocol_revision": revision, "required_features": f"0x{features:04x}",
        "packaging_opens_devices": False, "packaging_uploads_firmware": False, "packaging_starts_pump": False,
        "software_evidence": "PASS; see test logs and acceptance report",
        "firmware_evidence_label": label, "master_evidence_label": master_label,
        "current_reports": [p.name for p in args.report],
        "pending": ["complete V4 multi-device callback/fault and seg=0 traces", "1 lane >=50 batches rotating 5 devices",
                    "2 lanes >=50 overlapping batches", "5 lanes >=200 batches, 100/200/500mL",
                    "5 lanes >=8h wall-clock water/RF bench", "each fault class >=10 per rotating device",
                    "agreed physical-volume tolerance and weighed volumes", "water-task control tick <=50ms on latest firmware",
                    "phone BLE business/reconnect and three-end result agreement", "runtime heap/stack trends"],
        "builds": info,
        "identity_scope": "build_id indexes source/default configuration inputs; full ELF/bin and effective config hashes identify this artifact",
        "flash_note": "Use flash_files here. IDF native flash_args/flasher_args.json are NOT the PlatformIO upload layout.",
        "rollback_note": "Master NVS format 3; old firmware may not read it. Preserve fences/replay records; never blindly erase NVS.",
    }
    if label == "basic-five":
        record["pending"] = [
            "A: complete Application + NVS/RTOS fault injection and actual persistence/callback ordering coverage",
            "B: five devices simultaneously online, 20 system rounds / 100 node-rounds; phone connected/disconnected each >=10 rounds; all outputs zero",
            "C: all 31 nonempty masks, 85 water tasks; each branch error <= max(10mL, 5% target)",
            "C: measured electrical stop/release latency <=50ms and final-valve-to-pump-stop timing",
            "D: phone/link loss, master/slave reboot, no-flow/stall, result persistence failures and maintenance interlock",
            "RF/SAR callback attribution, SDK idempotent configuration readback, heap/stack/queue peaks and worst control-loop interval"]
    (target / "candidate.json").write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    text = """Mesh 软件候选包（硬件未验收）

主机生产默认并发保持 1；台架五路候选环境支持并发 5，实际包配置见 candidate.json 的 candidate_max_concurrent。
本打包工具不刷写设备或启泵。先阅读 docs/ 下实施报告和现场验收步骤；仅 1 台从机，五路尚未实机验收。
master/firmware.bin 与 slave/firmware.bin 均位于 0x20000。
ESP32-C3 主机和 ESP32-C3 从机 bootloader.bin 均位于 0x0；两端 partitions.bin 均位于 0x8000。
精确芯片/地址/SHA256 见 candidate.json，SHA256SUMS 覆盖包内文件。
不要使用 IDF 辅助 flash_args 中的 0x10000；刷写前确认 PlatformIO 中主从端口并核对设备身份。
APK 为 debug 候选构建，未安装到手机、未验收真机重连/重启 UI。
source/ 为三仓当前构建相关源码快照，未包含 Git 历史或全局 SDK；SDK 补丁和完整组件文件清单在包内。
source/master、source/slave 对应原 pump-muti、pump-muti-slave；运行跨仓host测试时恢复这两个相邻目录名。
主机 NVS 已升级 format 3，回退前必须制定状态迁移/恢复步骤，不能盲目清空 fence。
"""
    if label == "basic-five":
        text = text.replace("仅 1 台从机，五路尚未实机验收。", "用户已有 5 台从机，但本轮无实机测试条件，B/C/D 均未验收。")
    if label == "offline-fix":
        text += "\n本包为从机离线修复；优先阅读 docs/SLAVE_OFFLINE_FIX_20261019.md。旧实施报告记录历史版本。\n"
        text += "故障后仅恢复诊断通信，业务保持锁存停机；缺失完成回调禁止恢复发送。新业务须先受控重启并核对状态。\n"
        text += "刷写前核对主从物理身份和端口，保留 NVS。hello 可查询本包 build ID。\n"
    if args.app_evidence_reference:
        text += "APK 取自前一候选并核对清单哈希，当前 APP 源码/配置/测试日志及 XML 与旧快照相同；本轮复用证据，未重跑 Android 测试。\n"
    (target / "README.txt").write_text(text, encoding="utf-8")
    all_files = sorted(p for p in target.rglob("*") if p.is_file())
    sums = "".join(sha(p) + "  " + p.relative_to(target).as_posix() + "\n" for p in all_files)
    (target / "SHA256SUMS").write_text(sums, encoding="utf-8")
    archive = target.with_suffix(".zip")
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED) as output:
        for path in sorted(target.rglob("*")):
            if path.is_file():
                output.write(path, target.name + "/" + path.relative_to(target).as_posix())
    archive.with_suffix(".zip.sha256").write_text(sha(archive) + "  " + archive.name + "\n", encoding="utf-8")
    print(json.dumps({"package": str(target), "archive": str(archive), "archive_sha256": sha(archive),
                      "master": info["master"]["build_id"], "slave": info["slave"]["build_id"],
                      "app_tests": tests, "hardware_accepted": False}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
