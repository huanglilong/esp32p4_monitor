/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#pragma once

#include "linux/videodev2.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "example_video_common_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SCCB(I2C) pre-initialized port configuration
 */
#if CONFIG_EXAMPLE_SCCB_I2C_INIT_BY_APP
#define EXAMPLE_SCCB_I2C_PORT_INIT_BY_APP              CONFIG_EXAMPLE_SCCB_I2C_PORT_INIT_BY_APP
#endif

/**
 * @brief MIPI-CSI camera sensor common configuration
 */
#if CONFIG_EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR
#define EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR              1
#if CONFIG_EXAMPLE_SCCB_I2C_INIT_BY_APP
#define EXAMPLE_MIPI_CSI_SCCB_I2C_PORT                  EXAMPLE_SCCB_I2C_PORT_INIT_BY_APP
#else
#define EXAMPLE_MIPI_CSI_SCCB_I2C_PORT                  CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_PORT
#endif
#define EXAMPLE_MIPI_CSI_SCCB_I2C_FREQ                  CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_FREQ
#endif

/**
 * @brief Example camera device path configuration
 */
#if EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR
#define EXAMPLE_CAM_DEV_PATH                            ESP_VIDEO_MIPI_CSI_DEVICE_NAME
#else
#define EXAMPLE_CAM_DEV_PATH                            ESP_VIDEO_SPI_DEVICE_NAME
#endif

/**
 * @brief Example encoder handle
 */
typedef void *example_encoder_handle_t;

/**
 * @brief Example encoder configuration
 */
typedef struct example_encoder_config {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint8_t quality;
} example_encoder_config_t;

/**
 * @brief Initialize the video system
 */
esp_err_t example_video_init(void);

/**
 * @brief Deinitialize the video system
 */
esp_err_t example_video_deinit(void);

/**
 * @brief Initialize the encoder
 */
esp_err_t example_encoder_init(example_encoder_config_t *config, example_encoder_handle_t *ret_handle);

/**
 * @brief Allocate output buffer for encoder
 */
esp_err_t example_encoder_alloc_output_buffer(example_encoder_handle_t handle, uint8_t **buf, uint32_t *size);

/**
 * @brief Free encoder output buffer
 */
esp_err_t example_encoder_free_output_buffer(example_encoder_handle_t handle, uint8_t *buf);

/**
 * @brief Process encoder (encode a frame)
 */
esp_err_t example_encoder_process(example_encoder_handle_t handle, uint8_t *src_buf, uint32_t src_size,
                                   uint8_t *dst_buf, uint32_t dst_size, uint32_t *dst_size_out);

/**
 * @brief Set JPEG quality
 */
esp_err_t example_encoder_set_jpeg_quality(example_encoder_handle_t handle, uint8_t quality);

/**
 * @brief Deinitialize the encoder
 */
esp_err_t example_encoder_deinit(example_encoder_handle_t handle);

#ifdef __cplusplus
}
#endif
