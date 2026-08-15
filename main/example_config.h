#pragma once
#include "sdkconfig.h"
#include "driver/i2c_master.h"
#include <atomic>

#ifdef __cplusplus
extern "C" {
#endif

/* Board detection: true = LCD-4B (display + touch + dual codec + PA),
 * false = WIFI6 (headless, single codec). Set in main.cpp at boot. */
#ifdef __cplusplus
extern std::atomic<bool> g_has_lcd;
#else
#include <stdatomic.h>
extern _Atomic bool g_has_lcd;
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
/* Sensor preset: MIPI_2lane_24Minput_RAW8_800x800_50fps.
 * Frame rate reduced to ~5fps at runtime via OV5647 VTS modification
 * (VTS: 984 → 9840, see ov5647_set_vts_5fps() in camera_stream.cpp).
 * ISP DMA bandwidth: ~32 MB/s → ~6 MB/s. */
#define EXAMPLE_CAM_FORMAT                     "MIPI_2lane_24Minput_RAW8_800x800_50fps"
#endif
#endif

/* Audio I2S */
#define EXAMPLE_AUDIO_SAMPLE_RATE     (16000)
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
#define AUDIO_PA_GPIO          53   /* Power amplifier enable (HIGH=ON) */

/* SD Card */
#define SDMMC_MOUNT_POINT     "/sdcard"
#define SD_SPI_HOST           SPI2_HOST
#define SD_SPI_MOSI_GPIO      44   /* CMD → MOSI (DI) */
#define SD_SPI_MISO_GPIO      39   /* D0  → MISO (DO) */
#define SD_SPI_SCLK_GPIO      43   /* CLK → SCLK */
#define SD_SPI_CS_GPIO        42   /* D3  → CS */

/* NVS shared keys — used by PhoneAppSettings, PhoneAppMusic, web_config_server.
 * All settings use NVS namespace "settings". Keys must match across modules. */
#define NVS_NAMESPACE_SETTINGS        "settings"
#define NVS_KEY_WIFI_SSID             "ssid"
#define NVS_KEY_WIFI_PASS             "pass"
#define NVS_KEY_VOLUME                "volume"
#define NVS_KEY_BRIGHTNESS            "brightness"
#define NVS_KEY_CAM_STREAM            "cam_stream"
#define NVS_KEY_CAM_ROTATION           "cam_rotation"

/* Volume / Brightness shared constants */
#define VOLUME_MIN                    0
#define VOLUME_MAX                    100
#define VOLUME_DEFAULT                60
#define BRIGHTNESS_MIN                20
#define BRIGHTNESS_MAX                100
#define BRIGHTNESS_DEFAULT            80

/* WiFi event group bits — shared by PhoneAppSettings and web_config_server */
#define WIFI_CONNECTED_BIT            BIT0

/* Shared mDNS initialization guard with reference counting.
 * Both CameraStream and web_config_server use mDNS.
 * - shared_mdns_mutex_init() must be called once from app_main before
 *   any task that uses mDNS (eliminates lazy-init TOCTOU race).
 * - shared_mdns_ensure() increments ref count; first caller inits mDNS.
 * - shared_mdns_release() decrements ref count; last caller deinits mDNS.
 * MUST be called after WiFi is connected (delegated hostname needs IP).
 * Individual modules should only add/remove their own services,
 * never call mdns_free() directly. */
void shared_mdns_mutex_init(void);
bool shared_mdns_ensure(void);
void shared_mdns_release(void);
void shared_mdns_update_delegate_ip(void);

/* Returns the unique mDNS hostname (e.g. "esp-web-a1b2c3").
 * Valid after shared_mdns_ensure() has been called. */
const char *shared_mdns_hostname(void);

/* BSP I2C bus handle — provided by Waveshare BSP (shared I2C bus for GT911/ES8311/ES7210/OV5647) */
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

#ifdef __cplusplus
}
#endif
