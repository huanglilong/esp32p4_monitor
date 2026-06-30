#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/sdmmc_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es7210_adc.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_brookesia.hpp"
#include "private/esp_brookesia_utils.h"
#include "phone_app_camera.hpp"
#include "phone_app_audio.hpp"
#include "phone_app_music.hpp"
#include "phone_app_settings.hpp"
#include "phone_app_camera_stream.hpp"
#include "example_config.h"

static const char *TAG = "monitor";

/* Forward declarations */
static void monitor_init_display(lv_display_t **disp);
static void monitor_init_brookesia(lv_display_t *disp);
bool monitor_init_sdcard(void);
void monitor_deinit_sdcard(void);
void monitor_init_audio(void);
void monitor_deinit_audio(void);
static void on_clock_update_timer_cb(struct _lv_timer_t *t);

/* LVGL port config — pin to core 1 (core 0 reserved for detection/NPU inference) */
#define LVGL_PORT_INIT_CONFIG() \
    {                               \
        .task_priority = 4,       \
        .task_stack = 10 * 1024,  \
        .task_affinity = 1,       \
        .task_max_sleep_ms = 500, \
        .timer_period_ms = 5,     \
    }

/* Audio handles - exposed for audio app */
esp_codec_dev_handle_t s_codec_handle = NULL;     // Speaker (ES8311)
esp_codec_dev_handle_t s_codec_mic_handle = NULL; // Microphone (ES7210)
i2s_chan_handle_t s_rx_handle = NULL;             // I2S RX (for direct mic read)
i2s_chan_handle_t s_tx_handle = NULL;             // I2S TX (for music playback)
SemaphoreHandle_t s_codec_mutex = NULL;           // Protect concurrent codec access

/* SD card */
sdmmc_card_t *s_card = NULL;
static int s_sdcard_refcount = 0;

/* Audio reference counting: shared by Audio and Music apps */
static int s_audio_refcount = 0;

/*============================================================================
 * MIPI DSI Display + ESP-Brookesia
 *============================================================================*/
static void monitor_init_display(lv_display_t **disp)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
            .buff_dma = false,
#else
            .buff_dma = true,
#endif
            .buff_spiram = false,
            .sw_rotate = false,
        }
    };
    *disp = bsp_display_start_with_config(&cfg);
    assert(*disp);
    bsp_display_backlight_on();
    ESP_LOGI(TAG, "MIPI DSI display initialized (%dx%d)", BSP_LCD_H_RES, BSP_LCD_V_RES);
}

static void monitor_init_brookesia(lv_display_t *disp)
{
    bsp_display_lock(0);

    ESP_Brookesia_Phone *phone = new ESP_Brookesia_Phone(disp);
    ESP_BROOKESIA_CHECK_NULL_EXIT(phone, "Create phone failed");

    ESP_Brookesia_PhoneStylesheet_t *stylesheet = nullptr;
    if ((BSP_LCD_H_RES == 1024) && (BSP_LCD_V_RES == 600)) {
        stylesheet = new ESP_Brookesia_PhoneStylesheet_t(ESP_BROOKESIA_PHONE_1024_600_DARK_STYLESHEET());
        ESP_BROOKESIA_CHECK_NULL_EXIT(stylesheet, "Create stylesheet failed");
    } else if ((BSP_LCD_H_RES == 800) && (BSP_LCD_V_RES == 480)) {
        stylesheet = new ESP_Brookesia_PhoneStylesheet_t(ESP_BROOKESIA_PHONE_800_480_DARK_STYLESHEET());
        ESP_BROOKESIA_CHECK_NULL_EXIT(stylesheet, "Create stylesheet failed");
    } else if ((BSP_LCD_H_RES == 480) && (BSP_LCD_V_RES == 480)) {
        stylesheet = new ESP_Brookesia_PhoneStylesheet_t(ESP_BROOKESIA_PHONE_480_480_DARK_STYLESHEET());
        ESP_BROOKESIA_CHECK_NULL_EXIT(stylesheet, "Create stylesheet failed");
    } else if ((BSP_LCD_H_RES == 800) && (BSP_LCD_V_RES == 1280)) {
        stylesheet = new ESP_Brookesia_PhoneStylesheet_t(ESP_BROOKESIA_PHONE_800_1280_DARK_STYLESHEET());
        ESP_BROOKESIA_CHECK_NULL_EXIT(stylesheet, "Create stylesheet failed");
    }

    if (stylesheet != nullptr) {
        ESP_LOGI(TAG, "Using stylesheet (%s)", stylesheet->core.name);
        ESP_BROOKESIA_CHECK_FALSE_EXIT(phone->addStylesheet(stylesheet), "Add stylesheet failed");
        ESP_BROOKESIA_CHECK_FALSE_EXIT(phone->activateStylesheet(stylesheet), "Activate stylesheet failed");
        delete stylesheet;
    } else {
        ESP_LOGW(TAG, "No matching stylesheet for %dx%d, using default", BSP_LCD_H_RES, BSP_LCD_V_RES);
    }

    ESP_BROOKESIA_CHECK_FALSE_EXIT(phone->setTouchDevice(bsp_display_get_input_dev()), "Set touch device failed");
    phone->registerLvLockCallback((ESP_Brookesia_GUI_LockCallback_t)(bsp_display_lock), 0);
    phone->registerLvUnlockCallback((ESP_Brookesia_GUI_UnlockCallback_t)(bsp_display_unlock));
    ESP_BROOKESIA_CHECK_FALSE_EXIT(phone->begin(), "Begin failed");

    PhoneAppSquareline *app_squareline = PhoneAppSquareline::getInstance();
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_squareline, "Create app squareline failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_squareline) >= 0), "Install app squareline failed");

    PhoneAppCamera *app_camera = new PhoneAppCamera(false, false);
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_camera, "Create camera app failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_camera) >= 0), "Install camera app failed");

    PhoneAppAudio *app_audio = new PhoneAppAudio(false, false);
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_audio, "Create audio app failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_audio) >= 0), "Install audio app failed");

    PhoneAppMusic *app_music = new PhoneAppMusic(false, false);
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_music, "Create music app failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_music) >= 0), "Install music app failed");

    PhoneAppSettings *app_settings = new PhoneAppSettings(false, false);
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_settings, "Create settings app failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_settings) >= 0), "Install settings app failed");

    PhoneAppCameraStream *app_cam_stream = new PhoneAppCameraStream(false, false);
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_cam_stream, "Create camera stream app failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_cam_stream) >= 0), "Install camera stream app failed");

    lv_timer_create(on_clock_update_timer_cb, 1000, phone);
    bsp_display_unlock();
    ESP_LOGI(TAG, "ESP-Brookesia Phone UI initialized");
}

