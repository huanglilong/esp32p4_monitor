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
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/isp.h"
#include "driver/sdmmc_host.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_ctlr.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_vol.h"
#include "soc/clk_tree_defs.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_brookesia.hpp"
#include "private/esp_brookesia_utils.h"
#include "example_config.h"

static const char *TAG = "monitor";

/* Forward declarations */
static void monitor_init_display(lv_display_t **disp);
static void monitor_init_brookesia(lv_display_t *disp);
static void monitor_init_camera(void);
static void monitor_init_sdcard(void);
static void monitor_init_audio(void);
static void monitor_init_audio_es8311_es7210(void);
static void on_clock_update_timer_cb(struct _lv_timer_t *t);

/* LVGL port config */
#define LVGL_PORT_INIT_CONFIG() \
    {                               \
        .task_priority = 4,       \
        .task_stack = 10 * 1024,       \
        .task_affinity = -1,      \
        .task_max_sleep_ms = 500, \
        .timer_period_ms = 5,     \
    }

/* CSI camera handles */
static esp_cam_ctlr_handle_t s_cam_handle = NULL;
static isp_proc_handle_t s_isp_proc = NULL;

/* Audio handles */
static i2s_chan_handle_t s_tx_handle = NULL;
static i2s_chan_handle_t s_rx_handle = NULL;
static esp_codec_dev_handle_t s_codec_handle = NULL;

/* SD card */
static sdmmc_card_t *s_card = NULL;

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

    PhoneAppSimpleConf *app_simple_conf = new PhoneAppSimpleConf();
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_simple_conf, "Create app simple conf failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_simple_conf) >= 0), "Install app simple conf failed");

    PhoneAppComplexConf *app_complex_conf = new PhoneAppComplexConf();
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_complex_conf, "Create app complex conf failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_complex_conf) >= 0), "Install app complex conf failed");

    PhoneAppSquareline *app_squareline = PhoneAppSquareline::getInstance();
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_squareline, "Create app squareline failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_squareline) >= 0), "Install app squareline failed");

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
 * MIPI CSI Camera (OV5647)
 * NOTE: CSI and SDMMC share GPIO39/43/44 on this board.
 * Only one can be active at a time. Disable SDMMC when using camera.
 *============================================================================*/
static bool s_camera_get_new_vb(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    esp_cam_ctlr_trans_t *new_trans = (esp_cam_ctlr_trans_t *)user_data;
    trans->buffer = new_trans->buffer;
    trans->buflen = new_trans->buflen;
    return false;
}

static bool s_camera_get_finished_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    return false;
}

static void monitor_init_camera(void)
{
    esp_err_t ret = ESP_FAIL;
    size_t frame_buffer_size = EXAMPLE_DISP_HRES * EXAMPLE_DISP_VRES * EXAMPLE_RGB565_BYTES_PER_PIXEL;

    /* Allocate frame buffer in PSRAM for camera */
    void *frame_buffer = heap_caps_calloc(1, frame_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame_buffer) {
        ESP_LOGE(TAG, "Failed to allocate camera frame buffer");
        return;
    }
    ESP_LOGI(TAG, "Camera frame buffer: %dx%d, size: %zu, ptr: %p",
             EXAMPLE_DISP_HRES, EXAMPLE_DISP_VRES, frame_buffer_size, frame_buffer);

    /* NOTE: OV5647 sensor init requires I2C communication via BSP I2C bus.
     * Full sensor init via esp_cam_sensor component will be added when
     * camera support is enabled. Currently CSI + ISP are initialized
     * to validate the driver integration at build time. */

    /* CSI configuration */
    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id = 0,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .h_res = EXAMPLE_CAM_SENSOR_HRES,
        .v_res = EXAMPLE_CAM_SENSOR_VRES,
        .data_lane_num = 2,
        .lane_bit_rate_mbps = EXAMPLE_MIPI_CSI_LANE_BITRATE_MBPS,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RAW8,
        .data_type = {0},
        .queue_items = 1,
    };
    ret = esp_cam_new_csi_ctlr(&csi_config, &s_cam_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CSI init failed: %d", ret);
        free(frame_buffer);
        return;
    }

    esp_cam_ctlr_trans_t new_trans = {
        .buffer = frame_buffer,
        .buflen = frame_buffer_size,
    };
    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = s_camera_get_new_vb,
        .on_trans_finished = s_camera_get_finished_trans,
    };
    ESP_ERROR_CHECK(esp_cam_ctlr_register_event_callbacks(s_cam_handle, &cbs, &new_trans));
    ESP_ERROR_CHECK(esp_cam_ctlr_enable(s_cam_handle));

    /* ISP configuration: RAW8 -> RGB565 */
    esp_isp_processor_cfg_t isp_config = {
        .clk_hz = 80 * 1000 * 1000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet = false,
        .has_line_end_packet = false,
        .h_res = EXAMPLE_CAM_SENSOR_HRES,
        .v_res = EXAMPLE_CAM_SENSOR_VRES,
    };
    ESP_ERROR_CHECK(esp_isp_new_processor(&isp_config, &s_isp_proc));
    ESP_ERROR_CHECK(esp_isp_enable(s_isp_proc));

    /* Init frame buffer to white */
    memset(frame_buffer, 0xFF, frame_buffer_size);
    esp_cache_msync((void *)frame_buffer, frame_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    ESP_LOGI(TAG, "MIPI CSI camera (OV5647) initialized (sensor init pending)");
}

