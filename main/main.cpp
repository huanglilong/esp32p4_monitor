#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_ldo_regulator.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_brookesia.hpp"
#include "private/esp_brookesia_utils.h"
#include "peripherals.hpp"
#include "phone_app_camera.hpp"
#include "phone_app_audio.hpp"
#include "phone_app_music.hpp"
#include "phone_app_settings.hpp"
#include "phone_app_camera_stream.hpp"
#include "web_config_server.hpp"
#include "example_config.h"
#include "mdns.h"
#include "lwip/apps/netbiosns.h"
#include "esp_mac.h"
#include "ulog_writer.h"

static const char *TAG = "monitor";

/* Auto-detected: true if GT911 touch (I2C 0x5D) responds → LCD-4B board */
bool g_has_lcd = false;

/*============================================================================
 * mDNS hostnames:
 *   Primary:   "esp-web-XXXXXX"   — unique per device (MAC suffix), always
 *                                   resolves deterministically.  This is the
 *                                   hostname advertised in SRV records, so
 *                                   Flutter app discovery always sees a stable,
 *                                   unique name.
 *   Delegated: "esp-web"           — convenient alias for single-device use.
 *                                   In multi-device networks this may conflict
 *                                   and get auto-renamed by mDNS (e.g. esp-web-2),
 *                                   but the primary hostname above is always stable.
 *============================================================================*/
static char s_mdns_unique_hostname[24] = {0};

const char *shared_mdns_hostname(void)
{
    return s_mdns_unique_hostname;
}

static void _build_mdns_hostnames(void)
{
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        snprintf(s_mdns_unique_hostname, sizeof(s_mdns_unique_hostname),
                 "esp-web-%02x%02x%02x", mac[3], mac[4], mac[5]);
    } else {
        /* Fallback: use generic name if MAC read fails (should not happen) */
        strlcpy(s_mdns_unique_hostname, "esp-web", sizeof(s_mdns_unique_hostname));
    }
}

/*============================================================================
 * Shared mDNS initialization guard with reference counting.
 * Both CameraStream and web_config_server use mDNS.
 * - shared_mdns_ensure() increments the ref count; first caller inits mDNS.
 * - shared_mdns_release() decrements the ref count; last caller deinits mDNS.
 * Individual modules should only add/remove their own services, never call
 * mdns_free() directly.
 *============================================================================*/
static SemaphoreHandle_t s_mdns_mutex = NULL;
static int  s_mdns_refcount = 0;
static bool s_mdns_initialized = false;

/* Create the mutex lazily on first call.  Safe because shared_mdns_ensure()
 * is called before FreeRTOS scheduler starts (from main) or from the first
 * task that needs mDNS.  If called concurrently, the worst case is two
 * mutexes are created and one leaks — but that's better than a data race. */
static SemaphoreHandle_t _mdns_mutex_get(void)
{
    if (!s_mdns_mutex) {
        s_mdns_mutex = xSemaphoreCreateMutex();
    }
    return s_mdns_mutex;
}

bool shared_mdns_ensure(void)
{
    SemaphoreHandle_t mtx = _mdns_mutex_get();
    if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);

    if (s_mdns_initialized) {
        s_mdns_refcount++;
        if (mtx) xSemaphoreGive(mtx);
        return true;
    }
    if (mdns_init() != ESP_OK) {
        if (mtx) xSemaphoreGive(mtx);
        return false;
    }

    _build_mdns_hostnames();

    /* Primary hostname: unique per device — SRV records use this name,
     * so discovery clients (Flutter app) always get a stable identifier. */
    mdns_hostname_set(s_mdns_unique_hostname);

    /* Delegated hostname: "esp-web" — convenient alias for single-device
     * scenarios.  NULL address = auto-detect from netif. */
    if (strcmp(s_mdns_unique_hostname, "esp-web") != 0) {
        mdns_delegate_hostname_add("esp-web", NULL);
    }

    netbiosns_init();
    netbiosns_set_name(s_mdns_unique_hostname);

    s_mdns_initialized = true;
    s_mdns_refcount = 1;
    ESP_LOGI(TAG, "mDNS: %s.local (primary) + esp-web.local (alias)", s_mdns_unique_hostname);

    if (mtx) xSemaphoreGive(mtx);
    return true;
}