static void on_clock_update_timer_cb(struct _lv_timer_t *t)
{
    time_t now;
    struct tm timeinfo;
    ESP_Brookesia_Phone *phone = (ESP_Brookesia_Phone *)t->user_data;
    time(&now);
    localtime_r(&now, &timeinfo);
    ESP_BROOKESIA_CHECK_FALSE_EXIT(
        phone->getHome().getStatusBar()->setClock(timeinfo.tm_hour, timeinfo.tm_min),
        "Refresh status bar failed"
    );
}

/*============================================================================
 * SD Card (SPI mode — SDMMC slot 0 blocked by esp_hosted C6 WiFi on slot 1)
 * SPI pins: CS=GPIO42(D3), MOSI=GPIO44(CMD), SCLK=GPIO43(CLK), MISO=GPIO39(D0)
 *============================================================================*/

bool monitor_init_sdcard(void)
{
    /* Already initialized — just bump refcount */
    if (s_sdcard_refcount > 0) {
        s_sdcard_refcount++;
        ESP_LOGI(TAG, "SD card already initialized (refcount=%d)", s_sdcard_refcount);
        return true;
    }

    esp_err_t ret;
    ESP_LOGI(TAG, "Initializing SD card via SPI...");

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
        return false;
    }

    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s), continuing without SD", esp_err_to_name(ret));
        spi_bus_free(SPI2_HOST);
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", mount_point);
    sdmmc_card_print_info(stdout, s_card);
    s_sdcard_refcount = 1;
    return true;
}

void monitor_deinit_sdcard(void)
{
    if (s_sdcard_refcount <= 0) {
        ESP_LOGW(TAG, "SD card refcount already 0, skipping deinit");
        return;
    }
    s_sdcard_refcount--;
    if (s_sdcard_refcount > 0) {
        ESP_LOGI(TAG, "SD card still in use (refcount=%d), skipping deinit", s_sdcard_refcount);
        return;
    }

    ESP_LOGI(TAG, "Deinitializing SD card...");
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SDMMC_MOUNT_POINT, s_card);
        s_card = NULL;
    }
    spi_bus_free(SPI2_HOST);
    ESP_LOGI(TAG, "SD card deinitialized");
}

/*============================================================================
 * Audio I2S (ES8311 DAC + ES7210 ADC)
 *============================================================================*/

