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


def firmware(root, env, chip, nm, evidence_label):
    build = json.loads((root / ".mesh-sdk/build.json").read_text(encoding="utf-8"))
    for name, digest in build["inputs"].items():
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
    require(re.fullmatch(r"[a-z0-9-]+", label), "Invalid evidence label")
    for report in args.report:
        require(report.is_file(), "Missing report: " + str(report))
    require(not target.exists() and not target.with_suffix(".zip").exists(), "Output already exists")
    for root in (master, slave, app):
        require(not target.is_relative_to(root / "src") and target != root, "Invalid output path")
    tools = args.platformio_home / "packages"
    info = {
        "master": firmware(master, "pump_master", "esp32", tools / "toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf-nm.exe", label),
        "slave": firmware(slave, "valve_slave_c3", "esp32c3", tools / "toolchain-riscv32-esp/bin/riscv32-esp-elf-nm.exe", label),
    }
    approved = (master / "include/mesh_deployment.h").read_text(encoding="utf-8")
    require("0x" + info["slave"]["build_id"] + "U" in approved, "Slave build is not approved by master")
    board = (master / "include/board_profile.h").read_text(encoding="utf-8")
    require(re.search(r"MaxConcurrent\s*=\s*1\s*;", board), "Production concurrency must stay 1")
    expectations = {
        master / f".pio/host-{label}.log": ["28 test groups passed", "PASS governor", "PASS simulated 8h/5 nodes"] +
            [f"PASS {n} nodes x 1000 batches" for n in (1, 2, 5)],
        master / f".pio/sdk-{label}.log": ["PASS SDK host harness and complete component pinning"],
        master / f".pio/log-tools-{label}.log": ["Ran 7 tests", "OK"],
        slave / f".pio/host-{label}.log": ["slave flow tests passed"],
        app / "implementation-tests.log": ["BUILD SUCCESSFUL"],
    }
    if label == "offline-fix":
        expectations[master / f".pio/host-{label}.log"] += ["PASS recovery:", "PASS integrated fault recovery:"]
        expectations[slave / f".pio/host-{label}.log"] += ["PASS transport fault locks"]
    for path, markers in expectations.items():
        log = path.read_text(encoding="utf-8")
        require(all(marker in log for marker in markers), "Missing test evidence: " + str(path))
    xml_files = sorted((app / "app/build/test-results/testDebugUnitTest").glob("TEST-*.xml"))
    tests = failures = errors = skipped = 0
    for path in xml_files:
        suite = ET.parse(path).getroot()
        tests += int(suite.get("tests", 0))
        failures += int(suite.get("failures", 0))
        errors += int(suite.get("errors", 0))
        skipped += int(suite.get("skipped", 0))
    require(tests >= 78 and failures == errors == skipped == 0, "APP test result mismatch")
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
        require(sha(app / "implementation-tests.log") == sha(reference / "app/build-tests.log"), "Reused APP test log changed")
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
        for name in ("firmware.bin", "firmware.elf", "bootloader.bin", "partitions.bin"):
            copy(folder / name, role + "/" + name)
        for source, name in ((root / ".mesh-sdk/build.json", "build.json"),
                             (root / ".mesh-sdk/manifest.json", "sdk-manifest.json"),
                             (root / ("sdkconfig." + info[role]["environment"]), "sdkconfig.effective"),
                             (folder / "config/sdkconfig.json", "sdkconfig.json"),
                             (root / f".pio/compile_commands-{label}.json", "compile_commands.json"),
                             (root / f".pio/envdump-{label}.log", "envdump.log"),
                             (root / f".pio/build-{label}.log", "build.log"),
                             (root / f".pio/host-{label}.log", "host-tests.log")):
            copy(source, role + "/" + name)
    for name in ("sdk", "log-tools"):
        copy(master / (".pio/" + name + "-" + label + ".log"), "evidence/" + name + "-tests.log")
    copy(master / ".pio/sdk-tests/extracted.json", "evidence/sdk-extracted.json")
    copy(app / "implementation-tests.log", "app/build-tests.log")
    copy(app_apk, "app/app-debug.apk")
    copy(app_metadata, "app/output-metadata.json")
    for path in xml_files:
        copy(path, "app/test-results/" + path.name)
    for path in (master / "docs").glob("MESH_*ACCEPTANCE*20261019.*"):
        copy(path, "docs/" + path.name)
    for path in (master / "docs").glob("MESH_FIELD_*20261019.*"):
        copy(path, "docs/" + path.name)
    for name in ("MESH_FIVE_CHANNEL_IMPLEMENTATION_PLAN_20261019.md",):
        copy(master / "docs" / name, "docs/" + name)
    for report in args.report:
        copy(report, "docs/" + report.name)
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
        "devices_opened": False, "firmware_uploaded": False, "pump_started": False,
        "software_evidence": "PASS; see test logs and acceptance report",
        "firmware_evidence_label": label,
        "current_reports": [p.name for p in args.report],
        "pending": ["device SDK callback/fault matrix", "1/2/5-node RF", "1/2/5-node 20 batches each of 500mL",
                    "8-hour wall-clock bench", "agreed physical-volume tolerance", "runtime heap/stack peaks"],
        "builds": info,
        "identity_scope": "build_id indexes source/default configuration inputs; full ELF/bin and effective config hashes identify this artifact",
        "flash_note": "Use flash_files here. IDF native flash_args/flasher_args.json are NOT the PlatformIO upload layout.",
        "rollback_note": "Master NVS format 3; old firmware may not read it. Preserve fences/replay records; never blindly erase NVS.",
    }
    (target / "candidate.json").write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    text = """Mesh 软件候选包（硬件未验收）

主机生产并发保持 1；本包没有刷写设备或启泵。先阅读 docs/ 下实施报告和现场验收步骤。
master/firmware.bin 与 slave/firmware.bin 均位于 0x20000。
主机 bootloader.bin 位于 0x1000，从机位于 0x0；两端 partitions.bin 均位于 0x8000。
精确芯片/地址/SHA256 见 candidate.json，SHA256SUMS 覆盖包内文件。
不要使用 IDF 辅助 flash_args 中的 0x10000；不要直接使用主机默认 COM3。
APK 为 debug 候选构建，未安装到手机、未验收真机重连/重启 UI。
source/ 为三仓当前构建相关源码快照，未包含 Git 历史或全局 SDK；SDK 补丁和1893文件清单在包内。
source/master、source/slave 对应原 pump-muti、pump-muti-slave；运行跨仓host测试时恢复这两个相邻目录名。
主机 NVS 已升级 format 3，回退前必须制定状态迁移/恢复步骤，不能盲目清空 fence。
"""
    if label == "offline-fix":
        text += "\n本包为从机离线修复；优先阅读 docs/SLAVE_OFFLINE_FIX_20261019.md。旧实施报告记录历史版本。\n"
        text += "故障后仅恢复诊断通信，业务保持锁存停机；缺失完成回调禁止恢复发送。新业务须先受控重启并核对状态。\n"
        text += "本机最近识别端口：主机 COM5、从机 COM6；刷写前核对物理身份，保留 NVS。hello 可查询本包 build ID。\n"
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
