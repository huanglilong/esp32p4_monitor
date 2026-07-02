/*
 * PeripheralManager — unified facade for shared peripheral lifecycle.
 *
 * Migrated from main.cpp globals. All init/deinit logic preserved,
 * with refcounting for safe shared access between app modules.
 */

#include "peripherals.hpp"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/spi_common.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es7210_adc.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "example_config.h"

/* uORB for camera mutual exclusion */
#include "uorb.h"
#include "topics.h"

extern "C" i2c_master_bus_handle_t bsp_i2c_get_handle(void);

static const char *TAG = "PeriphMgr";

/*============================================================================
 * Singleton
 *============================================================================*/
PeripheralManager& PeripheralManager::instance(void)
{
    static PeripheralManager s;
    return s;
}

PeripheralManager::PeripheralManager() :
    _has_lcd(false),
    _card(nullptr),
    _sdcard_refcount(0),
    _sdcard_ldo_chan(nullptr),
    _codec_handle(nullptr),
    _codec_mic_handle(nullptr),
    _rx_handle(nullptr),
    _tx_handle(nullptr),
    _codec_mutex(nullptr),
    _audio_refcount(0),
    _volume(EXAMPLE_VOICE_VOLUME)
{
}

/*============================================================================
 * SD Card (SPI mode — SDMMC slot 0 blocked by esp_hosted C6 WiFi on slot 1)
 *============================================================================*/
bool PeripheralManager::init_sdcard(void)
{
    if (_sdcard_refcount > 0) {
        _sdcard_refcount++;
        ESP_LOGI(TAG, "SD card already initialized (refcount=%d)", _sdcard_refcount);
        return true;
    }

    esp_err_t ret;
    ESP_LOGI(TAG, "Initializing SD card via SPI...");

    /* Power-cycle SD via LDO VO4 to reset card into clean state */
    {
        esp_ldo_channel_config_t ldo_cfg = { .chan_id = 4, .voltage_mv = 3300 };
        ret = esp_ldo_acquire_channel(&ldo_cfg, &_sdcard_ldo_chan);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SD LDO acquire failed (%s)", esp_err_to_name(ret));
            _sdcard_ldo_chan = nullptr;
            return false;
        }
        /* Release → re-acquire toggles power, resetting SD card */
        esp_ldo_release_channel(_sdcard_ldo_chan);
        _sdcard_ldo_chan = nullptr;
        vTaskDelay(pdMS_TO_TICKS(50));
        ret = esp_ldo_acquire_channel(&ldo_cfg, &_sdcard_ldo_chan);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SD LDO re-acquire failed (%s)", esp_err_to_name(ret));
            _sdcard_ldo_chan = nullptr;
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
        esp_ldo_release_channel(_sdcard_ldo_chan);
        _sdcard_ldo_chan = nullptr;
        return false;
    }

    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s), continuing without SD", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        esp_ldo_release_channel(_sdcard_ldo_chan);
        _sdcard_ldo_chan = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", mount_point);
    sdmmc_card_print_info(stdout, _card);
    _sdcard_refcount = 1;
    return true;
}

void PeripheralManager::deinit_sdcard(void)
{
    if (_sdcard_refcount <= 0) {
        ESP_LOGW(TAG, "SD card refcount already 0, skipping deinit");
        return;
    }
    _sdcard_refcount--;
    if (_sdcard_refcount > 0) {
        ESP_LOGI(TAG, "SD card still in use (refcount=%d), skipping deinit", _sdcard_refcount);
        return;
    }

    ESP_LOGI(TAG, "Deinitializing SD card...");
    if (_card) {
        esp_vfs_fat_sdcard_unmount(SDMMC_MOUNT_POINT, _card);
        _card = nullptr;
    }
    spi_bus_free(SPI2_HOST);
    if (_sdcard_ldo_chan) {
        esp_ldo_release_channel(_sdcard_ldo_chan);
        _sdcard_ldo_chan = nullptr;
    }
    ESP_LOGI(TAG, "SD card deinitialized");
}

/*============================================================================
 * Audio I2S (board-dependent codec setup)
 *============================================================================*/
