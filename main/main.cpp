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
#include "git_info.h"
#include "example_config.h"
#include "mdns.h"
#include "lwip/apps/netbiosns.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "ulog_writer.h"

static const char *TAG = "monitor";

/* Auto-detected: true if GT911 touch (I2C 0x5D) responds → LCD-4B board.
 * volatile: written once in app_main (core 0), read from web_config_server
 * task (any core). volatile ensures cross-core visibility for write-once flag. */
volatile bool g_has_lcd = false;

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
 *
 * MUST be called after WiFi is connected and has an IP address,
 * otherwise the delegated unique hostname won't resolve.
 *============================================================================*/
static SemaphoreHandle_t s_mdns_mutex = NULL;
static int  s_mdns_refcount = 0;
static bool s_mdns_initialized = false;

/* Must be called once from app_main before any task that uses mDNS. */
void shared_mdns_mutex_init(void)
{
    if (!s_mdns_mutex) {
        s_mdns_mutex = xSemaphoreCreateMutex();
    }
}

static SemaphoreHandle_t _mdns_mutex_get(void)
{
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

    /* Primary hostname: "esp-web" — convenient for single-device use.
     * In multi-device networks, mDNS conflict resolution appends a
     * suffix (e.g. esp-web-2), but the unique delegated hostname
     * below always resolves deterministically. */
    mdns_hostname_set("esp-web");

    /* Delegated hostname: "esp-web-XXXXXX" — unique per device.
     * Unlike the primary hostname, this never conflicts.
     * Get the current WiFi STA IP and associate it with the delegate,
     * so both hostnames resolve to this device. */
    if (strcmp(s_mdns_unique_hostname, "esp-web") != 0) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0) {
            static mdns_ip_addr_t s_delegate_addr = {};
            s_delegate_addr.addr.u_addr.ip4 = ip_info.ip;
            s_delegate_addr.addr.type = ESP_IPADDR_TYPE_V4;
            s_delegate_addr.next = NULL;
            mdns_delegate_hostname_add(s_mdns_unique_hostname, &s_delegate_addr);
            ESP_LOGI(TAG, "mDNS: esp-web.local + %s.local → " IPSTR,
                     s_mdns_unique_hostname, IP2STR(&ip_info.ip));
        } else {
            mdns_delegate_hostname_add(s_mdns_unique_hostname, NULL);
            ESP_LOGW(TAG, "mDNS: esp-web.local + %s.local (no IP yet, delegate may not resolve)",
                     s_mdns_unique_hostname);
        }
    } else {
        ESP_LOGI(TAG, "mDNS: esp-web.local");
    }

    netbiosns_init();
    netbiosns_set_name("esp-web");

    s_mdns_initialized = true;
    s_mdns_refcount = 1;

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