void shared_mdns_release(void)
{
    SemaphoreHandle_t mtx = _mdns_mutex_get();
    if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);

    if (!s_mdns_initialized) {
        if (mtx) xSemaphoreGive(mtx);
        return;
    }
    s_mdns_refcount--;
    if (s_mdns_refcount <= 0) {
        mdns_free();
        s_mdns_initialized = false;
        s_mdns_refcount = 0;
        ESP_LOGI(TAG, "mDNS: fully deinitialized (last user released)");
    }

    if (mtx) xSemaphoreGive(mtx);
}

/* Forward declarations */
static void monitor_init_display(lv_display_t **disp);
static void monitor_init_brookesia(lv_display_t *disp);
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

    ESP_Brookesia_Phone *phone = new (std::nothrow) ESP_Brookesia_Phone(disp);
    ESP_BROOKESIA_CHECK_NULL_EXIT(phone, "Create phone failed");

    ESP_Brookesia_PhoneStylesheet_t *stylesheet = nullptr;
    if ((BSP_LCD_H_RES == 1024) && (BSP_LCD_V_RES == 600)) {
        stylesheet = new (std::nothrow) ESP_Brookesia_PhoneStylesheet_t(ESP_BROOKESIA_PHONE_1024_600_DARK_STYLESHEET());
        ESP_BROOKESIA_CHECK_NULL_EXIT(stylesheet, "Create stylesheet failed");
    } else if ((BSP_LCD_H_RES == 800) && (BSP_LCD_V_RES == 480)) {
        stylesheet = new (std::nothrow) ESP_Brookesia_PhoneStylesheet_t(ESP_BROOKESIA_PHONE_800_480_DARK_STYLESHEET());
        ESP_BROOKESIA_CHECK_NULL_EXIT(stylesheet, "Create stylesheet failed");
    } else if ((BSP_LCD_H_RES == 480) && (BSP_LCD_V_RES == 480)) {
        stylesheet = new (std::nothrow) ESP_Brookesia_PhoneStylesheet_t(ESP_BROOKESIA_PHONE_480_480_DARK_STYLESHEET());
        ESP_BROOKESIA_CHECK_NULL_EXIT(stylesheet, "Create stylesheet failed");
    } else if ((BSP_LCD_H_RES == 800) && (BSP_LCD_V_RES == 1280)) {
        stylesheet = new (std::nothrow) ESP_Brookesia_PhoneStylesheet_t(ESP_BROOKESIA_PHONE_800_1280_DARK_STYLESHEET());
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

    PhoneAppCamera *app_camera = new (std::nothrow) PhoneAppCamera(false, false);
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_camera, "Create camera app failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_camera) >= 0), "Install camera app failed");

    PhoneAppAudio *app_audio = new (std::nothrow) PhoneAppAudio(false, false);
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_audio, "Create audio app failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_audio) >= 0), "Install audio app failed");

    PhoneAppMusic *app_music = new (std::nothrow) PhoneAppMusic(false, false);
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_music, "Create music app failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_music) >= 0), "Install music app failed");

    PhoneAppSettings *app_settings = new (std::nothrow) PhoneAppSettings(false, false);
    ESP_BROOKESIA_CHECK_NULL_EXIT(app_settings, "Create settings app failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT((phone->installApp(app_settings) >= 0), "Install settings app failed");

    PhoneAppCameraStream *app_cam_stream = new (std::nothrow) PhoneAppCameraStream(false, false);
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
 * SD Card WiFi Config (first-boot fallback)
 * If NVS has no WiFi SSID, try reading wifi.txt from SD card.
 *============================================================================*/
static void boot_sdcard_wifi_config(void)
{
    /* Check if WiFi SSID already exists in NVS */
    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open("settings", NVS_READONLY, &nvs_h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Namespace doesn't exist yet — no SSID stored, proceed to SD */
        ESP_LOGI(TAG, "No NVS settings namespace, trying SD wifi.txt...");
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return;
    } else {
        char ssid[33] = {};
        size_t len = sizeof(ssid);
        nvs_get_str(nvs_h, "ssid", ssid, &len);
        nvs_close(nvs_h);
        if (strlen(ssid) > 0) {
            ESP_LOGI(TAG, "WiFi SSID already in NVS (%s), skip SD wifi.txt", ssid);
            return;
        }
    }

    ESP_LOGI(TAG, "No WiFi SSID in NVS, trying SD card wifi.txt...");

    /* Configure GPIO45 for SD card power reset (used after BSP unmount on LCD-4B) */
    if (g_has_lcd) {
        gpio_config_t sd_pwr_io = { .pin_bit_mask = (1ULL << 45), .mode = GPIO_MODE_OUTPUT };
        gpio_config(&sd_pwr_io);
    }

    sdmmc_card_t *sd_card = NULL;
    bool is_sdspi = false;

    if (g_has_lcd) {
        /* LCD-4B: use BSP native SDMMC (LDO powered by BSP display init) */
        ESP_LOGI(TAG, "Mounting SD card (BSP SDMMC)...");
        esp_err_t ret = bsp_sdcard_mount();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "BSP SD mount failed (%s)", esp_err_to_name(ret));
            return;
        }
        sd_card = bsp_sdcard;
    } else {
        /* WIFI6: self-powered SDSPI (SDMMC ctrl claimed by C6) */
        sd_pwr_ctrl_handle_t pwr_ctrl = NULL;
        sd_pwr_ctrl_ldo_config_t ldo_config = { .ldo_chan_id = 4 };
        esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SD LDO init: %s", esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(100));

        ESP_LOGI(TAG, "Mounting SD card (SDSPI)...");
        esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
            .format_if_mount_failed = false,
            .max_files = 2,
            .allocation_unit_size = 16 * 1024,
        };
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = SPI2_HOST;
        sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_cfg.gpio_cs = GPIO_NUM_42;
        slot_cfg.host_id = SPI2_HOST;
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = GPIO_NUM_44,
            .miso_io_num = GPIO_NUM_39,
            .sclk_io_num = GPIO_NUM_43,
            .quadwp_io_num = -1, .quadhd_io_num = -1,
            .max_transfer_sz = 4000,
        };
        ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SPI bus init failed (%s)", esp_err_to_name(ret));
            return;
        }
        ret = esp_vfs_fat_sdspi_mount(SDMMC_MOUNT_POINT, &host, &slot_cfg,
                                       &mount_cfg, &sd_card);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SD mount failed (%s)", esp_err_to_name(ret));
            spi_bus_free(SPI2_HOST);
            return;
        }
        is_sdspi = true;
    }

    FILE *f = fopen(SDMMC_MOUNT_POINT "/wifi.txt", "r");
    if (!f) {
        ESP_LOGW(TAG, "wifi.txt not found on SD card");
        if (is_sdspi) {
            esp_vfs_fat_sdcard_unmount(SDMMC_MOUNT_POINT, sd_card);
            spi_bus_free(SPI2_HOST);
        } else {
            bsp_sdcard_unmount();
            /* Power-cycle SD to reset mode (SDMMC → clean state for SDSPI later) */
            gpio_set_level(GPIO_NUM_45, 0);
            vTaskDelay(pdMS_TO_TICKS(10));
            gpio_set_level(GPIO_NUM_45, 1);
        }
        return;
    }

    char line[128];
    char file_ssid[33] = {};
    char file_pass[65] = {};

    while (fgets(line, sizeof(line), f)) {
        /* Trim trailing newline */
        size_t sl = strlen(line);
        while (sl > 0 && (line[sl - 1] == '\n' || line[sl - 1] == '\r'))
            line[--sl] = '\0';

        if (strncmp(line, "ssid:", 5) == 0) {
            const char *val = line + 5;
            while (*val == ' ' || *val == '\t') val++;
            strlcpy(file_ssid, val, sizeof(file_ssid));
        } else if (strncmp(line, "password:", 9) == 0) {
            const char *val = line + 9;
            while (*val == ' ' || *val == '\t') val++;
            strlcpy(file_pass, val, sizeof(file_pass));
        }
    }
    fclose(f);
    if (is_sdspi) {
        esp_vfs_fat_sdcard_unmount(SDMMC_MOUNT_POINT, sd_card);
        spi_bus_free(SPI2_HOST);
    } else {
        bsp_sdcard_unmount();
        /* Power-cycle SD to reset mode for later SDSPI use */
        gpio_set_level(GPIO_NUM_45, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(GPIO_NUM_45, 1);
    }

    if (strlen(file_ssid) == 0) {
        ESP_LOGW(TAG, "wifi.txt missing ssid field");
        return;
    }

    ESP_LOGI(TAG, "Read from wifi.txt: ssid=%s, pass_len=%d", file_ssid, (int)strlen(file_pass));

    /* Save to NVS */
    if (nvs_open("settings", NVS_READWRITE, &nvs_h) != ESP_OK) return;
    nvs_set_str(nvs_h, "ssid", file_ssid);
    if (strlen(file_pass) > 0)
        nvs_set_str(nvs_h, "pass", file_pass);
    nvs_set_i32(nvs_h, "wifi_en", 1);
    nvs_commit(nvs_h);
    nvs_close(nvs_h);
    ESP_LOGI(TAG, "WiFi config saved to NVS from SD wifi.txt");
}

