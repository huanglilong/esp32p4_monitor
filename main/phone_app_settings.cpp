#include "phone_app_settings.hpp"
#include "peripherals.hpp"
#include "private/esp_brookesia_utils.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "example_config.h"
#include "esp_sntp.h"
#include <string.h>
#include <stdio.h>
#include <atomic>

/* uORB */
#include "uorb.h"
#include "topics.h"

static const char *TAG = "Settings";

extern const lv_image_dsc_t esp_brookesia_image_large_app_launcher_default_112_112;

/* Static WiFi state — persists across Settings app open/close cycles */
TaskHandle_t PhoneAppSettings::_wifi_scan_task = nullptr;
EventGroupHandle_t PhoneAppSettings::_wifi_event_group = nullptr;
bool PhoneAppSettings::_wifi_initialized = false;
TimerHandle_t PhoneAppSettings::_wifi_reconnect_timer = nullptr;
uint32_t PhoneAppSettings::_wifi_reconnect_count = 0;
esp_event_handler_instance_t PhoneAppSettings::_wifi_handler_inst = nullptr;
esp_event_handler_instance_t PhoneAppSettings::_ip_handler_inst = nullptr;

/* Thread-safe WiFi state uORB publisher.
 * wifiEventHandler runs on the system event task and can fire concurrently
 * (e.g., rapid connect/disconnect). Use std::atomic to prevent double
 * orb_advertise() from racing on the lazy-init check. */
static std::atomic<orb_advert_t> s_wifi_pub{ORB_ADVERT_INVALID};

static void publish_wifi_state(bool connected, bool scanning, int8_t rssi, const char *ssid)
{
    orb_advert_t pub = s_wifi_pub.load();
    if (pub < 0) {
        orb_advert_t new_pub = orb_advertise(ORB_ID(wifi_state));
        /* CAS: if another thread already advertised, discard our handle */
        if (!s_wifi_pub.compare_exchange_strong(pub, new_pub)) {
            /* Another thread won the race; new_pub is our unused handle.
             * uORB handles are global and don't need explicit free. */
        } else {
            pub = new_pub;
        }
    } else {
        pub = s_wifi_pub.load();
    }
    if (pub >= 0) {
        struct wifi_state_s ws = {};
        ws.timestamp  = esp_timer_get_time();
        ws.connected  = connected;
        ws.scanning   = scanning;
        ws.rssi       = rssi;
        if (ssid) {
            strncpy(ws.ssid, ssid, sizeof(ws.ssid) - 1);
            ws.ssid[sizeof(ws.ssid) - 1] = '\0';
        }
        orb_publish(ORB_ID(wifi_state), pub, &ws);
    }
}

/* NVS keys now defined in example_config.h (NVS_NAMESPACE_SETTINGS, NVS_KEY_*) */
/* Backward-compatible local aliases for brevity in this file */
#define NVS_NAMESPACE            NVS_NAMESPACE_SETTINGS
#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_INIT_DONE_BIT      BIT1
/*============================================================================
 * Constructor / Destructor
 *============================================================================*/
PhoneAppSettings::PhoneAppSettings(bool use_status_bar, bool use_navigation_bar) :
    ESP_Brookesia_PhoneApp("Settings", &esp_brookesia_image_large_app_launcher_default_112_112,
                           true, use_status_bar, use_navigation_bar),
    _nvs_dirty(false), _nvs_save_timer(nullptr),
    _status_timer(nullptr),
    _screen_index(SCREEN_MAIN), _is_ui_del(true),
    _nvs{0, VOLUME_DEFAULT, BRIGHTNESS_DEFAULT},
    _scr_main(nullptr),

    _sw_wifi(nullptr), _label_wifi(nullptr),

    _slider_vol(nullptr), _label_vol(nullptr),
    _slider_brightness(nullptr), _label_brightness(nullptr),

    _scr_wifi_list(nullptr), _list_wifi(nullptr), _spinner_wifi(nullptr),
    _scr_wifi_pass(nullptr), _label_pass_ssid(nullptr),

    _wifi_scanning(false),
    _wifi_connecting(false)
{
    memset(_wifi_ssid, 0, sizeof(_wifi_ssid));
    memset(_wifi_password, 0, sizeof(_wifi_password));
    memset(_wifi_ip, 0, sizeof(_wifi_ip));
    _wifi_rssi = -100;
}

PhoneAppSettings::~PhoneAppSettings()
{
    /* Flush pending NVS writes — use direct NVS access (safe in destructor,
     * unlike setNvsParam() which may have side effects on member state) */
    if (_nvs_save_timer) {
        lv_timer_delete(_nvs_save_timer);
        _nvs_save_timer = nullptr;
    }
    if (_nvs_dirty) {
        _nvs_dirty = false;
        nvs_handle_t nvs_h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_h) == ESP_OK) {
            nvs_set_i32(nvs_h, NVS_KEY_VOLUME, _nvs.volume);
            nvs_set_i32(nvs_h, NVS_KEY_BRIGHTNESS, _nvs.brightness);
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
        }
    }

    /* Delete status refresh timer */
    if (_status_timer) {
        lv_timer_delete(_status_timer);
        _status_timer = nullptr;
    }

    /* Delete WiFi reconnect timer (defensive) */
    if (_wifi_reconnect_timer) {
        xTimerDelete(_wifi_reconnect_timer, 0);
        _wifi_reconnect_timer = nullptr;
    }

    /* Unregister WiFi event handlers if they were registered */
    if (_wifi_handler_inst) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, _wifi_handler_inst);
        _wifi_handler_inst = nullptr;
    }
    if (_ip_handler_inst) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, _ip_handler_inst);
        _ip_handler_inst = nullptr;
    }

    /* Stop WiFi scan task and delete event group.
     * Note: close() intentionally keeps WiFi alive when connected (persistent
     * connection), but in the destructor we must clean up everything — the
     * event handlers are already unregistered above, so callbacks would crash
     * with a dangling app pointer. Disconnect first, then free resources. */
    if (_wifi_scan_task) {
        vTaskDelete(_wifi_scan_task);
        _wifi_scan_task = nullptr;
    }
    if (_wifi_event_group) {
        vEventGroupDelete(_wifi_event_group);
        _wifi_event_group = nullptr;
    }
    _is_ui_del = true;
}

