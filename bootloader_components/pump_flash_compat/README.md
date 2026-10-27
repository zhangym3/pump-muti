# XM25QH128A boot compatibility

This bootloader-only component supports the ESP32-S3 master's observed XMC
XM25QH128A JEDEC ID `0x207018` with ESP-IDF 5.4.0. The SDK's strict XMC list
omits family `0x70`, so it incorrectly applies the C-series recovery sequence
and then rejects this readable A-series Flash.

The linker wraps `bootloader_flash_xmc_startup`. Only an exact `0x207018`
cached ID and a fresh matching RDID skip recovery, just as the SDK skips it
for its own known readable XMC IDs. Every other case calls the original SDK
function and preserves its return value. The component is enabled only for
ESP32-S3 with `CONFIG_BOOTLOADER_FLASH_XMC_SUPPORT=y`; it does not modify the
shared PlatformIO SDK or broaden acceptance to other `0x70` models.

References: ESP-IDF `components/bootloader_support/bootloader_flash/src/bootloader_flash.c`
(`is_xmc_chip_strict`, `bootloader_flash_xmc_startup`), and flashrom's
`include/flashchips.h` / `flashchips/xmc.c` (`XM25QH128A`, device `0x7018`,
manufacturer `0x20`, 16 MiB).

After building, verify the final bootloader ELF calls
`__wrap_bootloader_flash_xmc_startup` from `bootloader_init` and retains the
original implementation for fallback. Hardware validation must show the
`pump_flash` message, successful app startup, and active BLE advertising.

The project's pre-build hook includes this directory in its build identity
and invalidates PlatformIO's generated bootloader CMake replies when these
inputs change. This also discovers the component in an existing build cache.
Run `python tools/run_flash_compat_tests.py --compiler <path-to-zig-or-cc>`
for the focused host tests; they do not replace hardware validation.
