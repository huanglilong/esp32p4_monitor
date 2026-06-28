#pragma once
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Display */
#define EXAMPLE_RGB565_BITS_PER_PIXEL           16
#define EXAMPLE_RGB565_BYTES_PER_PIXEL          (EXAMPLE_RGB565_BITS_PER_PIXEL / 8)
#define EXAMPLE_MIPI_DSI_LANE_BITRATE_MBPS      480
#define EXAMPLE_DISP_HRES                       BSP_LCD_H_RES
#define EXAMPLE_DISP_VRES                       BSP_LCD_V_RES

/* MIPI CSI Camera */
#define EXAMPLE_MIPI_CSI_LANE_BITRATE_MBPS      200
#define EXAMPLE_MIPI_CSI_CAM_SCCB_SCL_IO        (8)
#define EXAMPLE_MIPI_CSI_CAM_SCCB_SDA_IO        (7)
#define EXAMPLE_CAM_SENSOR_HRES                 CONFIG_EXAMPLE_MIPI_CSI_DISP_HRES
#define EXAMPLE_CAM_SENSOR_VRES                 CONFIG_EXAMPLE_MIPI_CSI_DISP_VRES

#if CONFIG_EXAMPLE_MIPI_CSI_HRES_800
#if CONFIG_EXAMPLE_MIPI_CSI_VRES_800
#define EXAMPLE_CAM_FORMAT                     "MIPI_2lane_24Minput_RAW8_800x800_50fps"
#endif
#endif

/* Audio I2S */
#define EXAMPLE_AUDIO_SAMPLE_RATE     (48000)
#define EXAMPLE_AUDIO_MCLK_MULTIPLE   (I2S_MCLK_MULTIPLE_256)
#define EXAMPLE_AUDIO_MCLK_FREQ_HZ    (EXAMPLE_AUDIO_SAMPLE_RATE * EXAMPLE_AUDIO_MCLK_MULTIPLE)
#define EXAMPLE_VOICE_VOLUME          CONFIG_EXAMPLE_VOICE_VOLUME
/* Microphone gain is set directly in code (30dB, main.cpp:309) */

/* Audio I2C */
#define AUDIO_I2C_NUM         (0)
#define AUDIO_I2C_SCL_IO      CONFIG_EXAMPLE_I2C_SCL_IO
#define AUDIO_I2C_SDA_IO      CONFIG_EXAMPLE_I2C_SDA_IO

/* Audio I2S */
#define AUDIO_I2S_NUM         (0)
#define AUDIO_I2S_MCK_IO      CONFIG_EXAMPLE_I2S_MCLK_IO
#define AUDIO_I2S_BCK_IO      CONFIG_EXAMPLE_I2S_BCLK_IO
#define AUDIO_I2S_WS_IO       CONFIG_EXAMPLE_I2S_WS_IO
#define AUDIO_I2S_DO_IO       CONFIG_EXAMPLE_I2S_DOUT_IO
#define AUDIO_I2S_DI_IO       CONFIG_EXAMPLE_I2S_DIN_IO

/* SD Card */
#define SDMMC_MOUNT_POINT     "/sdcard"

#ifdef __cplusplus
}
#endif