/*============================================================================
 * Boot-time WiFi Auto-Connect
 *============================================================================*/
void PhoneAppSettings::bootWifiAutoConnect(void)
{
    nvs_handle_t nvs_h;
    if (nvs_open(NVS_NAMESPACE_SETTINGS, NVS_READONLY, &nvs_h) != ESP_OK) {
        return;
    }

    int32_t wifi_en = 0;
    nvs_get_i32(nvs_h, NVS_KEY_WIFI_EN, &wifi_en);
    if (!wifi_en) {
        nvs_close(nvs_h);
        return;
    }

    char ssid[33] = {};
    char pass[65] = {};
    size_t len;
    len = sizeof(ssid);
    nvs_get_str(nvs_h, NVS_KEY_WIFI_SSID, ssid, &len);
    len = sizeof(pass);
    nvs_get_str(nvs_h, NVS_KEY_WIFI_PASS, pass, &len);
    nvs_close(nvs_h);

    if (strlen(ssid) == 0) {
        ESP_LOGI(TAG, "WiFi enabled in NVS but no SSID stored");
        return;
    }

    ESP_LOGI(TAG, "Boot WiFi: connecting to %s...", ssid);

    /* One-time netif + event loop + wifi driver init */
    if (!_wifi_initialized) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
        assert(sta_netif);

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&cfg) != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_init failed, boot WiFi deferred");
            return;
        }

        /* Register event handler for WiFi/IP events.
         * Save instance handles to allow proper unregister if needed. */
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID,
            wifiEventHandler, nullptr, &_wifi_handler_inst));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP,
            wifiEventHandler, nullptr, &_ip_handler_inst));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        _wifi_initialized = true;
    }

    /* Create event group for tracking connection state */
    if (_wifi_event_group == nullptr) {
        _wifi_event_group = xEventGroupCreate();
    }

    esp_wifi_start();

    /* Set credentials and connect */
    wifi_config_t wifi_cfg = {};
    size_t slen = strlen(ssid);
    if (slen >= sizeof(wifi_cfg.sta.ssid)) slen = sizeof(wifi_cfg.sta.ssid) - 1;
    memcpy(wifi_cfg.sta.ssid, ssid, slen);
    slen = strlen(pass);
    if (slen >= sizeof(wifi_cfg.sta.password)) slen = sizeof(wifi_cfg.sta.password) - 1;
    memcpy(wifi_cfg.sta.password, pass, slen);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_connect();
    ESP_LOGI(TAG, "Boot WiFi: connection attempt started");
}

/*============================================================================
 * App Lifecycle
 *============================================================================*/
bool PhoneAppSettings::init(void)
{
    loadNvsParam();
    getNvsStr(NVS_KEY_WIFI_SSID, _wifi_ssid, sizeof(_wifi_ssid));
    getNvsStr(NVS_KEY_WIFI_PASS, _wifi_password, sizeof(_wifi_password));

    applySettings();
    return true;
}

bool PhoneAppSettings::run(void)
{
    _is_ui_del = false;
    createMainScreen();
    createWifiListScreen();
    createWifiPasswordScreen();
    lv_scr_load(_scr_main);

    /* Start NVS commit debounce timer (500ms) to avoid flash wear from rapid slider events */
    _nvs_save_timer = lv_timer_create(_nvs_save_timer_cb, 500, this);

    /* Auto-start WiFi scan task if WiFi was enabled from NVS.
     * If task already running (kept alive from previous session by close()),
     * reuse it — don't recreate. */
    if (_nvs.wifi_en && _wifi_scan_task == NULL) {
        _wifi_event_group = xEventGroupCreate();
        if (_wifi_event_group) {
            BaseType_t ret = xTaskCreate(wifiScanTaskHandler, "wifi_scan", TASK_STACK_WIFI_SCAN,
                                         this, TASK_PRIO_WIFI_SCAN, &_wifi_scan_task);
            if (ret != pdPASS) {
                ESP_LOGE(TAG, "Failed to create WiFi scan task on app run");
                vEventGroupDelete(_wifi_event_group);
                _wifi_event_group = nullptr;
            }
        } else {
            ESP_LOGE(TAG, "Failed to create WiFi event group on app run");
        }
    } else if (_wifi_scan_task) {
        ESP_LOGI(TAG, "Reusing existing WiFi background task");
    }

    /* WiFi/Volume/Brightness status refresh timer: update every 1s */
    _status_timer = lv_timer_create([](lv_timer_t *t) {
        PhoneAppSettings *app = (PhoneAppSettings *)t->user_data;
        if (!app || app->_is_ui_del || app->_screen_index != SCREEN_MAIN) return;

        /* WiFi status */
        if (app->_label_wifi) {
            bool wifi_on = app->_nvs.wifi_en != 0;
            if (wifi_on) {
                if (app->_wifi_event_group &&
                    (xEventGroupGetBits(app->_wifi_event_group) & WIFI_CONNECTED_BIT)) {
                    esp_wifi_sta_get_rssi(&app->_wifi_rssi);
                    const char *sig = (app->_wifi_rssi > -60) ? "***" : (app->_wifi_rssi > -80) ? "** " : "*  ";
                    lv_label_set_text_fmt(app->_label_wifi, "Wi-Fi  %s  %s", sig, app->_wifi_ip);
                } else {
                    lv_label_set_text(app->_label_wifi, "Wi-Fi (connecting...)");
                }
            } else {
                lv_label_set_text(app->_label_wifi, "Wi-Fi");
            }
        }

        /* Volume */
        if (app->_label_vol) {
            int32_t vol = app->_nvs.volume;
            char buf[32];
            snprintf(buf, sizeof(buf), "Volume: %ld", vol);
            lv_label_set_text(app->_label_vol, buf);
        }

        /* Brightness */
        if (app->_label_brightness) {
            int32_t bri = app->_nvs.brightness;
            char buf[32];
            snprintf(buf, sizeof(buf), "Brightness: %ld", bri);
            lv_label_set_text(app->_label_brightness, buf);
        }
    }, 1000, this);

    return true;
}

