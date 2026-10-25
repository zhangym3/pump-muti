"""Build and run hardware-independent controller tests, without touching any device."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--compiler", help="Path to zig, clang++, or g++ (otherwise detect from PATH)")
args = parser.parse_args()
compiler = args.compiler or next((p for name in ("zig", "clang++", "g++") if (p := shutil.which(name))), None)
if not compiler:
    parser.error("Pass --compiler with a C++17 compiler or Zig executable")
root = Path(__file__).resolve().parents[1]
for name in ("protocol.h", "mesh_tx_governor.h", "mesh_tx_diagnostics.h", "maintenance_session.h", "result_commit.h"):
    if (root / "include" / name).read_bytes() != (root.parent / "pump-muti-slave/include" / name).read_bytes():
        raise RuntimeError("Master/slave shared header differs: " + name)
out = root / ".pio" / "host-tests"
out.mkdir(parents=True, exist_ok=True)
exe = out / ("core_tests.exe" if sys.platform == "win32" else "core_tests")
cmd = [compiler]
if Path(compiler).stem.lower() == "zig":
    cmd += ["c++"]
cmd += ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-O0", "-g", "-I", str(root / "include")]
cmd += [str(root / p) for p in ("test/core_tests.cpp", "src/protocol.cpp",
                               "src/pump_safety_controller.cpp", "src/master_scheduler.cpp")]
cmd += ["-o", str(exe)]
subprocess.run(cmd, cwd=root, check=True)
subprocess.run([str(exe)], cwd=root, check=True)

mesh_exe = out / ("mesh_tests.exe" if sys.platform == "win32" else "mesh_tests")
mesh_cmd = [compiler] + (["c++"] if Path(compiler).stem.lower() == "zig" else [])
mesh_cmd += ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-DPUMP_HOST_TEST", "-O2", "-I", str(root / "include"), "-I", str(root.parent / "pump-muti-slave/include")]
mesh_cmd += [str(root / p) for p in ("test/mesh_tests.cpp", "src/protocol.cpp", "src/pump_safety_controller.cpp", "src/master_scheduler.cpp")]
mesh_cmd += [str(root.parent / "pump-muti-slave/src/flow_job_controller.cpp"), "-o", str(mesh_exe)]
subprocess.run(mesh_cmd, cwd=root, check=True)
subprocess.run([str(mesh_exe)], cwd=root, check=True)
