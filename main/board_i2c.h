#pragma once

/*
 * board_i2c — shared I2C master bus for the ESP32-P4-WIFI6 board.
 *
 * Single I2C bus (GPIO7=SDA, GPIO8=SCL, port 0) shared by:
 *   - ES8311 audio codec (0x18)
 *   - OV5647 camera SCCB (auto-detect)
 *   - esp_video SCCB (CONFIG_EXAMPLE_SCCB_I2C_INIT_BY_APP)
 *
 * Replaces the Waveshare LCD-4B BSP's bsp_i2c_init()/bsp_i2c_get_handle()
 * after LCD-4B board support was removed.
 */

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the shared I2C bus (idempotent). Call once from app_main
 * before any consumer (audio, camera) requests the handle. */
esp_err_t board_i2c_init(void);

/* Get the shared I2C bus handle (NULL before board_i2c_init succeeds). */
i2c_master_bus_handle_t board_i2c_get_handle(void);

#ifdef __cplusplus
}
#endif