bool PhoneAppSettings::back(void)
{
    if (_screen_index == SCREEN_WIFI_PASSWORD) {
        _screen_index = SCREEN_WIFI_LIST;
        lv_scr_load(_scr_wifi_list);
    } else if (_screen_index == SCREEN_WIFI_LIST) {

        stopWifiScan();

        _screen_index = SCREEN_MAIN;
        lv_scr_load(_scr_main);
    } else {

        stopWifiScan();

        notifyCoreClosed();
    }
    return true;
}

bool PhoneAppSettings::close(void)
{
    stopWifiScan();

    /* Flush pending NVS writes before cleaning up */
    if (_nvs_save_timer) {
        lv_timer_delete(_nvs_save_timer);
        _nvs_save_timer = nullptr;
    }
    if (_status_timer) {
        lv_timer_delete(_status_timer);
        _status_timer = nullptr;
    }
    if (_nvs_dirty) {
        _nvs_dirty = false;
        setNvsParam(NVS_KEY_VOLUME, _nvs.volume);
        setNvsParam(NVS_KEY_BRIGHTNESS, _nvs.brightness);
    }

    _is_ui_del = true;

    /* Null out LVGL pointers — widgets may be freed by framework.
     * WiFi task and event group: keep alive if connected (persistent connection),
     * clean up only if WiFi is off or disconnected. */
    _scr_main = nullptr;
    _scr_wifi_list = nullptr;
    _list_wifi = nullptr;
    _spinner_wifi = nullptr;
    _scr_wifi_pass = nullptr;
    _spinner_connect = nullptr;
    _label_connect_status = nullptr;

    bool wifi_connected = _wifi_event_group &&
        (xEventGroupGetBits(_wifi_event_group) & WIFI_CONNECTED_BIT);
    if (!wifi_connected) {
        /* Unregister event handlers BEFORE deleting the event group to prevent
         * use-after-free: wifiEventHandler checks _wifi_event_group but a
         * TOCTOU race exists between the null-check and xEventGroupSetBits. */
        if (_wifi_handler_inst) {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, _wifi_handler_inst);
            _wifi_handler_inst = nullptr;
        }
        if (_ip_handler_inst) {
            esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, _ip_handler_inst);
            _ip_handler_inst = nullptr;
        }
        if (_wifi_scan_task) {
            vTaskDelete(_wifi_scan_task);
            _wifi_scan_task = nullptr;
        }
        if (_wifi_event_group) {
            vEventGroupDelete(_wifi_event_group);
            _wifi_event_group = nullptr;
        }
    } else {
        ESP_LOGI(TAG, "WiFi connected, keeping background task alive");
    }

    return true;
}

/*============================================================================
 * NVS Methods
 *============================================================================*/
bool PhoneAppSettings::loadNvsParam(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) { ESP_LOGW(TAG, "NVS open failed, using defaults"); return false; }
    /* Load each parameter; write default if not found in NVS */
    struct { const char *key; int32_t *val; } params[] = {
        { NVS_KEY_WIFI_EN,    &_nvs.wifi_en },
        { NVS_KEY_VOLUME,     &_nvs.volume },
        { NVS_KEY_BRIGHTNESS, &_nvs.brightness },
    };
    for (auto &p : params) {
        err = nvs_get_i32(nvs_handle, p.key, p.val);
        if (err == ESP_OK) ESP_LOGI(TAG, "Loaded %s = %ld", p.key, (long)*p.val);
        else if (err == ESP_ERR_NVS_NOT_FOUND) nvs_set_i32(nvs_handle, p.key, *p.val);
    }
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return true;
}

bool PhoneAppSettings::setNvsParam(const char *key, int32_t value)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) return false;
    nvs_set_i32(nvs_handle, key, value);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return true;
}

bool PhoneAppSettings::setNvsStr(const char *key, const char *value)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) return false;
    nvs_set_str(nvs_handle, key, value);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return true;
}

bool PhoneAppSettings::getNvsStr(const char *key, char *out, size_t max_len)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) != ESP_OK) return false;
    size_t len = max_len;
    esp_err_t err = nvs_get_str(nvs_handle, key, out, &len);
    nvs_close(nvs_handle);
    return (err == ESP_OK);
}

void PhoneAppSettings::applySettings(void)
{
    int32_t vol = _nvs.volume;
    if (vol < VOLUME_MIN) vol = VOLUME_MIN;
    if (vol > VOLUME_MAX) vol = VOLUME_MAX;
    _nvs.volume = vol;
    PeripheralManager::instance().set_volume((int)vol);

    int32_t bri = _nvs.brightness;
    if (bri < BRIGHTNESS_MIN) bri = BRIGHTNESS_MIN;
    if (bri > BRIGHTNESS_MAX) bri = BRIGHTNESS_MAX;
    _nvs.brightness = bri;
    bsp_display_brightness_set((int)bri);

    ESP_LOGI(TAG, "Settings applied: vol=%ld, brightness=%ld", vol, bri);
}

/*============================================================================
 * UI Helpers
 *============================================================================*/
static lv_obj_t *create_back_button(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 50, 50);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, LV_SYMBOL_LEFT);
    lv_obj_center(label);
    return btn;
}

static lv_obj_t *create_screen_title(lv_obj_t *parent, const char *title)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x000000), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 15);
    return label;
}

/*============================================================================
 * UI - Main Screen
 *============================================================================*/
