idf_build_get_property(target IDF_TARGET)

set(srcs "")
if(target STREQUAL "esp32s3" AND CONFIG_BOOTLOADER_FLASH_XMC_SUPPORT)
    list(APPEND srcs "pump_flash_compat.c")
endif()

# Dependencies must also be declared during IDF's early requirements pass,
# before sdkconfig values are available.
idf_component_register(SRCS "${srcs}"
                       PRIV_REQUIRES bootloader_support esp_rom spi_flash hal)

# Keep the SDK implementation available for every other Flash ID and for
# unstable reads. Force inclusion of the wrapper from this static library.
if(srcs)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=bootloader_flash_xmc_startup"
        "-u __wrap_bootloader_flash_xmc_startup")
endif()
