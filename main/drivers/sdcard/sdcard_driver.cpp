/*
 * SDCardDriver — manages SD card lifecycle via SDSPI.
 *
 * Extracted from PeripheralManager for independent module ownership.
 * Reference-counted init/deinit, LDO power-cycle for clean state.
 */

#include "sdcard_driver.hpp"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "example_config.h"

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
    _lifecycle_mutex(nullptr),
    _card(nullptr),
    _refcount(0),
    _ldo_chan(nullptr)
{
    _lifecycle_mutex = xSemaphoreCreateMutex();
}

SDCardDriver::~SDCardDriver()
{
    if (_lifecycle_mutex) {
        vSemaphoreDelete(_lifecycle_mutex);
        _lifecycle_mutex = nullptr;
    }
}

/*============================================================================
 * SD Card (SPI mode — SDMMC slot 0 blocked by esp_hosted C6 WiFi on slot 1)
 *============================================================================*/
bool SDCardDriver::init(void)
{
    if (!_lifecycle_mutex) return false;
    xSemaphoreTake(_lifecycle_mutex, portMAX_DELAY);

    if (_refcount > 0) {
        _refcount++;
        ESP_LOGI(TAG, "SD card already initialized (refcount=%d)", _refcount);
        xSemaphoreGive(_lifecycle_mutex);
        return true;
    }

    esp_err_t ret;
    ESP_LOGI(TAG, "Initializing SD card via SPI...");

    /* Power-cycle SD via LDO VO4 to reset card into clean state */
    {
        esp_ldo_channel_config_t ldo_cfg = { .chan_id = 4, .voltage_mv = 3300 };
        ret = esp_ldo_acquire_channel(&ldo_cfg, &_ldo_chan);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SD LDO acquire failed (%s)", esp_err_to_name(ret));
            _ldo_chan = nullptr;
            xSemaphoreGive(_lifecycle_mutex);
            return false;
        }
        /* Release → re-acquire toggles power, resetting SD card */
        esp_ldo_release_channel(_ldo_chan);
        _ldo_chan = nullptr;
        vTaskDelay(pdMS_TO_TICKS(50));
        ret = esp_ldo_acquire_channel(&ldo_cfg, &_ldo_chan);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SD LDO re-acquire failed (%s)", esp_err_to_name(ret));
            _ldo_chan = nullptr;
            xSemaphoreGive(_lifecycle_mutex);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    const char mount_point[] = SDMMC_MOUNT_POINT;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs   = GPIO_NUM_42;  /* D3 → CS */
    slot_config.host_id   = SPI2_HOST;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = GPIO_NUM_44,  /* CMD → MOSI (DI) */
        .miso_io_num = GPIO_NUM_39,  /* D0  → MISO (DO) */
        .sclk_io_num = GPIO_NUM_43,  /* CLK → SCLK */
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPI bus init failed (%s)", esp_err_to_name(ret));
        esp_ldo_release_channel(_ldo_chan);
        _ldo_chan = nullptr;
        xSemaphoreGive(_lifecycle_mutex);
        return false;
    }

    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s), continuing without SD", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        esp_ldo_release_channel(_ldo_chan);
        _ldo_chan = nullptr;
        xSemaphoreGive(_lifecycle_mutex);
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", mount_point);
    sdmmc_card_print_info(stdout, _card);
    _refcount = 1;
    xSemaphoreGive(_lifecycle_mutex);
    return true;
}

void SDCardDriver::deinit(void)
{
    if (!_lifecycle_mutex) return;
    xSemaphoreTake(_lifecycle_mutex, portMAX_DELAY);

    if (_refcount <= 0) {
        ESP_LOGW(TAG, "SD card refcount already 0, skipping deinit");
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }
    _refcount--;
    if (_refcount > 0) {
        ESP_LOGI(TAG, "SD card still in use (refcount=%d), skipping deinit", _refcount);
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }

    ESP_LOGI(TAG, "Deinitializing SD card...");
    if (_card) {
        esp_vfs_fat_sdcard_unmount(SDMMC_MOUNT_POINT, _card);
        _card = nullptr;
    }
    spi_bus_free(SPI2_HOST);
    if (_ldo_chan) {
        esp_ldo_release_channel(_ldo_chan);
        _ldo_chan = nullptr;
    }
    ESP_LOGI(TAG, "SD card deinitialized");
    xSemaphoreGive(_lifecycle_mutex);
}