void PhoneAppSettings::createMainScreen(void)
{
    _scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr_main, lv_color_hex(0xFFFFFF), 0);
    create_screen_title(_scr_main, "Settings");

    lv_obj_t *btn_back = create_back_button(_scr_main);
    lv_obj_add_event_cb(btn_back, onBackClicked, LV_EVENT_CLICKED, this);
    /* --- WiFi row --- */
    lv_obj_t *cont_wifi = lv_obj_create(_scr_main);
    lv_obj_set_size(cont_wifi, 620, 60);
    lv_obj_align(cont_wifi, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_border_width(cont_wifi, 0, 0);
    lv_obj_set_style_bg_opa(cont_wifi, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(cont_wifi, lv_color_hex(0xE0E0E0), 0);
    /* Tap the row to enter WiFi list */
    lv_obj_add_flag(cont_wifi, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cont_wifi, onWifiRowClicked, LV_EVENT_CLICKED, this);

    lv_obj_t *icon_wifi = lv_label_create(cont_wifi);
    lv_label_set_text(icon_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(icon_wifi, LV_ALIGN_LEFT_MID, 15, 0);

    _label_wifi = lv_label_create(cont_wifi);
    lv_label_set_text(_label_wifi, "Wi-Fi");
    lv_obj_set_style_text_color(_label_wifi, lv_color_hex(0x000000), 0);
    lv_obj_align(_label_wifi, LV_ALIGN_LEFT_MID, 55, 0);

    _sw_wifi = lv_switch_create(cont_wifi);
    lv_obj_align(_sw_wifi, LV_ALIGN_RIGHT_MID, -15, 0);
    lv_obj_add_event_cb(_sw_wifi, onWifiSwitchChanged, LV_EVENT_VALUE_CHANGED, this);
    /* --- Volume row --- */
    lv_obj_t *cont_vol = lv_obj_create(_scr_main);
    lv_obj_set_size(cont_vol, 620, 90);

    lv_obj_align(cont_vol, LV_ALIGN_TOP_MID, 0, 145);
    lv_obj_set_style_border_width(cont_vol, 0, 0);
    lv_obj_set_style_bg_opa(cont_vol, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(cont_vol, lv_color_hex(0xE0E0E0), 0);

    lv_obj_t *icon_vol = lv_label_create(cont_vol);
    lv_label_set_text(icon_vol, LV_SYMBOL_VOLUME_MID);
    lv_obj_align(icon_vol, LV_ALIGN_TOP_LEFT, 15, 10);

    _label_vol = lv_label_create(cont_vol);
    lv_label_set_text(_label_vol, "Volume: 60");
    lv_obj_set_style_text_color(_label_vol, lv_color_hex(0x000000), 0);
    lv_obj_align(_label_vol, LV_ALIGN_TOP_LEFT, 55, 10);

    _slider_vol = lv_slider_create(cont_vol);
    lv_obj_set_size(_slider_vol, 580, 10);
    lv_obj_align(_slider_vol, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_slider_set_range(_slider_vol, VOLUME_MIN, VOLUME_MAX);
    lv_obj_add_event_cb(_slider_vol, onVolumeSliderChanged, LV_EVENT_VALUE_CHANGED, this);

    /* --- Brightness row --- */
    lv_obj_t *cont_bri = lv_obj_create(_scr_main);
    lv_obj_set_size(cont_bri, 620, 90);

    lv_obj_align(cont_bri, LV_ALIGN_TOP_MID, 0, 250);
    lv_obj_set_style_border_width(cont_bri, 0, 0);
    lv_obj_set_style_bg_opa(cont_bri, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(cont_bri, lv_color_hex(0xE0E0E0), 0);

    lv_obj_t *icon_bri = lv_label_create(cont_bri);
    lv_label_set_text(icon_bri, LV_SYMBOL_SETTINGS);
    lv_obj_align(icon_bri, LV_ALIGN_TOP_LEFT, 15, 10);

    _label_brightness = lv_label_create(cont_bri);
    lv_label_set_text(_label_brightness, "Brightness: 80");
    lv_obj_set_style_text_color(_label_brightness, lv_color_hex(0x000000), 0);
    lv_obj_align(_label_brightness, LV_ALIGN_TOP_LEFT, 55, 10);

    _slider_brightness = lv_slider_create(cont_bri);
    lv_obj_set_size(_slider_brightness, 580, 10);
    lv_obj_align(_slider_brightness, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_slider_set_range(_slider_brightness, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
    lv_obj_add_event_cb(_slider_brightness, onBrightnessSliderChanged, LV_EVENT_VALUE_CHANGED, this);

    updateMainScreenFromNvs();
}

void PhoneAppSettings::updateMainScreenFromNvs(void)
{
    int32_t vol = _nvs.volume;
    int32_t bri = _nvs.brightness;

    int32_t wifi_en = _nvs.wifi_en;
    if (wifi_en) {
        lv_obj_add_state(_sw_wifi, LV_STATE_CHECKED);
        /* Show connection status in WiFi row */
        if (strlen(_wifi_ip) > 0) {
            const char *sig = (_wifi_rssi > -60) ? "***" : (_wifi_rssi > -80) ? "** " : "*  ";
            lv_label_set_text_fmt(_label_wifi, "Wi-Fi  %s  %s", sig, _wifi_ip);
        } else {
            lv_label_set_text(_label_wifi, "Wi-Fi (connecting...)");
        }
    } else {
        lv_obj_clear_state(_sw_wifi, LV_STATE_CHECKED);
        lv_label_set_text(_label_wifi, "Wi-Fi");
    }
    lv_slider_set_value(_slider_vol, (int32_t)vol, LV_ANIM_OFF);
    char buf[32];
    snprintf(buf, sizeof(buf), "Volume: %ld", vol);
    lv_label_set_text(_label_vol, buf);

    lv_slider_set_value(_slider_brightness, (int32_t)bri, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "Brightness: %ld", bri);
    lv_label_set_text(_label_brightness, buf);
}

/*============================================================================
 * UI - WiFi Screens
 *============================================================================*/
void PhoneAppSettings::createWifiListScreen(void)
{

    _scr_wifi_list = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr_wifi_list, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_user_data(_scr_wifi_list, this);
    create_screen_title(_scr_wifi_list, "Wi-Fi");

    lv_obj_t *btn_back = create_back_button(_scr_wifi_list);
    lv_obj_add_event_cb(btn_back, onBackClicked, LV_EVENT_CLICKED, this);

    _list_wifi = lv_obj_create(_scr_wifi_list);
    lv_obj_set_size(_list_wifi, 660, 580);
    lv_obj_align(_list_wifi, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_border_width(_list_wifi, 0, 0);
    lv_obj_set_style_bg_opa(_list_wifi, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(_list_wifi, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(_list_wifi, LV_DIR_VER);

    _spinner_wifi = lv_spinner_create(_scr_wifi_list);
    lv_obj_set_size(_spinner_wifi, 60, 60);
    lv_obj_align(_spinner_wifi, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_flag(_spinner_wifi, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(_scr_wifi_list, onWifiListScreenLoaded, LV_EVENT_SCREEN_LOADED, this);

}

void PhoneAppSettings::createWifiPasswordScreen(void)
{

    _scr_wifi_pass = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr_wifi_pass, lv_color_hex(0xFFFFFF), 0);
    create_screen_title(_scr_wifi_pass, "Connect");

    lv_obj_t *btn_back = create_back_button(_scr_wifi_pass);
    lv_obj_add_event_cb(btn_back, onBackClicked, LV_EVENT_CLICKED, this);

    _label_pass_ssid = lv_label_create(_scr_wifi_pass);
    lv_obj_set_style_text_font(_label_pass_ssid, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_label_pass_ssid, lv_color_hex(0x000000), 0);
    lv_obj_align(_label_pass_ssid, LV_ALIGN_TOP_MID, 0, 80);
    lv_label_set_text(_label_pass_ssid, "SSID: --");

    _ta_password = lv_textarea_create(_scr_wifi_pass);
    lv_obj_set_size(_ta_password, 500, 50);
    lv_obj_align(_ta_password, LV_ALIGN_TOP_MID, 0, 130);
    lv_textarea_set_placeholder_text(_ta_password, "Password");
    lv_textarea_set_password_mode(_ta_password, true);
    lv_textarea_set_one_line(_ta_password, true);

    _kb_password = lv_keyboard_create(_scr_wifi_pass);
    lv_keyboard_set_textarea(_kb_password, _ta_password);
    lv_obj_add_event_cb(_kb_password, onKeyboardEnterClicked, LV_EVENT_READY, this);

    _spinner_connect = lv_spinner_create(_scr_wifi_pass);
    lv_obj_set_size(_spinner_connect, 60, 60);
    lv_obj_align(_spinner_connect, LV_ALIGN_BOTTOM_MID, 0, -150);
    lv_obj_add_flag(_spinner_connect, LV_OBJ_FLAG_HIDDEN);

    _label_connect_status = lv_label_create(_scr_wifi_pass);
    lv_obj_set_style_text_font(_label_connect_status, &lv_font_montserrat_20, 0);
    lv_obj_align(_label_connect_status, LV_ALIGN_BOTTOM_MID, 0, -80);
    lv_obj_add_flag(_label_connect_status, LV_OBJ_FLAG_HIDDEN);

}

/*============================================================================
 * Callbacks - Volume & Brightness (always enabled)
 *============================================================================*/
void PhoneAppSettings::onBackClicked(lv_event_t *e)
{
    PhoneAppSettings *app = (PhoneAppSettings *)lv_event_get_user_data(e);
    if (app) app->back();
}

void PhoneAppSettings::onVolumeSliderChanged(lv_event_t *e)
{
    PhoneAppSettings *app = (PhoneAppSettings *)lv_event_get_user_data(e);
    if (!app) return;
    int32_t vol = (int32_t)lv_slider_get_value(app->_slider_vol);
    if (vol != app->_nvs.volume) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Volume: %ld", vol);
        lv_label_set_text(app->_label_vol, buf);
        app->_nvs.volume = vol;
        app->_nvs_dirty = true;  // Defer NVS write (500ms debounce)
        PeripheralManager::instance().set_volume((int)vol);
    }
}

void PhoneAppSettings::onBrightnessSliderChanged(lv_event_t *e)
{
    PhoneAppSettings *app = (PhoneAppSettings *)lv_event_get_user_data(e);
    if (!app) return;
    int32_t bri = (int32_t)lv_slider_get_value(app->_slider_brightness);
    if (bri != app->_nvs.brightness) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Brightness: %ld", bri);
        lv_label_set_text(app->_label_brightness, buf);
        app->_nvs.brightness = bri;
        app->_nvs_dirty = true;  // Defer NVS write (500ms debounce)
        if (bsp_display_brightness_set((int)bri) != ESP_OK) {
            lv_slider_set_value(app->_slider_brightness, app->_nvs.brightness, LV_ANIM_OFF);
        }
    }
}

void PhoneAppSettings::onMainScreenLoaded(lv_event_t *e)
{
    PhoneAppSettings *app = (PhoneAppSettings *)lv_event_get_user_data(e);
    if (!app) return;
    app->_screen_index = SCREEN_MAIN;
    app->updateMainScreenFromNvs();
}

/*============================================================================
 * NVS Save Debounce Timer (500ms) — avoids flash wear from rapid slider events
 *============================================================================*/
void PhoneAppSettings::_nvs_save_timer_cb(lv_timer_t *timer)
{
    PhoneAppSettings *app = (PhoneAppSettings *)timer->user_data;
    if (!app || !app->_nvs_dirty) return;
    app->_nvs_dirty = false;
    /* Batch both writes in a single NVS open/close session
     * (avoids 2x flash access overhead from separate setNvsParam calls). */
    nvs_handle_t nvs_h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_h) == ESP_OK) {
        nvs_set_i32(nvs_h, NVS_KEY_VOLUME, app->_nvs.volume);
        nvs_set_i32(nvs_h, NVS_KEY_BRIGHTNESS, app->_nvs.brightness);
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }
}

/*============================================================================
 * WiFi Methods (CONFIG_ESP_WIFI_ENABLED only)
 *============================================================================*/
int PhoneAppSettings::getWifiSignalLevel(int rssi)
{
    if (rssi > -60)  return 3;
    if (rssi > -80)  return 2;
    if (rssi > -100) return 1;
    return 0;
}

void PhoneAppSettings::startWifiScan(void)
{
    if (_wifi_scanning) return;
    if (_is_ui_del || !_list_wifi || !_spinner_wifi) return;
    _wifi_scanning = true;
    if (bsp_display_lock(0)) {
        lv_obj_clear_flag(_spinner_wifi, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(_list_wifi);
        bsp_display_unlock();
    }
}

void PhoneAppSettings::stopWifiScan(void)
{
    if (!_wifi_scanning) return;
    _wifi_scanning = false;
    if (!_is_ui_del && _spinner_wifi) {
        if (bsp_display_lock(0)) {
            lv_obj_add_flag(_spinner_wifi, LV_OBJ_FLAG_HIDDEN);
            bsp_display_unlock();
        }
    }
}

void PhoneAppSettings::scanWifiAndUpdateUi(void)
{
    if (_is_ui_del || !_list_wifi) return;

    uint16_t ap_count = 0;
    wifi_ap_record_t ap_info[WIFI_SCAN_MAX];
    memset(ap_info, 0, sizeof(ap_info));

    esp_wifi_scan_start(NULL, true);
    esp_wifi_scan_get_ap_num(&ap_count);
    uint16_t num = (ap_count < WIFI_SCAN_MAX) ? ap_count : WIFI_SCAN_MAX;
    esp_wifi_scan_get_ap_records(&num, ap_info);

    if (!bsp_display_lock(0)) return;  // LVGL rendering in progress, skip this cycle
    lv_obj_clean(_list_wifi);

    for (int i = 0; i < num; i++) {
        lv_obj_t *item = lv_btn_create(_list_wifi);
        lv_obj_set_size(item, 620, 55);
        lv_obj_set_style_bg_color(item, lv_color_hex(0xF0F0F0), 0);
        lv_obj_set_style_bg_color(item, lv_color_hex(0xD0D0D0), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(item, 0, 0);

        lv_obj_t *ssid_label = lv_label_create(item);
        lv_label_set_text_fmt(ssid_label, "%.32s", ap_info[i].ssid);
        lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(ssid_label, lv_color_hex(0x000000), 0);
        lv_obj_align(ssid_label, LV_ALIGN_LEFT_MID, 15, 0);

        if (ap_info[i].authmode != WIFI_AUTH_OPEN && ap_info[i].authmode != WIFI_AUTH_OWE) {
            lv_obj_t *lock = lv_label_create(item);
            lv_label_set_text(lock, "[S]");
            lv_obj_set_style_text_color(lock, lv_color_hex(0xFFC107), 0);
            lv_obj_align(lock, LV_ALIGN_RIGHT_MID, -80, 0);
        }

        int level = getWifiSignalLevel(ap_info[i].rssi);
        const char *sig = (level == 3) ? "***" : (level == 2) ? "** " : (level == 1) ? "*  " : "   ";
        lv_obj_t *sig_lbl = lv_label_create(item);
        lv_label_set_text(sig_lbl, sig);
        lv_obj_set_style_text_color(sig_lbl, lv_color_hex(0x4CAF50), 0);
        lv_obj_align(sig_lbl, LV_ALIGN_RIGHT_MID, -15, 0);

        lv_obj_add_event_cb(item, onWifiItemClicked, LV_EVENT_CLICKED, nullptr);
    }
    bsp_display_unlock();
}

void PhoneAppSettings::processWifiConnect(WifiConnectState state)
{
    if (_is_ui_del || !_spinner_connect || !_label_connect_status) return;

    if (!bsp_display_lock(0)) return;  // LVGL rendering in progress, skip
    switch (state) {
    case WIFI_CONNECT_RUNNING:
        lv_obj_clear_flag(_spinner_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_label_connect_status, LV_OBJ_FLAG_HIDDEN);
        break;
    case WIFI_CONNECT_SUCCESS:
        lv_obj_add_flag(_spinner_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_label_connect_status, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(_label_connect_status, "Connected!");
        lv_obj_set_style_text_color(_label_connect_status, lv_color_hex(0x4CAF50), 0);
        break;
    case WIFI_CONNECT_FAIL:
        lv_obj_add_flag(_spinner_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_label_connect_status, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(_label_connect_status, "Connection failed");
        lv_obj_set_style_text_color(_label_connect_status, lv_color_hex(0xF44336), 0);
        break;
    default:
        lv_obj_add_flag(_spinner_connect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_label_connect_status, LV_OBJ_FLAG_HIDDEN);
        break;
    }
    bsp_display_unlock();
}

esp_err_t PhoneAppSettings::wifiInit(void)
{
    if (_wifi_event_group == NULL) {
        _wifi_event_group = xEventGroupCreate();
    }
    if (!_wifi_event_group) return ESP_ERR_NO_MEM;

    /* One-time init: netif + event loop + wifi driver */
    if (!_wifi_initialized) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_t *sta = esp_netif_create_default_wifi_sta();
        assert(sta);

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t ret = esp_wifi_init(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s (0x%x). WiFi may not be available on this hardware.",
                     esp_err_to_name(ret), ret);
            return ret;
        }

        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, this, &_wifi_handler_inst));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, this, &_ip_handler_inst));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        _wifi_initialized = true;
    }

    esp_wifi_start();

    if (strlen(_wifi_ssid) > 0) {
        wifi_config_t wifi_cfg = {};
        size_t len = strlen(_wifi_ssid);
        if (len >= sizeof(wifi_cfg.sta.ssid)) len = sizeof(wifi_cfg.sta.ssid) - 1;
        memcpy(wifi_cfg.sta.ssid, _wifi_ssid, len);
        wifi_cfg.sta.ssid[len] = '\0';
        len = strlen(_wifi_password);
        if (len >= sizeof(wifi_cfg.sta.password)) len = sizeof(wifi_cfg.sta.password) - 1;
        memcpy(wifi_cfg.sta.password, _wifi_password, len);
        wifi_cfg.sta.password[len] = '\0';
        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        esp_wifi_connect();
    }
    return ESP_OK;
}

void PhoneAppSettings::wifiScanTaskHandler(void *arg)
{
    PhoneAppSettings *app = (PhoneAppSettings *)arg;
    esp_err_t ret = app->wifiInit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed, scan task exiting");
        app->_wifi_scan_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    xEventGroupSetBits(app->_wifi_event_group, WIFI_INIT_DONE_BIT);
    /* Run one WiFi scan on entering the list screen, then stop — no continuous scanning */
    while (1) {
        if (app->_wifi_scanning && !app->_is_ui_del) {
            app->scanWifiAndUpdateUi();
            app->_wifi_scanning = false;  // Single scan, stop afterwards
        }
        if (xEventGroupGetBits(app->_wifi_event_group) & WIFI_CONNECTED_BIT) {
            esp_wifi_sta_get_rssi(&app->_wifi_rssi);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void PhoneAppSettings::wifiConnectTaskHandler(void *arg)
{
    PhoneAppSettings *app = (PhoneAppSettings *)arg;
    wifi_config_t wifi_cfg = {};

    /* Copy SSID and password from LVGL UI under lock to avoid race with rendering */
    char ssid_buf[64] = {};
    char pass_buf[64] = {};
    bsp_display_lock(portMAX_DELAY);
    const char *ssid_label = lv_label_get_text(app->_label_pass_ssid);
    const char *pass_text = lv_textarea_get_text(app->_ta_password);
    if (ssid_label) {
        strncpy(ssid_buf, ssid_label, sizeof(ssid_buf) - 1);
    }
    if (pass_text) {
        strncpy(pass_buf, pass_text, sizeof(pass_buf) - 1);
    }
    bsp_display_unlock();

    const char *ssid = ssid_buf;
    const char *pass = pass_buf;
    if (strncmp(ssid, "SSID: ", 6) == 0) ssid += 6;

    /* Guard: skip if SSID is empty */
    if (strlen(ssid) == 0) {
        ESP_LOGW(TAG, "WiFi SSID is empty — skipping connect");
        app->_wifi_connecting = false;
        vTaskDelete(NULL);
        return;
    }

    size_t slen = strlen(ssid);
    if (slen >= sizeof(wifi_cfg.sta.ssid)) slen = sizeof(wifi_cfg.sta.ssid) - 1;
    memcpy(wifi_cfg.sta.ssid, ssid, slen); wifi_cfg.sta.ssid[slen] = '\0';
    slen = strlen(pass);
    if (slen >= sizeof(wifi_cfg.sta.password)) slen = sizeof(wifi_cfg.sta.password) - 1;
    memcpy(wifi_cfg.sta.password, pass, slen); wifi_cfg.sta.password[slen] = '\0';
    slen = strlen(ssid);
    if (slen >= sizeof(app->_wifi_ssid)) slen = sizeof(app->_wifi_ssid) - 1;
    memcpy(app->_wifi_ssid, ssid, slen); app->_wifi_ssid[slen] = '\0';
    slen = strlen(pass);
    if (slen >= sizeof(app->_wifi_password)) slen = sizeof(app->_wifi_password) - 1;
    memcpy(app->_wifi_password, pass, slen); app->_wifi_password[slen] = '\0';

    if (!app->_is_ui_del) app->processWifiConnect(WIFI_CONNECT_RUNNING);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(app->_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) {
        app->setNvsStr(NVS_KEY_WIFI_SSID, app->_wifi_ssid);
        app->setNvsStr(NVS_KEY_WIFI_PASS, app->_wifi_password);
        app->setNvsParam(NVS_KEY_WIFI_EN, 1);
        app->_nvs.wifi_en = 1;
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!app->_is_ui_del) {
            app->processWifiConnect(WIFI_CONNECT_HIDE);
            if (bsp_display_lock(0)) {
                lv_textarea_set_text(app->_ta_password, "");
                app->_screen_index = SCREEN_WIFI_LIST;
                lv_scr_load(app->_scr_wifi_list);
                bsp_display_unlock();
            }
        }
    } else {
        if (!app->_is_ui_del) {
            app->processWifiConnect(WIFI_CONNECT_FAIL);
            vTaskDelay(pdMS_TO_TICKS(2000));
            app->processWifiConnect(WIFI_CONNECT_HIDE);
        }
    }
    app->_wifi_connecting = false;
    vTaskDelete(NULL);
}

void PhoneAppSettings::wifiEventHandler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    /* Use static _wifi_event_group — arg may be nullptr during boot auto-connect */
    if (!_wifi_event_group) return;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* Cancel any pending reconnection and try fresh */
        if (_wifi_reconnect_timer) {
            xTimerStop(_wifi_reconnect_timer, 0);
            xTimerDelete(_wifi_reconnect_timer, 0);
            _wifi_reconnect_timer = nullptr;
        }
        _wifi_reconnect_count = 0;
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        xEventGroupSetBits(_wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_event_sta_connected_t *evt = (wifi_event_sta_connected_t *)event_data;
        publish_wifi_state(true, false, 0, (const char *)evt->ssid);
        if (_wifi_reconnect_timer) {
            ESP_LOGI(TAG, "WiFi reconnected to %s after %lu attempt(s)", evt->ssid, _wifi_reconnect_count);
            _wifi_reconnect_count = 0;
            xTimerStop(_wifi_reconnect_timer, 0);
            xTimerDelete(_wifi_reconnect_timer, 0);
            _wifi_reconnect_timer = nullptr;
        } else {
            ESP_LOGI(TAG, "WiFi connected to %s", evt->ssid);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(_wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGI(TAG, "WiFi disconnected, reason=%d", evt->reason);
        publish_wifi_state(false, false, 0, "");
        /* Check if WiFi is enabled in NVS — if so, start 10s periodic reconnect */
        nvs_handle_t nvs_h;
        if (nvs_open(NVS_NAMESPACE_SETTINGS, NVS_READONLY, &nvs_h) == ESP_OK) {
            int32_t wifi_en = 0;
            nvs_get_i32(nvs_h, NVS_KEY_WIFI_EN, &wifi_en);
            nvs_close(nvs_h);
            if (wifi_en && !_wifi_reconnect_timer) {
                _wifi_reconnect_timer = xTimerCreate("wifi_recon",
                    pdMS_TO_TICKS(10000), pdTRUE, NULL, wifiReconnectTimerCallback);
                if (_wifi_reconnect_timer) {
                    xTimerStart(_wifi_reconnect_timer, 0);
                    ESP_LOGI(TAG, "WiFi auto-reconnect timer started (10s interval)");
                }
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        /* Update mDNS delegated hostname IP now that WiFi has an address */
        shared_mdns_update_delegate_ip();
        /* Start SNTP to sync wall-clock time (needed for ULog date-based naming) */
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_set_time_sync_notification_cb([](struct timeval *tv) {
                ESP_LOGI(TAG, "SNTP synchronized — wall-clock time: %ld.%06ld", (long)tv->tv_sec, (long)tv->tv_usec);
            });
            esp_sntp_init();
            ESP_LOGI(TAG, "SNTP started — wall-clock time will sync shortly");
        }
    }
}

void PhoneAppSettings::wifiReconnectTimerCallback(TimerHandle_t xTimer)
{
    _wifi_reconnect_count++;
    if (_wifi_reconnect_count > 20) {
        ESP_LOGW(TAG, "WiFi auto-reconnect stopped after %lu failed attempts", _wifi_reconnect_count);
        xTimerStop(_wifi_reconnect_timer, 0);
        return;
    }
    ESP_LOGI(TAG, "WiFi auto-reconnect [%lu]: calling esp_wifi_connect()", _wifi_reconnect_count);
    esp_wifi_connect();
}

void PhoneAppSettings::onWifiRowClicked(lv_event_t *e)
{
    PhoneAppSettings *app = (PhoneAppSettings *)lv_event_get_user_data(e);
    if (!app) return;

    app->_screen_index = SCREEN_WIFI_LIST;
    lv_scr_load(app->_scr_wifi_list);

    /* Start scan if WiFi is enabled */
    if (app->_nvs.wifi_en) {
        app->startWifiScan();
    }
}

void PhoneAppSettings::onWifiSwitchChanged(lv_event_t *e)
{
    PhoneAppSettings *app = (PhoneAppSettings *)lv_event_get_user_data(e);
    if (!app) return;
    bool on = lv_obj_has_state(app->_sw_wifi, LV_STATE_CHECKED);
    app->_nvs.wifi_en = on ? 1 : 0;
    app->setNvsParam(NVS_KEY_WIFI_EN, on ? 1 : 0);

    if (on) {
        if (app->_wifi_scan_task == NULL) {
            /* Create event group BEFORE spawning task (OFF path needs it) */
            if (app->_wifi_event_group == NULL) {
                app->_wifi_event_group = xEventGroupCreate();
            }
            if (app->_wifi_event_group == NULL) {
                ESP_LOGE(TAG, "Failed to create WiFi event group");
                lv_obj_clear_state(app->_sw_wifi, LV_STATE_CHECKED);
                app->_nvs.wifi_en = 0;
                app->setNvsParam(NVS_KEY_WIFI_EN, 0);
                return;
            }
            BaseType_t ret = xTaskCreate(wifiScanTaskHandler, "wifi_scan", TASK_STACK_WIFI_SCAN,
                                         app, TASK_PRIO_WIFI_SCAN, &app->_wifi_scan_task);
            if (ret != pdPASS) {
                ESP_LOGE(TAG, "Failed to create WiFi scan task");
                lv_obj_clear_state(app->_sw_wifi, LV_STATE_CHECKED);
                app->_nvs.wifi_en = 0;
                app->setNvsParam(NVS_KEY_WIFI_EN, 0);
                vEventGroupDelete(app->_wifi_event_group);
                app->_wifi_event_group = nullptr;
            }
        }
    } else {
        app->stopWifiScan();
        if (app->_wifi_event_group &&
            (xEventGroupGetBits(app->_wifi_event_group) & WIFI_CONNECTED_BIT)) {
            esp_wifi_disconnect();
        }
        esp_wifi_stop();  // Power down WiFi hardware (keeps stack init'd)
        /* Stop and delete reconnection timer, reset count */
        if (_wifi_reconnect_timer) {
            xTimerStop(_wifi_reconnect_timer, 0);
            xTimerDelete(_wifi_reconnect_timer, 0);
            _wifi_reconnect_timer = nullptr;
        }
        _wifi_reconnect_count = 0;
        /* Clean up scan task and event group on WiFi OFF */
        if (app->_wifi_scan_task) {
            vTaskDelete(app->_wifi_scan_task);
            app->_wifi_scan_task = nullptr;
        }
        if (app->_wifi_event_group) {
            vEventGroupDelete(app->_wifi_event_group);
            app->_wifi_event_group = nullptr;
        }
        /* Unregister event handler instances to allow clean re-init */
        if (_wifi_handler_inst) {
            esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, _wifi_handler_inst);
            _wifi_handler_inst = nullptr;
        }
        if (_ip_handler_inst) {
            esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, _ip_handler_inst);
            _ip_handler_inst = nullptr;
        }
        _wifi_initialized = false;
    }
}

void PhoneAppSettings::onWifiItemClicked(lv_event_t *e)
{
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t *ssid_label = lv_obj_get_child(btn, 0);
    if (!ssid_label) return;
    const char *ssid = lv_label_get_text(ssid_label);
    if (!ssid || strlen(ssid) == 0) return;

    lv_obj_t *scr = lv_obj_get_screen(btn);
    PhoneAppSettings *app = (PhoneAppSettings *)lv_obj_get_user_data(scr);
    if (!app) return;

    char buf[64];
    snprintf(buf, sizeof(buf), "SSID: %s", ssid);
    lv_label_set_text(app->_label_pass_ssid, buf);
    lv_textarea_set_text(app->_ta_password, "");
    app->stopWifiScan();
    app->_screen_index = SCREEN_WIFI_PASSWORD;
    lv_scr_load(app->_scr_wifi_pass);
}

void PhoneAppSettings::onKeyboardEnterClicked(lv_event_t *e)
{
    PhoneAppSettings *app = (PhoneAppSettings *)lv_event_get_user_data(e);
    if (!app || app->_wifi_connecting) return;  // Guard against multiple connect tasks
    app->_wifi_connecting = true;
    BaseType_t ret = xTaskCreate(wifiConnectTaskHandler, "wifi_conn", TASK_STACK_WIFI_CONNECT,
                                 app, TASK_PRIO_WIFI_CONNECT, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WiFi connect task");
        app->_wifi_connecting = false;
    }
}

void PhoneAppSettings::onWifiListScreenLoaded(lv_event_t *e)
{
    PhoneAppSettings *app = (PhoneAppSettings *)lv_event_get_user_data(e);
    if (!app) return;
    app->_screen_index = SCREEN_WIFI_LIST;
    app->processWifiConnect(WIFI_CONNECT_HIDE);
    if (app->_nvs.wifi_en) app->startWifiScan();
}
