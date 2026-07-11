/*
 * SDCardDriver — manages SD card lifecycle via SDSPI.
 *
 * Init-once, never deinit. SD card is mounted at boot and stays mounted.
 * Uses ESP-IDF sd_pwr_ctrl API for LDO4 power management.
 *
 * Extracted from PeripheralManager for independent module ownership.
 */

#include "sdcard_driver.hpp"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "example_config.h"
#include <sys/stat.h>

static const char *TAG = "SDCardDriver";

/*============================================================================
 * Singleton
 *============================================================================*/
SDCardDriver& SDCardDriver::instance(void)
{
    static SDCardDriver s;
    return s;
}

SDCardDriver::SDCardDriver() :
    _init_mutex(nullptr),
    _card(nullptr),
    _initialized(false),
    _has_lcd(false),
    _pwr_ctrl(nullptr)
{
    _init_mutex = xSemaphoreCreateMutex();
}

SDCardDriver::~SDCardDriver()
{
    if (_pwr_ctrl) {
        sd_pwr_ctrl_del_on_chip_ldo(_pwr_ctrl);
        _pwr_ctrl = nullptr;
    }
    if (_init_mutex) {
        vSemaphoreDelete(_init_mutex);
        _init_mutex = nullptr;
    }
}

/*============================================================================
 * SD Card — init-once (never deinit).
 *   LCD-4B: SDSPI via SPI2 (LDO4 pre-powered by BSP display init).
 *   WIFI6:  SDSPI via SPI2 + sd_pwr_ctrl for LDO4 power.
 *   SDMMC native mode is NOT used — host controller conflicts with C6 SDIO.
 *============================================================================*/
bool SDCardDriver::init(void)
{
    if (!_init_mutex) return false;
    xSemaphoreTake(_init_mutex, portMAX_DELAY);

    if (_initialized) {
        ESP_LOGI(TAG, "SD card already initialized");
        xSemaphoreGive(_init_mutex);
        return true;
    }

    /* Check if SD is already mounted (e.g. BSP SDMMC on LCD-4B from older code) */
    {
        struct stat st;
        if (stat(SDMMC_MOUNT_POINT, &st) == 0) {
            ESP_LOGI(TAG, "SD card already mounted at %s", SDMMC_MOUNT_POINT);
            _initialized = true;
            xSemaphoreGive(_init_mutex);
            return true;
        }
    }

    esp_err_t ret;
    ESP_LOGI(TAG, "Initializing SD card via SDSPI...");

    /* Power on SD via LDO4.  On LCD-4B the BSP display init will
     * also acquire LDO4 via esp_ldo_acquire_channel — but that API
     * does not support shared ownership.  We skip LDO4 here on
     * LCD-4B and let the display init handle it; the caller must
     * retry init_sdcard() after the display is up. */
    if (!_has_lcd.load(std::memory_order_relaxed)) {
        sd_pwr_ctrl_ldo_config_t ldo_config = { .ldo_chan_id = 4 };
        ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &_pwr_ctrl);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SD LDO power-on failed (%s)", esp_err_to_name(ret));
            _pwr_ctrl = nullptr;
            xSemaphoreGive(_init_mutex);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    } else {
        ESP_LOGI(TAG, "LCD-4B: deferring LDO4 to BSP display init");
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024
    };
    const char mount_point[] = SDMMC_MOUNT_POINT;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.pwr_ctrl_handle = _pwr_ctrl;  /* let SDMMC driver manage power */

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs   = (gpio_num_t)SD_SPI_CS_GPIO;
    slot_config.host_id   = SD_SPI_HOST;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = (gpio_num_t)SD_SPI_MOSI_GPIO,
        .miso_io_num = (gpio_num_t)SD_SPI_MISO_GPIO,
        .sclk_io_num = (gpio_num_t)SD_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPI bus init failed (%s)", esp_err_to_name(ret));
        if (_pwr_ctrl) { sd_pwr_ctrl_del_on_chip_ldo(_pwr_ctrl); _pwr_ctrl = nullptr; }
        xSemaphoreGive(_init_mutex);
        return false;
    }

    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s), continuing without SD", esp_err_to_name(ret));
        spi_bus_free(SD_SPI_HOST);
        if (_pwr_ctrl) { sd_pwr_ctrl_del_on_chip_ldo(_pwr_ctrl); _pwr_ctrl = nullptr; }
        xSemaphoreGive(_init_mutex);
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", mount_point);
    sdmmc_card_print_info(stdout, _card);
    _initialized = true;
    xSemaphoreGive(_init_mutex);
    return true;
}

void SDCardDriver::deinit(void)
{
    /* SD card is never unmounted — kept for API compatibility */
    ESP_LOGI(TAG, "SD card deinit skipped (SD stays mounted permanently)");
}