void monitor_init_audio(void)
{
    /* Already initialized — just bump refcount */
    if (s_audio_refcount > 0) {
        s_audio_refcount++;
        ESP_LOGI(TAG, "Audio already initialized (refcount=%d)", s_audio_refcount);
        return;
    }

    ESP_LOGI(TAG, "Initializing audio (ES8311 + ES7210)...");

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

    /* I2S channel init (duplex, STD, 16kHz stereo) */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle));

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
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_handle));

    /* Shared I2S data interface - SEPARATE out/in to avoid direction conflict */
    audio_codec_i2s_cfg_t i2s_out_cfg = {
        .port = 0,
        .rx_handle = NULL,
        .tx_handle = s_tx_handle,
    };
    audio_codec_i2s_cfg_t i2s_in_cfg = {
        .port = 0,
        .rx_handle = s_rx_handle,
        .tx_handle = NULL,
    };
    const audio_codec_data_if_t *data_out = audio_codec_new_i2s_data(&i2s_out_cfg);
    const audio_codec_data_if_t *data_in  = audio_codec_new_i2s_data(&i2s_in_cfg);
    assert(data_out);
    assert(data_in);

    i2c_master_bus_handle_t i2c_handle = bsp_i2c_get_handle();
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    assert(gpio_if);

    /* ===== ES8311 DAC (Speaker) ===== */
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
    s_codec_handle = esp_codec_dev_new(&dev_dac);
    assert(s_codec_handle);

    /* ===== ES7210 ADC (Mic) ===== */
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
    s_codec_mic_handle = esp_codec_dev_new(&dev_adc);
    assert(s_codec_mic_handle);

    /* Open both codecs */
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 2, .channel_mask = 0x03, .sample_rate = EXAMPLE_AUDIO_SAMPLE_RATE };
    ESP_ERROR_CHECK(esp_codec_dev_open(s_codec_handle, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
    esp_codec_dev_set_out_vol(s_codec_handle, EXAMPLE_VOICE_VOLUME);
    ESP_ERROR_CHECK(esp_codec_dev_open(s_codec_mic_handle, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
    esp_codec_dev_set_in_gain(s_codec_mic_handle, 42);  // Max gain for quiet recordings

    ESP_LOGI(TAG, "Audio initialized: ES8311 + ES7210, vol=%d", EXAMPLE_VOICE_VOLUME);
    s_codec_mutex = xSemaphoreCreateMutex();
    s_audio_refcount = 1;
}

void monitor_deinit_audio(void)
{
    if (s_audio_refcount <= 0) {
        ESP_LOGW(TAG, "Audio refcount already 0, skipping deinit");
        return;
    }
    s_audio_refcount--;
    if (s_audio_refcount > 0) {
        ESP_LOGI(TAG, "Audio still in use (refcount=%d), skipping deinit", s_audio_refcount);
        return;
    }

    ESP_LOGI(TAG, "Deinitializing audio...");

    /* Close and delete codec devices */
    if (s_codec_handle) {
        esp_codec_dev_close(s_codec_handle);
        esp_codec_dev_delete(s_codec_handle);
        s_codec_handle = NULL;
    }
    if (s_codec_mic_handle) {
        esp_codec_dev_close(s_codec_mic_handle);
        esp_codec_dev_delete(s_codec_mic_handle);
        s_codec_mic_handle = NULL;
    }

    /* Delete I2S channels (i2s_del_channel handles disable internally) */
    if (s_tx_handle) {
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
    }
    if (s_rx_handle) {
        i2s_del_channel(s_rx_handle);
        s_rx_handle = NULL;
    }

    /* Disable PA (speaker amplifier) */
    gpio_set_level((gpio_num_t)53, 0);

    if (s_codec_mutex) {
        vSemaphoreDelete(s_codec_mutex);
        s_codec_mutex = NULL;
    }

    ESP_LOGI(TAG, "Audio deinitialized");
}

/*============================================================================
 * Main
 *============================================================================*/
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-P4 Monitor Starting ===");

    /* 0. NVS init (for persistent settings) */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_LOGI(TAG, "NVS initialized");

    /* 1. MIPI DSI Display */
    lv_display_t *disp = NULL;
    monitor_init_display(&disp);

    /* Apply saved brightness from NVS (if available) */
    {
        nvs_handle_t nvs_h;
        if (nvs_open("settings", NVS_READONLY, &nvs_h) == ESP_OK) {
            int32_t brightness = 80; /* default */
            nvs_get_i32(nvs_h, "brightness", &brightness);
            if (brightness < 20) brightness = 20;
            if (brightness > 100) brightness = 100;
            bsp_display_brightness_set((int)brightness);
            ESP_LOGI(TAG, "Brightness loaded from NVS: %ld", brightness);
            nvs_close(nvs_h);
        }
    }

    /* 2. ESP-Brookesia Phone UI */
    monitor_init_brookesia(disp);

    /* 3. MIPI CSI Camera - available via Camera app on launcher */
    /* (Camera init is handled by PhoneAppCamera, not here) */

    /* 4. SDMMC SD Card & Audio — deferred to Music/Audio apps */
    ESP_LOGI(TAG, "=== Core peripherals initialized (SD/Audio deferred to apps) ===");

    /* Idle loop */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
