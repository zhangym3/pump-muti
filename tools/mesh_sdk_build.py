"""PlatformIO pre-build hook; the root CMake file selects our bt component."""
Import("env")
import importlib.util
from pathlib import Path

root = Path(env["PROJECT_DIR"])
spec = importlib.util.spec_from_file_location("prepare_mesh_sdk", root / "tools/prepare_mesh_sdk.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
module.prepare(env.PioPlatform().get_package_dir("framework-espidf"), root)

import hashlib, json
inputs = {}
inputs["@environment"] = env["PIOENV"]
for folder in ('include', 'src', 'bootloader_components', 'tools/mesh_sdk'):
    for file in sorted((root / folder).rglob('*')):
        if file.is_file(): inputs[file.relative_to(root).as_posix()] = hashlib.sha256(file.read_bytes()).hexdigest()

# PlatformIO 6.10 may reuse the bootloader CMake model when new project
# components are added. Invalidate only its generated JSON replies when our
# bootloader inputs change, so the wrapper is actually discovered and linked.
boot_inputs = {name: sha for name, sha in inputs.items() if name.startswith('bootloader_components/')}
boot_signature = hashlib.sha256(json.dumps(boot_inputs, sort_keys=True).encode()).hexdigest()
boot_stamp = root / '.mesh-sdk' / ('bootloader-cmake-' + env['PIOENV'] + '.sha256')
if not boot_stamp.is_file() or boot_stamp.read_text().strip() != boot_signature:
    boot_reply = Path(env.subst('$BUILD_DIR')) / 'bootloader/.cmake/api/v1/reply'
    for reply in boot_reply.glob('*.json'):
        reply.unlink()
    boot_stamp.write_text(boot_signature + '\n')

for name in ('platformio.ini', 'CMakeLists.txt', 'tools/prepare_mesh_sdk.py', 'tools/mesh_sdk_build.py', 'partitions.csv'):
    inputs[name] = hashlib.sha256((root/name).read_bytes()).hexdigest()
for file in sorted(root.glob('sdkconfig*defaults')):
    inputs[file.name] = hashlib.sha256(file.read_bytes()).hexdigest()
identity = hashlib.sha256(json.dumps(inputs,sort_keys=True).encode()).hexdigest()
env.Append(CPPDEFINES=[('PUMP_BUILD_ID', '0x'+identity[:8]+'U')])
(root/'.mesh-sdk/build.json').write_text(json.dumps({'build_id': identity[:8], 'source_sha256':identity, 'inputs':inputs},indent=2)+'\n')
