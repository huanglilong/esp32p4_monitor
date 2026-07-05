#pragma once

#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_brookesia.hpp"
#include "esp_event.h"
#include "esp_wifi.h"
class PhoneAppSettings : public ESP_Brookesia_PhoneApp {
public:
    PhoneAppSettings(bool use_status_bar = false, bool use_navigation_bar = false);
    ~PhoneAppSettings();

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

    /** Boot-time WiFi auto-connect: read NVS and connect if SSID stored */
    static void bootWifiAutoConnect(void);

private:
    /* Screen indices */
    enum ScreenIndex {
        SCREEN_MAIN = 0,
        SCREEN_WIFI_LIST,
        SCREEN_WIFI_PASSWORD,
        SCREEN_MAX
    };
    enum WifiConnectState {
        WIFI_CONNECT_HIDE = 0,
        WIFI_CONNECT_RUNNING,
        WIFI_CONNECT_SUCCESS,
        WIFI_CONNECT_FAIL,
    };
    /* NVS */
    bool loadNvsParam(void);
    bool setNvsParam(const char *key, int32_t value);
    bool setNvsStr(const char *key, const char *value);
    bool getNvsStr(const char *key, char *out, size_t max_len);
    void applySettings(void);

    /* UI creation */
    void createMainScreen(void);
    void createWifiListScreen(void);
    void createWifiPasswordScreen(void);
    void updateMainScreenFromNvs(void);
    /* WiFi */
    esp_err_t wifiInit(void);
    void startWifiScan(void);
    void stopWifiScan(void);
    void scanWifiAndUpdateUi(void);
    void processWifiConnect(WifiConnectState state);
    int  getWifiSignalLevel(int rssi);

    /* Tasks */
    static void wifiScanTaskHandler(void *arg);
    static void wifiConnectTaskHandler(void *arg);

    /* Event handlers */
    static void wifiEventHandler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data);
    /** Reconnect timer: fires every 10s to retry esp_wifi_connect() on disconnect */
    static void wifiReconnectTimerCallback(TimerHandle_t xTimer);

    /* WiFi callbacks */
    static void onWifiSwitchChanged(lv_event_t *e);
    static void onWifiRowClicked(lv_event_t *e);
    static void onWifiItemClicked(lv_event_t *e);
    static void onKeyboardEnterClicked(lv_event_t *e);
    static void onWifiListScreenLoaded(lv_event_t *e);
    /* UI callbacks */
    static void onBackClicked(lv_event_t *e);
    static void onVolumeSliderChanged(lv_event_t *e);
    static void onBrightnessSliderChanged(lv_event_t *e);
    static void onMainScreenLoaded(lv_event_t *e);

    /* NVS debounce save (avoids flash wear from rapid slider events) */
    static void _nvs_save_timer_cb(lv_timer_t *timer);
    bool                   _nvs_dirty;
    lv_timer_t            *_nvs_save_timer;
    lv_timer_t            *_status_timer;       // WiFi/volume/brightness status refresh

    /* State */
    ScreenIndex            _screen_index;
    bool                   _is_ui_del;

    /* NVS parameters — flat struct avoids std::map / std::string heap allocations.
     * Only 3 keys: wifi_en, volume, brightness — no need for a map. */
    struct {
        int32_t wifi_en;
        int32_t volume;
        int32_t brightness;
    } _nvs;

    char                   _wifi_ssid[33];
    char                   _wifi_password[65];
    char                   _wifi_ip[16];         /* "xxx.xxx.xxx.xxx" */
    int                    _wifi_rssi;           /* signal strength dBm */

    /* LVGL objects - Main screen */
    lv_obj_t              *_scr_main;

    lv_obj_t              *_sw_wifi;
    lv_obj_t              *_label_wifi;

    lv_obj_t              *_slider_vol;
    lv_obj_t              *_label_vol;
    lv_obj_t              *_slider_brightness;
    lv_obj_t              *_label_brightness;

    /* LVGL objects - WiFi list screen */
    lv_obj_t              *_scr_wifi_list;
    lv_obj_t              *_list_wifi;
    lv_obj_t              *_spinner_wifi;

    /* LVGL objects - WiFi password screen */
    lv_obj_t              *_scr_wifi_pass;
    lv_obj_t              *_label_pass_ssid;
    lv_obj_t              *_ta_password;
    lv_obj_t              *_kb_password;
    lv_obj_t              *_spinner_connect;
    lv_obj_t              *_label_connect_status;
    /* WiFi state — static to persist across Settings app open/close cycles.
     * WiFi runs in background even when Settings app is closed. */
    static TaskHandle_t      _wifi_scan_task;
    static EventGroupHandle_t _wifi_event_group;
    static bool              _wifi_initialized;  // one-time netif/wifi init done
    static TimerHandle_t     _wifi_reconnect_timer;  // 10s periodic reconnect timer
    static uint32_t          _wifi_reconnect_count;  // consecutive reconnect attempts
    static esp_event_handler_instance_t _wifi_handler_inst;  // WIFI_EVENT handler instance
    static esp_event_handler_instance_t _ip_handler_inst;    // IP_EVENT handler instance
    volatile bool            _wifi_scanning;
    volatile bool            _wifi_connecting;     // Guard against multiple connect tasks

    static constexpr int   WIFI_SCAN_MAX = 20;
    static constexpr int   TASK_STACK_WIFI_SCAN = 6 * 1024;
    static constexpr int   TASK_STACK_WIFI_CONNECT = 4 * 1024;
    static constexpr int   TASK_PRIO_WIFI_SCAN = 1;
    static constexpr int   TASK_PRIO_WIFI_CONNECT = 4;
    static constexpr int   VOLUME_MIN = 0;
    static constexpr int   VOLUME_MAX = 100;
    static constexpr int   VOLUME_DEFAULT = 60;
    static constexpr int   BRIGHTNESS_MIN = 20;
    static constexpr int   BRIGHTNESS_MAX = 100;
    static constexpr int   BRIGHTNESS_DEFAULT = 80;
};