/*============================================================================
 * SDMMC (SD Card)
 *============================================================================*/
static void monitor_init_sdcard(void)
{
    esp_err_t ret;
    ESP_LOGI(TAG, "Initializing SD card via SDMMC...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    const char mount_point[] = SDMMC_MOUNT_POINT;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.unaligned_multi_block_rw_max_chunk_size = 8;
#if CONFIG_EXAMPLE_SDMMC_SPEED_HS
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = (gpio_num_t)CONFIG_EXAMPLE_PIN_CLK;
    slot_config.cmd = (gpio_num_t)CONFIG_EXAMPLE_PIN_CMD;
    slot_config.d0 = (gpio_num_t)CONFIG_EXAMPLE_PIN_D0;
    slot_config.d1 = (gpio_num_t)CONFIG_EXAMPLE_PIN_D1;
    slot_config.d2 = (gpio_num_t)CONFIG_EXAMPLE_PIN_D2;
    slot_config.d3 = (gpio_num_t)CONFIG_EXAMPLE_PIN_D3;
#endif
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s), continuing without SD", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "SD card mounted at %s", mount_point);
    sdmmc_card_print_info(stdout, s_card);
}

/*============================================================================
 * Audio I2S (ES8311 DAC + ES7210 ADC)
 *============================================================================*/
static void monitor_init_audio(void)
{
    monitor_init_audio_es8311_es7210();
}

static void monitor_init_audio_es8311_es7210(void)
{
    ESP_LOGI(TAG, "Initializing audio I2S (ES8311 + ES7210)...");

    /* I2S channel init */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)AUDIO_I2S_MCK_IO,
            .bclk = (gpio_num_t)AUDIO_I2S_BCK_IO,
            .ws = (gpio_num_t)AUDIO_I2S_WS_IO,
            .dout = (gpio_num_t)AUDIO_I2S_DO_IO,
            .din = (gpio_num_t)AUDIO_I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_handle));

    /* ES8311 DAC codec init via BSP I2C */
    i2c_master_bus_handle_t i2c_handle = bsp_i2c_get_handle();

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = AUDIO_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if);

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = AUDIO_I2S_NUM,
        .rx_handle = s_rx_handle,
        .tx_handle = s_tx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    assert(gpio_if);

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = (int16_t)EXAMPLE_PA_CTRL_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .no_dac_ref = false,
        .mclk_div = 384,
    };
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
    assert(es8311_if);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = es8311_if,
        .data_if = data_if,
    };
    s_codec_handle = esp_codec_dev_new(&dev_cfg);
    assert(s_codec_handle);

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = EXAMPLE_AUDIO_SAMPLE_RATE,
    };
    if (esp_codec_dev_open(s_codec_handle, &sample_cfg) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Open codec device failed");
        return;
    }
    esp_codec_dev_set_out_vol(s_codec_handle, EXAMPLE_VOICE_VOLUME);
    ESP_LOGI(TAG, "Audio I2S initialized (ES8311 DAC + ES7210 ADC)");
}

/*============================================================================
 * Main
 *============================================================================*/
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-P4 Monitor Starting ===");

    /* 1. MIPI DSI Display */
    lv_display_t *disp = NULL;
    monitor_init_display(&disp);

    /* 2. ESP-Brookesia Phone UI */
    monitor_init_brookesia(disp);

    /* 3. MIPI CSI Camera - NOTE: shares GPIO39/43/44 with SDMMC */
    // monitor_init_camera();  // Uncomment when camera is needed

    /* 4. SDMMC SD Card */
    monitor_init_sdcard();

    /* 5. Audio I2S (ES8311 + ES7210) */
    monitor_init_audio();

    ESP_LOGI(TAG, "=== All peripherals initialized ===");

    /* Memory monitor loop */
    char buffer[128];
    while (1) {
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        size_t external_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t external_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        snprintf(buffer, sizeof(buffer),
                 "SRAM: free=%zu/%zu  PSRAM: free=%zu/%zu  largest_free=%zu",
                 internal_free, internal_total,
                 external_free, external_total,
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        ESP_LOGI("MEM", "%s", buffer);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
