#include "bootloader_flash.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_spiflash.h"

esp_err_t __real_bootloader_flash_xmc_startup(void);

esp_err_t IRAM_ATTR __wrap_bootloader_flash_xmc_startup(void)
{
    /* XM25QH128A (16 MiB) responds normally to RDID. IDF 5.4's strict XMC
     * list omits its 0x70 family, incorrectly sending it through the C-series
     * power-down recovery and rejecting it afterwards. Like the SDK's known
     * readable IDs, this exact model can skip that recovery sequence.
     * Require a fresh matching read; leave all other cases to the SDK.
     */
    const uint32_t xm25qh128a_id = 0x207018;
    if (g_rom_flashchip.device_id == xm25qh128a_id &&
            bootloader_read_flash_id() == xm25qh128a_id) {
        ESP_EARLY_LOGI("pump_flash", "XM25QH128A RDID 0x207018 verified; normal startup");
        return ESP_OK;
    }
    return __real_bootloader_flash_xmc_startup();
}
