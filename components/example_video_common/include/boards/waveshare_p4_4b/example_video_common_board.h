/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Waveshare ESP32-P4-WiFi6-Touch-LCD-4B board configuration
 *
 * MIPI CSI camera (OV5647):
 *   - I2C SCL=8, SDA=7 (shared bus with audio, touch)
 *   - No reset pin, no pwdn pin, no xclk pin (MIPI CSI uses internal clock)
 */

#if CONFIG_EXAMPLE_SCCB_I2C_INIT_BY_APP
#define EXAMPLE_SCCB_I2C_SCL_PIN_INIT_BY_APP            8
#define EXAMPLE_SCCB_I2C_SDA_PIN_INIT_BY_APP            7
#endif /* CONFIG_EXAMPLE_SCCB_I2C_INIT_BY_APP */

#if CONFIG_EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR
/**
 * @brief MIPI-CSI camera sensor configuration
 */
#define EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN               8
#define EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN               7
#define EXAMPLE_MIPI_CSI_CAM_SENSOR_RESET_PIN           -1
#define EXAMPLE_MIPI_CSI_CAM_SENSOR_PWDN_PIN            -1
#define EXAMPLE_MIPI_CSI_XCLK_PIN                       -1
#endif /* CONFIG_EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR */

#ifdef __cplusplus
}
#endif
