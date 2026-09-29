/*
 * board_i2c — shared I2C master bus (ESP32-P4-WIFI6).
 * See board_i2c.h for the consumer list.
 */

#include "board_i2c.h"
#include "example_config.h"

static i2c_master_bus_handle_t s_i2c_handle;

esp_err_t board_i2c_init(void)
{
    if (s_i2c_handle) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = AUDIO_I2C_SDA_IO,
        .scl_io_num = AUDIO_I2C_SCL_IO,
        .i2c_port = AUDIO_I2C_NUM,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_handle);
    if (ret != ESP_OK) {
        s_i2c_handle = NULL;
    }
    return ret;
}

i2c_master_bus_handle_t board_i2c_get_handle(void)
{
    return s_i2c_handle;
}
