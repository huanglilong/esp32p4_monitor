/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include "esp_log.h"
#include "esp_check.h"
#include "esp_vfs.h"
#include "example_video_common.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_video_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Forward-declare BSP I2C handle getter (provided by Waveshare BSP component).
 * We avoid including bsp/esp-bsp.h here to keep the component self-contained. */
#include "driver/i2c_master.h"  /* for i2c_master_bus_handle_t */
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

/* Forward-declare internal destroy functions from esp_video.
 * These are needed to force-cleanup stale video devices when esp_video_deinit()
 * fails or skips cleanup due to inconsistent internal flags.
 * Declared in esp_video_device_internal.h (private include, not in public path). */
extern esp_err_t esp_video_destroy_isp_video_device(void);
extern esp_err_t esp_video_destroy_csi_video_device(void);

#if EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR
static const esp_video_init_csi_config_t s_csi_config = {
    .sccb_config = {
#if !CONFIG_EXAMPLE_SCCB_I2C_INIT_BY_APP
        .init_sccb = true,
        .i2c_config = {
            .port      = EXAMPLE_MIPI_CSI_SCCB_I2C_PORT,
            .scl_pin   = EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN,
            .sda_pin   = EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN,
        },
#endif
        .freq = EXAMPLE_MIPI_CSI_SCCB_I2C_FREQ,
    },
    .reset_pin = EXAMPLE_MIPI_CSI_CAM_SENSOR_RESET_PIN,
    .pwdn_pin  = EXAMPLE_MIPI_CSI_CAM_SENSOR_PWDN_PIN,
};
#endif

static const esp_video_init_config_t s_cam_config = {
#if EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR
    .csi = &s_csi_config,
#endif
};

#if CONFIG_EXAMPLE_SCCB_I2C_INIT_BY_APP
static i2c_master_bus_handle_t s_i2cbus_handle;
#endif

#if defined(EXAMPLE_MIPI_CSI_XCLK_PIN) && EXAMPLE_MIPI_CSI_XCLK_PIN > 0
static esp_cam_sensor_xclk_handle_t s_xclk_handle;
#endif

static bool s_is_init = false;
static SemaphoreHandle_t s_init_mutex = NULL;
static const char *TAG = "example_init_video";

esp_err_t example_video_init(void)
{
    esp_err_t ret;

    /* Lazy-create mutex on first call */
    if (!s_init_mutex) {
        s_init_mutex = xSemaphoreCreateMutex();
    }
    if (s_init_mutex) {
        xSemaphoreTake(s_init_mutex, portMAX_DELAY);
    }

    if (s_is_init) {
        if (s_init_mutex) xSemaphoreGive(s_init_mutex);
        return ESP_OK;
    }

    const esp_video_init_config_t *cam_config_ptr = &s_cam_config;

#if CONFIG_EXAMPLE_SCCB_I2C_INIT_BY_APP
    /* Use the BSP's pre-initialized I2C bus (shared with audio, touch, camera) */
    s_i2cbus_handle = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(s_i2cbus_handle, ESP_ERR_INVALID_STATE, TAG, "BSP I2C bus not initialized");

#if EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR
    esp_video_init_csi_config_t csi_config = s_csi_config;
    csi_config.sccb_config.init_sccb = false;
    csi_config.sccb_config.i2c_handle = s_i2cbus_handle;
#endif

    esp_video_init_config_t cam_config = {
#if EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR
        .csi = &csi_config,
#endif
    };

    cam_config_ptr = &cam_config;
#endif

#if defined(EXAMPLE_MIPI_CSI_XCLK_PIN) && EXAMPLE_MIPI_CSI_XCLK_PIN > 0
    esp_cam_sensor_xclk_config_t cam_xclk_config = {
        .esp_clock_router_cfg = {
            .xclk_pin = EXAMPLE_MIPI_CSI_XCLK_PIN,
            .xclk_freq_hz = EXAMPLE_MIPI_CSI_XCLK_FREQ,
        }
    };
    ESP_LOGI(TAG, "MIPI-CSI xclk pin=%d, freq=%d", EXAMPLE_MIPI_CSI_XCLK_PIN, EXAMPLE_MIPI_CSI_XCLK_FREQ);
    ESP_GOTO_ON_ERROR(esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER, &s_xclk_handle),
                      failed_0, TAG, "failed to allocate xclk");
    ESP_GOTO_ON_ERROR(esp_cam_sensor_xclk_start(s_xclk_handle, &cam_xclk_config),
                      failed_1, TAG, "failed to start xclk");