void shared_mdns_update_delegate_ip(void)
{
    SemaphoreHandle_t mtx = _mdns_mutex_get();
    if (mtx) xSemaphoreTake(mtx, portMAX_DELAY);

    if (!s_mdns_initialized || strcmp(s_mdns_unique_hostname, "esp-web") == 0) {
        if (mtx) xSemaphoreGive(mtx);
        return;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0) {
        static mdns_ip_addr_t s_delegate_addr = {};
        s_delegate_addr.addr.u_addr.ip4 = ip_info.ip;
        s_delegate_addr.addr.type = ESP_IPADDR_TYPE_V4;
        s_delegate_addr.next = NULL;
        mdns_delegate_hostname_set_address(s_mdns_unique_hostname, &s_delegate_addr);
        ESP_LOGI(TAG, "mDNS delegate: %s.local → " IPSTR,
                 s_mdns_unique_hostname, IP2STR(&ip_info.ip));
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
            /* ESP32-P4 PSRAM is DMA-capable (SOC_PSRAM_DMA_CAPABLE=y).
             * Allocating draw buffers from PSRAM frees ~72KB internal SRAM
             * for JPEG encoder DMA descriptors (rxlink/txlink). */
            .buff_dma = true,
#endif
            .buff_spiram = true,
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
    ESP_Brookesia_Phone *phone = nullptr;
    ESP_Brookesia_PhoneStylesheet_t *stylesheet = nullptr;

    bsp_display_lock(0);

    phone = new (std::nothrow) ESP_Brookesia_Phone(disp);
    if (!phone) { ESP_LOGE(TAG, "Create phone failed"); goto cleanup; }

    if ((BSP_LCD_H_RES == 1024) && (BSP_LCD_V_RES == 600)) {
        stylesheet = new (std::nothrow) ESP_Brookesia_PhoneStylesheet_t(ESP_BROOKESIA_PHONE_1024_600_DARK_STYLESHEET());
    } else if ((BSP_LCD_H_RES == 800) && (BSP_LCD_V_RES == 480)) {
        stylesheet = new (std::nothrow) ESP_Brookesia_PhoneStylesheet_t(ESP_BROOKESIA_PHONE_800_480_DARK_STYLESHEET());
    } else if ((BSP_LCD_H_RES == 480) && (BSP_LCD_V_RES == 480)) {
        stylesheet = new (std::nothrow) ESP_Brookesia_PhoneStylesheet_t(ESP_BROOKESIA_PHONE_480_480_DARK_STYLESHEET());
    } else if ((BSP_LCD_H_RES == 800) && (BSP_LCD_V_RES == 1280)) {
        stylesheet = new (std::nothrow) ESP_Brookesia_PhoneStylesheet_t(ESP_BROOKESIA_PHONE_800_1280_DARK_STYLESHEET());
    }

    if (stylesheet != nullptr) {
        ESP_LOGI(TAG, "Using stylesheet (%s)", stylesheet->core.name);
        if (!phone->addStylesheet(stylesheet) || !phone->activateStylesheet(stylesheet)) {
            ESP_LOGE(TAG, "Add/activate stylesheet failed");
            delete stylesheet;
            goto cleanup;
        }
        delete stylesheet;
    } else {
        ESP_LOGW(TAG, "No matching stylesheet for %dx%d, using default", BSP_LCD_H_RES, BSP_LCD_V_RES);
    }

    if (!phone->setTouchDevice(bsp_display_get_input_dev())) { ESP_LOGE(TAG, "Set touch device failed"); goto cleanup; }
    phone->registerLvLockCallback((ESP_Brookesia_GUI_LockCallback_t)(bsp_display_lock), 0);
    phone->registerLvUnlockCallback((ESP_Brookesia_GUI_UnlockCallback_t)(bsp_display_unlock));
    if (!phone->begin()) { ESP_LOGE(TAG, "Begin failed"); goto cleanup; }

    {
        PhoneAppSquareline *app_squareline = PhoneAppSquareline::getInstance();
        if (!app_squareline || phone->installApp(app_squareline) < 0) { ESP_LOGE(TAG, "Install app squareline failed"); goto cleanup; }
    }

    {
        PhoneAppCamera *app_camera = new (std::nothrow) PhoneAppCamera(false, false);
        if (!app_camera || phone->installApp(app_camera) < 0) { ESP_LOGE(TAG, "Install camera app failed"); delete app_camera; goto cleanup; }
    }

    {
        PhoneAppAudio *app_audio = new (std::nothrow) PhoneAppAudio(false, false);
        if (!app_audio || phone->installApp(app_audio) < 0) { ESP_LOGE(TAG, "Install audio app failed"); delete app_audio; goto cleanup; }
    }

    {
        PhoneAppMusic *app_music = new (std::nothrow) PhoneAppMusic(false, false);
        if (!app_music || phone->installApp(app_music) < 0) { ESP_LOGE(TAG, "Install music app failed"); delete app_music; goto cleanup; }
    }

    {
        PhoneAppSettings *app_settings = new (std::nothrow) PhoneAppSettings(false, false);
        if (!app_settings || phone->installApp(app_settings) < 0) { ESP_LOGE(TAG, "Install settings app failed"); delete app_settings; goto cleanup; }
    }

    {
        PhoneAppCameraStream *app_cam_stream = new (std::nothrow) PhoneAppCameraStream(false, false);
        if (!app_cam_stream || phone->installApp(app_cam_stream) < 0) { ESP_LOGE(TAG, "Install camera stream app failed"); delete app_cam_stream; goto cleanup; }
    }

    lv_timer_create(on_clock_update_timer_cb, 1000, phone);
    bsp_display_unlock();
    ESP_LOGI(TAG, "ESP-Brookesia Phone UI initialized");
    return;

cleanup:
    bsp_display_unlock();
    ESP_LOGE(TAG, "ESP-Brookesia Phone UI initialization failed");
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
 * SD card must already be mounted by the caller.
 *============================================================================*/
static void boot_sdcard_wifi_config(void)
{
    /* Check if WiFi SSID already exists in NVS */
    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open("settings", NVS_READONLY, &nvs_h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
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

    FILE *f = fopen(SDMMC_MOUNT_POINT "/wifi.txt", "r");
    if (!f) {
        ESP_LOGW(TAG, "wifi.txt not found on SD card");
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
    ESP_LOGI(TAG, "Git Info: %s", GIT_LOG1);
    ESP_LOGI(TAG, "  branch:  %s", GIT_BRANCH);
    ESP_LOGI(TAG, "  commit:  %s", GIT_COMMIT);
    ESP_LOGI(TAG, "  author:  %s", GIT_AUTHOR);
    ESP_LOGI(TAG, "  date:    %s", GIT_DATE);
    ESP_LOGI(TAG, "  message: %s", GIT_MSG);

    /* 0a. mDNS mutex init — must happen before any task uses shared_mdns_ensure/release */
    shared_mdns_mutex_init();

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

    if (g_has_lcd) {
        /* === LCD-4B: BSP display init (powers LDO4), then SD before WiFi === */
        lv_display_t *disp = NULL;
        monitor_init_display(&disp);

        /* Mount SD via SDSPI (LDO4 already powered by BSP display init above).
         * SDMMC native mode not used — host controller conflicts with C6 SDIO. */
        ESP_LOGI(TAG, "Mounting SD card (SDSPI, LDO4 from BSP)...");
        if (!PeripheralManager::instance().init_sdcard()) {
            ESP_LOGW(TAG, "SD card init failed at boot, continuing without SD");
        }

        /* Try SD wifi.txt AFTER SD is mounted */
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
        /* === WIFI6: mount SD via SDSPI (SDCardDriver powers LDO4), then WiFi === */
        if (!PeripheralManager::instance().init_sdcard()) {
            ESP_LOGW(TAG, "SD card init failed at boot, continuing without SD");
        }
        boot_sdcard_wifi_config();
        ESP_LOGI(TAG, "=== WIFI6 mode (no display) ===");
    }

    /* Boot WiFi auto-connect — AFTER SD card mount (C6 SDIO claims host ctrl) */
    PhoneAppSettings::bootWifiAutoConnect();

    web_config_server_start();

    /* ── ULog Logger initialization (only if SD card is mounted) ── */
    if (PeripheralManager::instance().sdcard_available()) {
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
    } else {
        ESP_LOGW(TAG, "SD card not available, skipping ULog writer init");
    }

    /* All setup complete — delete this task to reclaim its stack/TCB.
     * The FreeRTOS idle task will clean up. All work continues in
     * dedicated tasks (LVGL, WiFi, httpd, ULog, etc.). */
    vTaskDelete(NULL);
}
