/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include "esp_log.h"
#include "esp_check.h"
#include "example_video_common.h"
#include "esp_cam_sensor_xclk.h"

/* Forward-declare BSP I2C handle getter (provided by Waveshare BSP component).
 * We avoid including bsp/esp-bsp.h here to keep the component self-contained. */
#include "driver/i2c_master.h"  /* for i2c_master_bus_handle_t */
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

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
static const char *TAG = "example_init_video";

esp_err_t example_video_init(void)
{
    esp_err_t ret;

    if (s_is_init) {
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
    return ESP_OK;

failed_2:
#if EXAMPLE_MIPI_CSI_XCLK_PIN > 0
    esp_cam_sensor_xclk_stop(s_xclk_handle);
failed_1:
    esp_cam_sensor_xclk_free(s_xclk_handle);
    s_xclk_handle = NULL;
failed_0:
#endif
    return ret;
}

esp_err_t example_video_deinit(void)
{
    esp_err_t ret = ESP_OK;

    if (!s_is_init) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_video_deinit(), TAG, "failed to deinitialize video");

#if EXAMPLE_MIPI_CSI_XCLK_PIN > 0
    ESP_RETURN_ON_ERROR(esp_cam_sensor_xclk_stop(s_xclk_handle), TAG, "failed to stop xclk");
    ESP_RETURN_ON_ERROR(esp_cam_sensor_xclk_free(s_xclk_handle), TAG, "failed to free xclk");
    s_xclk_handle = NULL;
#endif

    s_i2cbus_handle = NULL;
    s_is_init = false;

    return ret;
}