#endif

    ESP_LOGI(TAG, "MIPI-CSI camera sensor I2C port=%d, scl_pin=%d, sda_pin=%d, freq=%d",
             EXAMPLE_MIPI_CSI_SCCB_I2C_PORT,
             EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN,
             EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN,
             EXAMPLE_MIPI_CSI_SCCB_I2C_FREQ);

    ESP_GOTO_ON_ERROR(esp_video_init(cam_config_ptr), failed_2, TAG, "failed to initialize video");

    s_is_init = true;
    if (s_init_mutex) xSemaphoreGive(s_init_mutex);
    return ESP_OK;

failed_2:
    /* esp_video_init() may have partially registered VFS devices before
     * failing.  esp_video_deinit() uses internal flags (s_video_device_inited_flags)
     * that may not reflect partial state, so it skips cleanup.
     *
     * IMPORTANT: Call esp_video_deinit() FIRST to let it clean up properly
     * (it removes objects from s_video_list AND unregisters VFS). Only force-
     * unregister VFS entries as a last resort AFTER deinit, because pre-
     * unregistering VFS causes esp_video_destroy() to fail (can't unregister
     * already-removed VFS), which aborts all subsequent device cleanup. */
    ESP_LOGW(TAG, "esp_video_init failed, attempting cleanup and retry...");

    /* Step 1: Full deinit — removes objects from s_video_list + VFS */
    esp_video_deinit();

    /* Step 2: Force-destroy any stale objects that deinit skipped
     * (happens when s_video_device_inited_flags didn't get set) */
    esp_video_destroy_isp_video_device();
    esp_video_destroy_csi_video_device();

    /* Step 3: Force-unregister any VFS entries still remaining */
    {
        static const int video_ids[] = { ESP_VIDEO_MIPI_CSI_DEVICE_ID, ESP_VIDEO_ISP1_DEVICE_ID };
        for (size_t i = 0; i < sizeof(video_ids) / sizeof(video_ids[0]); i++) {
            char vfs_path[16];
            snprintf(vfs_path, sizeof(vfs_path), "/dev/video%d", video_ids[i]);
            esp_err_t vfs_ret = esp_vfs_unregister(vfs_path);
            if (vfs_ret == ESP_OK) {
                ESP_LOGW(TAG, "Unregistered stale VFS device %s", vfs_path);
            }
            /* ESP_ERR_INVALID_STATE = not registered (expected), suppress */
        }
    }

    /* Short delay to let hardware settle before retry */
    vTaskDelay(pdMS_TO_TICKS(100));

    ret = esp_video_init(cam_config_ptr);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "esp_video_init retry succeeded after VFS cleanup");
        s_is_init = true;
        if (s_init_mutex) xSemaphoreGive(s_init_mutex);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "esp_video_init retry also failed (0x%x)", ret);
#if EXAMPLE_MIPI_CSI_XCLK_PIN > 0
    esp_cam_sensor_xclk_stop(s_xclk_handle);
failed_1:
    esp_cam_sensor_xclk_free(s_xclk_handle);
    s_xclk_handle = NULL;
failed_0:
#endif
    if (s_init_mutex) xSemaphoreGive(s_init_mutex);
    return ret;
}

esp_err_t example_video_deinit(void)
{
    esp_err_t ret = ESP_OK;

    if (s_init_mutex) {
        xSemaphoreTake(s_init_mutex, portMAX_DELAY);
    }

    if (!s_is_init) {
        ESP_LOGW(TAG, "example_video_deinit: not initialized, skipping");
        if (s_init_mutex) xSemaphoreGive(s_init_mutex);
        return ESP_OK;
    }

    ret = esp_video_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_deinit failed (0x%x), attempting force cleanup", ret);
        /* Force-destroy ISP and CSI devices if normal deinit failed.
         * This can happen when V4L2 fds are still open or internal flags
         * are out of sync. Force cleanup ensures VFS devices are removed. */
        esp_video_destroy_isp_video_device();
        esp_video_destroy_csi_video_device();
        /* Also force VFS unregister as last resort */
        {
            static const int video_ids[] = { ESP_VIDEO_MIPI_CSI_DEVICE_ID, ESP_VIDEO_ISP1_DEVICE_ID };
            for (size_t i = 0; i < sizeof(video_ids) / sizeof(video_ids[0]); i++) {
                char vfs_path[16];
                snprintf(vfs_path, sizeof(vfs_path), "/dev/video%d", video_ids[i]);
                esp_vfs_unregister(vfs_path);
            }
        }
    }

#if EXAMPLE_MIPI_CSI_XCLK_PIN > 0
    esp_cam_sensor_xclk_stop(s_xclk_handle);
    esp_cam_sensor_xclk_free(s_xclk_handle);
    s_xclk_handle = NULL;
#endif

    s_i2cbus_handle = NULL;
    /* Always clear s_is_init even on partial failure — prevents
     * subsequent calls from being silently skipped. */
    s_is_init = false;

    if (s_init_mutex) xSemaphoreGive(s_init_mutex);
    return ret;
}
