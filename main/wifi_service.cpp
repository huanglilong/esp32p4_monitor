/**
 * @file wifi_service.cpp
 * @brief C++ facade over wifi_manager — unified WiFi service.
 *
 * Replaces inline WiFi code previously split across:
 *   - PhoneAppSettings::bootWifiAutoConnect() / wifiInit() / wifiEventHandler()
 *   - web_config_server.cpp settings_handler (WiFi connect verification)
 *   - main.cpp boot_sdcard_wifi_config()
 *
 * Uses wifi_manager for STA connection, auto-reconnect, and (optionally)
 * AP provisioning mode. Publishes uORB wifi_state via the state callback.
 */

#include "wifi_service.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_sntp.h"

#include "example_config.h"
#include "wifi_manager.h"
#include "generated/wifi_state.h"
#include "topics.h"

static const char *TAG = "wifi_service";

/* ── Singleton ──────────────────────────────────────────────────── */

WifiService& WifiService::instance() {
    static WifiService s;
    return s;
}

/* ── NVS helpers ─────────────────────────────────────────────────── */

bool WifiService::_read_nvs_creds(char *ssid_out, size_t ssid_size,
                                   char *pass_out, size_t pass_size) {
    nvs_handle_t h;
    ssid_out[0] = '\0';
    pass_out[0] = '\0';

    if (nvs_open(NVS_NAMESPACE_SETTINGS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t len = ssid_size;
    nvs_get_str(h, NVS_KEY_WIFI_SSID, ssid_out, &len);
    len = pass_size;
    nvs_get_str(h, NVS_KEY_WIFI_PASS, pass_out, &len);
    nvs_close(h);

    return (strlen(ssid_out) > 0);
}

void WifiService::_save_nvs_creds(const char *ssid, const char *password) {
    if (!ssid || strlen(ssid) == 0) return;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE_SETTINGS, NVS_READWRITE, &h) != ESP_OK) return;

    nvs_set_str(h, NVS_KEY_WIFI_SSID, ssid);
    if (password && strlen(password) > 0) {
        nvs_set_str(h, NVS_KEY_WIFI_PASS, password);
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi credentials saved to NVS: ssid=%s", ssid);
}

/* ── uORB wifi_state publisher ──────────────────────────────────── */

void WifiService::_publish_wifi_state(bool connected, const char *ssid, int8_t rssi) {
    /* Lazy-init uORB advertiser (thread-safe CAS loop) */
    orb_advert_t pub = _wifi_state_pub.load(std::memory_order_acquire);
    if (pub == ORB_ADVERT_INVALID) {
        orb_advert_t expected = ORB_ADVERT_INVALID;
        orb_advert_t new_pub = orb_advertise(ORB_ID(wifi_state));
        if (!_wifi_state_pub.compare_exchange_strong(expected, new_pub,
                std::memory_order_release, std::memory_order_acquire)) {
            /* Another thread already set it — ignore our unused handle */
        }
        pub = _wifi_state_pub.load(std::memory_order_acquire);
    }

    if (pub == ORB_ADVERT_INVALID) return;

    wifi_state_s ws = {};
    ws.timestamp = esp_timer_get_time();
    ws.connected = connected;
    ws.scanning = false;
    ws.rssi = rssi;
    if (ssid) {
        strlcpy(ws.ssid, ssid, sizeof(ws.ssid));
    }
    orb_publish(ORB_ID(wifi_state), pub, &ws);
}

/* ── State callback (called by wifi_manager from event handler) ── */

void WifiService::_state_callback(bool connected, void *user_ctx) {
    WifiService *self = static_cast<WifiService *>(user_ctx);
    if (!self) return;

    /* Update uORB wifi_state */
    wifi_manager_status_t st;
    wifi_manager_get_status(&st);

    int8_t rssi = 0;
    if (connected) {
        int rssi_raw = 0;
        esp_wifi_sta_get_rssi(&rssi_raw);
        rssi = (int8_t)rssi_raw;
        self->_publish_wifi_state(true, st.sta_ssid, rssi);

        /* Update mDNS delegated hostname IP now that WiFi has an address */
        shared_mdns_update_delegate_ip();
    } else {
        self->_publish_wifi_state(false, "", 0);
    }

    /* Start SNTP on first connect */
    if (connected && !self->_sntp_started.load(std::memory_order_acquire)) {
        self->set_sntp_started();
        /* Web config server task will detect this and start SNTP */
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

esp_err_t WifiService::init() {
    if (_initialized.load(std::memory_order_acquire)) return ESP_OK;

    /* Create scan mutex */
    _scan_mutex = xSemaphoreCreateMutexStatic(&_scan_mutex_buf);
    if (!_scan_mutex) return ESP_ERR_NO_MEM;

    /* Initialize wifi_manager (netif + event loop + wifi driver) */
    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_manager_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Register state callback for uORB publishing */
    wifi_manager_register_state_callback(_state_callback, this);

    _initialized.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "WiFi service initialized");
    return ESP_OK;
}

esp_err_t WifiService::start() {
    if (!_initialized.load(std::memory_order_acquire)) {
        ESP_LOGE(TAG, "WifiService not initialized — call init() first");
        return ESP_ERR_INVALID_STATE;
    }

    char ssid[33] = {};
    char pass[65] = {};
    bool has_creds = _read_nvs_creds(ssid, sizeof(ssid), pass, sizeof(pass));

    wifi_manager_config_t cfg = {};
    if (has_creds) {
        cfg.sta_ssid = ssid;
        cfg.sta_password = (strlen(pass) > 0) ? pass : nullptr;
        strlcpy(_current_ssid, ssid, sizeof(_current_ssid));
        ESP_LOGI(TAG, "Starting WiFi with STA: ssid=%s", ssid);
    } else {
        /* No stored credentials — AP-only provisioning mode.
         * Use a descriptive AP SSID prefix for this device. */
        cfg.ap_ssid_prefix = "esp-monitor";
        ESP_LOGI(TAG, "Starting WiFi in AP-only provisioning mode");
    }

    /* We don't use close_on_sta — keep AP always available for
     * WIFI6 (headless) boards to always have a provisioning path. */
    cfg.ap_behavior = "keep";

    esp_err_t err = wifi_manager_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_manager_start failed: %s", esp_err_to_name(err));
        return err;
    }

    _started.store(true, std::memory_order_release);
    return ESP_OK;
}

esp_err_t WifiService::scan(wifi_scan_result_t *results, uint16_t *out_count) {
    if (!_initialized.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    if (!results || !out_count) return ESP_ERR_INVALID_ARG;

    /* Serialize scans (single mutex, short critical section) */
    if (xSemaphoreTake(_scan_mutex, pdMS_TO_TICKS(20000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    wifi_manager_scan_record_t raw[WIFI_SERVICE_SCAN_MAX];
    uint16_t count = 0;
    esp_err_t err = wifi_manager_scan_aps(raw, WIFI_SERVICE_SCAN_MAX, &count);

    if (err == ESP_OK) {
        for (uint16_t i = 0; i < count; i++) {
            strlcpy(results[i].ssid, raw[i].ssid, sizeof(results[i].ssid));
            results[i].rssi = raw[i].rssi;
            results[i].authmode = raw[i].authmode;
        }
        *out_count = count;
    }

    xSemaphoreGive(_scan_mutex);
    return err;
}

esp_err_t WifiService::connect(const char *ssid, const char *password) {
    if (!_initialized.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    /* Use wifi_manager_apply_sta_config to hot-swap credentials */
    wifi_manager_config_t cfg = {};
    cfg.sta_ssid = ssid;
    cfg.sta_password = (password && strlen(password) > 0) ? password : nullptr;

    esp_err_t err = wifi_manager_apply_sta_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply_sta_config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Wait for connection (blocking, caller runs in a task) */
    err = wifi_manager_wait_connected(15000);
    if (err == ESP_OK) {
        _save_nvs_creds(ssid, password);
        strlcpy(_current_ssid, ssid, sizeof(_current_ssid));
        ESP_LOGI(TAG, "Connected to %s, credentials saved", ssid);
    } else {
        ESP_LOGW(TAG, "Connect to %s timed out — NOT saving to NVS", ssid);
    }

    return err;
}

esp_err_t WifiService::disconnect() {
    if (!_initialized.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;

    /* Apply empty STA config — wifi_manager will switch to AP-only provision mode */
    wifi_manager_config_t cfg = {};
    cfg.ap_behavior = "keep";
    return wifi_manager_apply_sta_config(&cfg);
}

esp_err_t WifiService::apply_sta_config(const char *ssid, const char *password) {
    if (!_initialized.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;

    wifi_manager_config_t cfg = {};
    cfg.sta_ssid = (ssid && strlen(ssid) > 0) ? ssid : nullptr;
    cfg.sta_password = (password && strlen(password) > 0) ? password : nullptr;
    /* Keep AP running for provisioning */
    cfg.ap_behavior = "keep";

    esp_err_t err = wifi_manager_apply_sta_config(&cfg);
    if (err == ESP_OK) {
        if (ssid && strlen(ssid) > 0) {
            _save_nvs_creds(ssid, password);
            strlcpy(_current_ssid, ssid, sizeof(_current_ssid));
        }
    }
    return err;
}

void WifiService::get_status(wifi_service_status_t *status) {
    if (!status) return;

    wifi_manager_status_t st;
    wifi_manager_get_status(&st);

    status->sta_connected = st.sta_connected;
    status->sta_configured = st.sta_configured;
    strlcpy(status->sta_ip, st.sta_ip, sizeof(status->sta_ip));
    strlcpy(status->sta_ssid, st.sta_ssid ? st.sta_ssid : _current_ssid, sizeof(status->sta_ssid));
    status->sta_rssi = 0;
    if (st.sta_connected) {
        int rssi_raw = 0;
        esp_wifi_sta_get_rssi(&rssi_raw);
        status->sta_rssi = (int8_t)rssi_raw;
    }
}

esp_err_t WifiService::wait_connected(uint32_t timeout_ms) {
    return wifi_manager_wait_connected(timeout_ms);
}

void *WifiService::get_ap_netif() {
    return wifi_manager_get_ap_netif();
}