/*============================================================================
 * Auto-detect LCD via GT911 I2C probe
 *============================================================================*/
static bool detect_lcd_via_i2c(void)
{
    /* BSP I2C must be initialized first */
    bsp_i2c_init();
    i2c_master_bus_handle_t i2c = bsp_i2c_get_handle();
    if (!i2c) return false;

    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x5D,  /* GT911 default */
        .scl_speed_hz = 100000,
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c, &dev_cfg, &dev);
    if (ret != ESP_OK) return false;

    /* Probe: write 1 byte, check for ACK (null buffer not allowed) */
    uint8_t dummy = 0;
    ret = i2c_master_transmit(dev, &dummy, 1, pdMS_TO_TICKS(50));
    i2c_master_bus_rm_device(dev);
    return (ret == ESP_OK);
}

/*============================================================================
 * Main
 *============================================================================*/
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-P4 Monitor Starting ===");

    /* 0. NVS init */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_LOGI(TAG, "NVS initialized");

    /* 0b. Auto-detect LCD via GT911 I2C probe */
    g_has_lcd = detect_lcd_via_i2c();
    PeripheralManager::instance().set_has_lcd(g_has_lcd);
    ESP_LOGI(TAG, "LCD detected: %s", g_has_lcd ? "YES (LCD-4B)" : "NO (WIFI6)");

    /* Boot WiFi auto-connect */
    PhoneAppSettings::bootWifiAutoConnect();

    if (g_has_lcd) {
        /* === LCD-4B: BSP display init (powers SDMMC), then SD wifi.txt === */
        lv_display_t *disp = NULL;
        monitor_init_display(&disp);

        /* Try SD wifi.txt AFTER BSP powers SD slot */
        boot_sdcard_wifi_config();

        /* Apply saved brightness */
        {
            nvs_handle_t nvs_h;
            if (nvs_open("settings", NVS_READONLY, &nvs_h) == ESP_OK) {
                int32_t brightness = 80;
                nvs_get_i32(nvs_h, "brightness", &brightness);
                if (brightness < 20) brightness = 20;
                if (brightness > 100) brightness = 100;
                bsp_display_brightness_set((int)brightness);
                ESP_LOGI(TAG, "Brightness loaded from NVS: %ld", brightness);
                nvs_close(nvs_h);
            }
        }

        monitor_init_brookesia(disp);
        ESP_LOGI(TAG, "=== LCD-4B mode initialized ===");
    } else {
        /* === WIFI6: SD wifi.txt (SDSPI), then WiFi === */
        boot_sdcard_wifi_config();
        ESP_LOGI(TAG, "=== WIFI6 mode (no display) ===");
    }
    web_config_server_start();

    /* ── ULog Logger initialization ── */
    ulog_writer_t *ulog = ulog_writer_get();
    ulog_writer_init(ulog, "/sdcard");
    ulog_writer_add_topic(ulog, ORB_ID(fps_stats), 0);       /* default 100ms */
    ulog_writer_add_topic(ulog, ORB_ID(detection_result), 0); /* default 100ms */
    ulog_writer_add_topic(ulog, ORB_ID(wifi_state), 500);     /* 500ms */
    ulog_writer_add_topic(ulog, ORB_ID(audio_level), 100);    /* same as UI refresh */
    ulog_writer_add_topic(ulog, ORB_ID(camera_state), 0);     /* default 100ms */
    ulog_writer_add_topic(ulog, ORB_ID(recording_state), 0);  /* default 100ms */
    ulog_writer_add_topic(ulog, ORB_ID(volume_state), 0);     /* default 100ms */
    ulog_writer_add_topic(ulog, ORB_ID(ulog_state), 0);       /* log the logger itself */
    ESP_LOGI(TAG, "ULog writer initialized with %d topics", 8);

    /* Idle loop */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