void PeripheralManager::init_audio(void)
{
    if (_audio_refcount > 0) {
        _audio_refcount++;
        ESP_LOGI(TAG, "Audio already initialized (refcount=%d)", _audio_refcount);
        return;
    }

    ESP_LOGI(TAG, "Initializing audio (%s)...", _has_lcd ? "ES8311 + ES7210" : "ES8311 single-chip");

    /* Enable PA GPIO 53 (critical for speaker output) */
    gpio_config_t pa_conf = {
        .pin_bit_mask = (1ULL << 53),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_conf);
    gpio_set_level((gpio_num_t)53, 1);
    ESP_LOGI(TAG, "PA GPIO 53 enabled");

    /* I2S channel init (duplex, STD, 48kHz stereo) */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &_tx_handle, &_rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)13,
            .bclk = (gpio_num_t)12,
            .ws   = (gpio_num_t)10,
            .dout = (gpio_num_t)9,
            .din  = (gpio_num_t)11,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = EXAMPLE_AUDIO_MCLK_MULTIPLE;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(_tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(_rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(_tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(_rx_handle));

    i2c_master_bus_handle_t i2c_handle = bsp_i2c_get_handle();
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    assert(gpio_if);

    if (_has_lcd) {
        /* ===== LCD-4B: ES8311 DAC (Speaker, 0x30) + ES7210 ADC (Mic, 0x80) ===== */
        audio_codec_i2s_cfg_t i2s_out_cfg = {
            .port = 0,
            .rx_handle = NULL,
            .tx_handle = _tx_handle,
        };
        const audio_codec_data_if_t *data_out = audio_codec_new_i2s_data(&i2s_out_cfg);
        assert(data_out);

        audio_codec_i2s_cfg_t i2s_in_cfg = {
            .port = 0,
            .rx_handle = _rx_handle,
            .tx_handle = NULL,
        };
        const audio_codec_data_if_t *data_in = audio_codec_new_i2s_data(&i2s_in_cfg);
        assert(data_in);

        audio_codec_i2c_cfg_t i2c_dac = { .port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_handle };
        const audio_codec_ctrl_if_t *ctrl_dac = audio_codec_new_i2c_ctrl(&i2c_dac);
        es8311_codec_cfg_t es8311_cfg = {
            .ctrl_if = ctrl_dac, .gpio_if = gpio_if,
            .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
            .pa_pin = 53, .master_mode = false, .use_mclk = true,
            .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
            .mclk_div = I2S_MCLK_MULTIPLE_256,
        };
        esp_codec_dev_cfg_t dev_dac = {
            .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es8311_codec_new(&es8311_cfg), .data_if = data_out
        };
        _codec_handle = esp_codec_dev_new(&dev_dac);
        assert(_codec_handle);

        audio_codec_i2c_cfg_t i2c_adc = { .port = 0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = i2c_handle };
        const audio_codec_ctrl_if_t *ctrl_adc = audio_codec_new_i2c_ctrl(&i2c_adc);
        es7210_codec_cfg_t es7210_cfg = {
            .ctrl_if = ctrl_adc, .master_mode = false,
            .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
            .mclk_src = ES7210_MCLK_FROM_PAD, .mclk_div = I2S_MCLK_MULTIPLE_256,
        };
        esp_codec_dev_cfg_t dev_adc = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = es7210_codec_new(&es7210_cfg), .data_if = data_in
        };
        _codec_mic_handle = esp_codec_dev_new(&dev_adc);
        assert(_codec_mic_handle);
    } else {
        /* ===== WIFI6: ES8311 single-chip (0x30, ADC + DAC) + NS4150B amp ===== */
        audio_codec_i2s_cfg_t i2s_data_cfg = {
            .port = 0,
            .rx_handle = _rx_handle,
            .tx_handle = _tx_handle,
        };
        const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
        assert(data_if);

        audio_codec_i2c_cfg_t i2c_es8311 = { .port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_handle };
        const audio_codec_ctrl_if_t *ctrl_es8311 = audio_codec_new_i2c_ctrl(&i2c_es8311);
        es8311_codec_cfg_t es8311_cfg = {
            .ctrl_if = ctrl_es8311, .gpio_if = gpio_if,
            .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
            .pa_pin = 53, .master_mode = false, .use_mclk = true,
            .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
            .mclk_div = I2S_MCLK_MULTIPLE_256,
        };
        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
            .codec_if = es8311_codec_new(&es8311_cfg),
            .data_if = data_if,
        };
        _codec_handle = esp_codec_dev_new(&dev_cfg);
        assert(_codec_handle);
        /* _codec_mic_handle stays NULL */
    }

    /* Create mutex BEFORE opening codecs */
    _codec_mutex = xSemaphoreCreateMutex();

    /* Open codecs */
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 2, .channel_mask = 0x03, .sample_rate = EXAMPLE_AUDIO_SAMPLE_RATE };
    if (_has_lcd) {
        ESP_ERROR_CHECK(esp_codec_dev_open(_codec_handle, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
        esp_codec_dev_set_out_vol(_codec_handle, _volume);
        ESP_ERROR_CHECK(esp_codec_dev_open(_codec_mic_handle, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
        esp_codec_dev_set_in_gain(_codec_mic_handle, 42);
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_open(_codec_handle, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
        esp_codec_dev_set_out_vol(_codec_handle, _volume);
        esp_codec_dev_set_in_gain(_codec_handle, 24);
    }

    ESP_LOGI(TAG, "Audio initialized: %s, vol=%d", _has_lcd ? "ES8311 + ES7210" : "ES8311 (single-chip)", _volume);
    _audio_refcount = 1;
}

void PeripheralManager::deinit_audio(void)
{
    if (_audio_refcount <= 0) {
        ESP_LOGW(TAG, "Audio refcount already 0, skipping deinit");
        return;
    }
    _audio_refcount--;
    if (_audio_refcount > 0) {
        ESP_LOGI(TAG, "Audio still in use (refcount=%d), skipping deinit", _audio_refcount);
        return;
    }

    ESP_LOGI(TAG, "Deinitializing audio...");

    if (_codec_handle) {
        esp_codec_dev_close(_codec_handle);
        esp_codec_dev_delete(_codec_handle);
        _codec_handle = nullptr;
    }
    if (_codec_mic_handle) {
        esp_codec_dev_close(_codec_mic_handle);
        esp_codec_dev_delete(_codec_mic_handle);
        _codec_mic_handle = nullptr;
    }

    if (_tx_handle) {
        i2s_del_channel(_tx_handle);
        _tx_handle = nullptr;
    }
    if (_rx_handle) {
        i2s_del_channel(_rx_handle);
        _rx_handle = nullptr;
    }

    gpio_set_level((gpio_num_t)53, 0);

    if (_codec_mutex) {
        vSemaphoreDelete(_codec_mutex);
        _codec_mutex = nullptr;
    }

    ESP_LOGI(TAG, "Audio deinitialized");
}

/*============================================================================
 * Thread-safe codec operations
 *============================================================================*/
void PeripheralManager::set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    _volume = volume;

    if (_codec_handle && _codec_mutex &&
        xSemaphoreTake(_codec_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        esp_codec_dev_set_out_vol(_codec_handle, volume);
        xSemaphoreGive(_codec_mutex);
    }
}

void PeripheralManager::set_mic_gain(int gain_db)
{
    if (_codec_mic_handle && _codec_mutex &&
        xSemaphoreTake(_codec_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        esp_codec_dev_set_in_gain(_codec_mic_handle, gain_db);
        xSemaphoreGive(_codec_mutex);
    }
}

int PeripheralManager::codec_write(const uint8_t *data, int size)
{
    if (!_codec_handle || !_codec_mutex || size <= 0) return -1;
    if (xSemaphoreTake(_codec_mutex, pdMS_TO_TICKS(50)) != pdPASS) return -1;
    int ret = esp_codec_dev_write(_codec_handle, (void *)data, size);
    xSemaphoreGive(_codec_mutex);
    return (ret == ESP_CODEC_DEV_OK) ? size : -1;
}

/*============================================================================
 * Camera mutual exclusion via uORB
 *============================================================================*/
bool PeripheralManager::camera_available(void) const
{
    /* Check if any module has published camera_state.running = true */
    static orb_sub_t sub = ORB_ADVERT_INVALID;
    if (sub < 0) {
        sub = orb_subscribe(ORB_ID(camera_state));
    }
    if (sub >= 0) {
        bool updated = false;
        if (orb_check(sub, &updated) == 0 && updated) {
            struct camera_state_s cs = {};
            orb_copy(ORB_ID(camera_state), sub, &cs);
            if (cs.running) return false;
        }
    }
    return true;
}
