"""Exercise the real bootloader wrapper with host Flash/SDK stubs; no device IO."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", help="Path to zig, clang, or gcc")
    args = parser.parse_args()
    compiler = args.compiler or next(
        (p for name in ("zig", "clang", "gcc") if (p := shutil.which(name))), None)
    if not compiler:
        parser.error("Pass --compiler with a C compiler or Zig executable")
    root = Path(__file__).resolve().parents[1]
    source = root / "bootloader_components/pump_flash_compat/pump_flash_compat.c"
    with tempfile.TemporaryDirectory(prefix="pump-flash-compat-") as tmp:
        out = Path(tmp)
        headers = {
            "bootloader_flash.h": "#include <stdint.h>\ntypedef int esp_err_t;\n"
                "#define ESP_OK 0\nuint32_t bootloader_read_flash_id(void);\n",
            "esp_attr.h": "#define IRAM_ATTR\n",
            "esp_log.h": "#define ESP_EARLY_LOGI(...) ((void)0)\n",
            "esp_rom_spiflash.h": "#include <stdint.h>\n"
                "typedef struct { uint32_t device_id; } flash_chip_t;\n"
                "extern flash_chip_t g_rom_flashchip;\n",
        }
        for name, content in headers.items():
            (out / name).write_text(content, encoding="utf-8")
        harness = out / "harness.c"
        harness.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include "bootloader_flash.h"
#include "esp_rom_spiflash.h"

flash_chip_t g_rom_flashchip;
static uint32_t fresh_id;
static int reads, fallbacks, fallback_result;
esp_err_t __wrap_bootloader_flash_xmc_startup(void);
uint32_t bootloader_read_flash_id(void) { ++reads; return fresh_id; }
esp_err_t __real_bootloader_flash_xmc_startup(void) {
    ++fallbacks;
    return fallback_result;
}

static void check(uint32_t cached, uint32_t fresh, int sdk_result,
                  int expected_result, int expected_reads, int expected_fallbacks) {
    g_rom_flashchip.device_id = cached;
    fresh_id = fresh;
    fallback_result = sdk_result;
    reads = fallbacks = 0;
    assert(__wrap_bootloader_flash_xmc_startup() == expected_result);
    assert(reads == expected_reads);
    assert(fallbacks == expected_fallbacks);
    assert(g_rom_flashchip.device_id == cached);
}

int main(void) {
    check(0x207018, 0x207018, -1, 0, 1, 0);
    /* Unreadable/changing ID must defer to the SDK, preserving its result. */
    const uint32_t unstable[] = {0, 0xffffff, 0x207017, 0x207019, 0xef4018};
    for (unsigned i = 0; i < sizeof unstable / sizeof unstable[0]; ++i) {
        check(0x207018, unstable[i], -1, -1, 1, 1);
        check(0x207018, unstable[i], 0x123, 0x123, 1, 1);
        check(0x207018, unstable[i], 0, 0, 1, 1);
    }
    /* Other XMC models, vendors and invalid cached IDs retain SDK behavior. */
    const uint32_t others[] = {0, 0xffffff, 0x207017, 0x207019, 0x204018,
                              0x204117, 0x205016, 0xef4018, 0xc87018};
    for (unsigned i = 0; i < sizeof others / sizeof others[0]; ++i) {
        check(others[i], 0x207018, -1, -1, 0, 1);
        check(others[i], 0x207018, 0, 0, 0, 1);
    }
    puts("PASS: 34 cases; exact model, fresh ID, SDK fallback and error propagation");
    return 0;
}
''', encoding="utf-8")
        executable = out / "flash_compat_test.exe"
        command = [compiler] + (["cc"] if Path(compiler).stem.lower() == "zig" else [])
        subprocess.run(command + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                                  "-I", str(out), str(source), str(harness),
                                  "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
