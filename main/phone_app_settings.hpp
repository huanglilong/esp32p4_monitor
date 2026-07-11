#pragma once

#include <atomic>
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_brookesia.hpp"
#include "esp_event.h"
#include "esp_wifi.h"
#include "uorb.h"
#include "topics.h"

class PhoneAppSettings : public ESP_Brookesia_PhoneApp {
public:
    PhoneAppSettings(bool use_status_bar = false, bool use_navigation_bar = false);
    ~PhoneAppSettings();

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

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
    void startWifiScan(void);
    void stopWifiScan(void);
    void scanWifiAndUpdateUi(void);
    void processWifiConnect(WifiConnectState state);
    int  getWifiSignalLevel(int rssi);

    /* Tasks */
    static void wifiScanTaskHandler(void *arg);
    static void wifiConnectTaskHandler(void *arg);

    /* WiFi callbacks */
    static void onWifiRowClicked(lv_event_t *e);
    static void onWifiItemClicked(lv_event_t *e);
    static void onKeyboardEnterClicked(lv_event_t *e);
    static void onWifiListScreenLoaded(lv_event_t *e);
    /* UI callbacks */
    static void onBackClicked(lv_event_t *e);
    static void onVolumeSliderChanged(lv_event_t *e);
    static void onBrightnessSliderChanged(lv_event_t *e);
    static void onMainScreenLoaded(lv_event_t *e);

    /* Camera Stream callback */
    static void onCamStreamSwitchChanged(lv_event_t *e);

    /* NVS debounce save (avoids flash wear from rapid slider events) */
    static void _nvs_save_timer_cb(lv_timer_t *timer);
    bool                   _nvs_dirty;
    lv_timer_t            *_nvs_save_timer;
    lv_timer_t            *_status_timer;       // WiFi/volume/brightness status refresh

    /* State */
    ScreenIndex            _screen_index;
    std::atomic<bool>      _is_ui_del;

    /* NVS parameters — flat struct avoids std::map / std::string heap allocations.
     * Only 2 keys: volume, brightness — no need for a map. WiFi is always enabled. */
    struct {
        int32_t volume;
        int32_t brightness;
    } _nvs;

    char                   _wifi_ssid[33];
    char                   _wifi_password[65];
    char                   _wifi_ip[16];         /* "xxx.xxx.xxx.xxx" */
    int                    _wifi_rssi;           /* signal strength dBm */

    /* LVGL objects - Main screen */
    lv_obj_t              *_scr_main;

    lv_obj_t              *_label_wifi;

    lv_obj_t              *_slider_vol;
    lv_obj_t              *_label_vol;
    lv_obj_t              *_slider_brightness;
    lv_obj_t              *_label_brightness;

    /* LVGL objects - Camera Stream */
    lv_obj_t              *_sw_cam_stream;
    lv_obj_t              *_label_cam_status;

    /* Camera Stream state */
    static orb_sub_t       s_fps_sub;
    uint32_t               _cam_fps;
    uint32_t               _cam_frame_count;
    static std::atomic<TaskHandle_t> _cam_start_stop_task;  /* Tracks cam_start/cam_stop task to prevent orphans */

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
     * WiFi runs in background even when Settings app is closed, managed by WifiService. */
    static std::atomic<TaskHandle_t> _wifi_scan_task;
    std::atomic<bool>               _wifi_scanning;
    static std::atomic<bool>        _wifi_scan_exit;          /* Signal scan task to self-delete */
    static std::atomic<TaskHandle_t> _wifi_connect_task;      /* Handle for connect task (for cleanup) */
    static std::atomic<bool>        _wifi_connecting;

    static constexpr int   WIFI_SCAN_MAX = 20;
    static constexpr int   TASK_STACK_WIFI_SCAN = 6 * 1024;
    static constexpr int   TASK_STACK_WIFI_CONNECT = 4 * 1024;
    static constexpr int   TASK_PRIO_WIFI_SCAN = 1;
    static constexpr int   TASK_PRIO_WIFI_CONNECT = 4;
};
