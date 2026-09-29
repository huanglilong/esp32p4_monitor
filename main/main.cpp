#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "board_i2c.h"
#include "peripherals.hpp"
#include "web_config_server.hpp"
#include "wifi_service.hpp"
#include "logger/logger.hpp"
#include "system_monitor.hpp"
#include "git_info.h"
#include "example_config.h"
#include "mdns.h"
#include "lwip/apps/netbiosns.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "ulog_writer.h"
#include "uorb.h"


static const char *TAG = "monitor";

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
        esp_err_t nvs_err = nvs_get_str(nvs_h, "ssid", ssid, &len);
        nvs_close(nvs_h);
        if (nvs_err != ESP_OK && nvs_err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "NVS read ssid failed: %s", esp_err_to_name(nvs_err));
            return;
        }
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
    nvs_commit(nvs_h);
    nvs_close(nvs_h);
    ESP_LOGI(TAG, "WiFi config saved to NVS from SD wifi.txt");
}

/*============================================================================
 * Main
 *============================================================================*/
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-P4 Monitor Starting ===");

    /* 0a. mDNS mutex init — must happen before any task uses shared_mdns_ensure/release */
    shared_mdns_mutex_init();

    /* 0b. uORB init — must happen before any uORB API calls */
    orb_init();

    /* 0. NVS init */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_LOGI(TAG, "NVS initialized");

    /* 0b. Shared I2C bus (ES8311 codec + OV5647 SCCB + esp_video) */
    ESP_ERROR_CHECK(board_i2c_init());

    /* Mount SD via SDSPI (SDCardDriver powers LDO4), then WiFi */
    if (!PeripheralManager::instance().init_sdcard()) {
        ESP_LOGW(TAG, "SD card init failed at boot, continuing without SD");
    }
    boot_sdcard_wifi_config();

    /* Boot WiFi — use WifiService (wifi_manager) instead of inline code.
     * WifiService reads NVS credentials, starts STA, and auto-reconnects forever.
     * On boards without stored credentials, it starts an AP for provisioning. */
    ESP_ERROR_CHECK(WifiService::instance().init());
    ESP_ERROR_CHECK(WifiService::instance().start());

    web_config_server_start();

    /* ── Text Logger (SD card, ESP_LOG* capture) ── */
    if (PeripheralManager::instance().sdcard_available()) {
        logger_init("/sdcard");
    } else {
        ESP_LOGW(TAG, "SD card not available, skipping text logger init");
    }

    // git info
    ESP_LOGI(TAG, "Git Info: %s", GIT_LOG1);
    ESP_LOGI(TAG, "  branch:  %s", GIT_BRANCH);
    ESP_LOGI(TAG, "  commit:  %s", GIT_COMMIT);
    ESP_LOGI(TAG, "  author:  %s", GIT_AUTHOR);
    ESP_LOGI(TAG, "  date:    %s", GIT_DATE);
    ESP_LOGI(TAG, "  message: %s", GIT_MSG);

    /* ── ULog Logger initialization (only if SD card is mounted) ── */
    if (PeripheralManager::instance().sdcard_available()) {
        /* Session counter: load from NVS, increment, save */
        uint16_t session = 0;
        nvs_handle_t nvs_h;
        if (nvs_open("ulog", NVS_READONLY, &nvs_h) == ESP_OK) {
            nvs_get_u16(nvs_h, "session", &session);
            nvs_close(nvs_h);
        }
        if (session > 60000) session = 0;
        session++;
        if (nvs_open("ulog", NVS_READWRITE, &nvs_h) == ESP_OK) {
            nvs_set_u16(nvs_h, "session", session);
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
        }

        /* Check wall-clock time availability */
        bool has_rtc = false;
        struct timespec ts;
        if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
            has_rtc = (uint64_t)ts.tv_sec > 1577836800ULL; /* > 2020-01-01 */
        }

        /* Hardware info */
        uint8_t mac[6];
        if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
            memset(mac, 0, sizeof(mac));
            ESP_LOGW(TAG, "MAC read failed, ULog UUID will be zeros");
        }
        char sys_uuid[24];
        snprintf(sys_uuid, sizeof(sys_uuid), "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        ulog_init_config_t cfg = {};
        cfg.session_counter = session;
        cfg.has_wall_clock = has_rtc;
        strlcpy(cfg.sys_name, "esp32p4_monitor", sizeof(cfg.sys_name));
        snprintf(cfg.ver_sw, sizeof(cfg.ver_sw), "IDF %s", esp_get_idf_version());
        strlcpy(cfg.ver_hw, "ESP32-P4-WIFI6", sizeof(cfg.ver_hw));
        strlcpy(cfg.sys_uuid, sys_uuid, sizeof(cfg.sys_uuid));
        strlcpy(cfg.sys_os_name, "FreeRTOS", sizeof(cfg.sys_os_name));
        strlcpy(cfg.sys_os_ver, esp_get_idf_version(), sizeof(cfg.sys_os_ver));
        strlcpy(cfg.sys_mcu, "ESP32-P4NRW32", sizeof(cfg.sys_mcu));
        strlcpy(cfg.arch, "esp32p4", sizeof(cfg.arch));

        ulog_writer_t *ulog = ulog_writer_get();
        esp_err_t ulog_ret = ulog_writer_init(ulog, "/sdcard", &cfg);
        if (ulog_ret != ESP_OK) {
            ESP_LOGE(TAG, "ULog writer init failed: %s — skipping topic registration", esp_err_to_name(ulog_ret));
        } else {
            /* Pass git version info for ULog Info messages */
            ulog_git_info_t git = {};
            strlcpy(git.branch, GIT_BRANCH, sizeof(git.branch));
            strlcpy(git.commit, GIT_COMMIT, sizeof(git.commit));
            strlcpy(git.author, GIT_AUTHOR, sizeof(git.author));
            strlcpy(git.date, GIT_DATE, sizeof(git.date));
            strlcpy(git.message, GIT_MSG, sizeof(git.message));
            ulog_writer_set_git_info(ulog, &git);

            ulog_writer_add_topic(ulog, ORB_ID(fps_stats), 0);       /* default 100ms */
            ulog_writer_add_topic(ulog, ORB_ID(wifi_state), 500);     /* 500ms */
            ulog_writer_add_topic(ulog, ORB_ID(camera_state), 0);     /* default 100ms */
            ulog_writer_add_topic(ulog, ORB_ID(recording_state), 0);  /* default 100ms */
            ulog_writer_add_topic(ulog, ORB_ID(volume_state), 0);     /* default 100ms */
            ulog_writer_add_topic(ulog, ORB_ID(ulog_state), 0);       /* log the logger itself */
            ulog_writer_add_topic(ulog, ORB_ID(system_stats), 500);   /* system CPU/memory every 500ms */
            ulog_writer_add_topic(ulog, ORB_ID(system_alert), 0);     /* alerts on event */
            ulog_writer_add_topic(ulog, ORB_ID(camera_frame_chunk), 30);   /* camera JPEG chunks, 30ms to capture all chunks per frame */
            ulog_writer_add_topic(ulog, ORB_ID(audio_frame), 30);       /* audio AAC frames, 30ms = ~15.6fps */
            ESP_LOGI(TAG, "ULog writer initialized with %d topics", 10);
        }
    } else {
        ESP_LOGW(TAG, "SD card not available, skipping ULog writer init");
    }

    /* ── System Performance Monitor ── */
    SystemMonitor::instance().init();
    SystemMonitor::instance().start();

    /* All setup complete — delete this task to reclaim its stack/TCB.
     * The FreeRTOS idle task will clean up. All work continues in
     * dedicated tasks (WiFi, httpd, ULog, camera, etc.). */
    vTaskDelete(NULL);
}
