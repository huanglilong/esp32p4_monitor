/*
 * Camera Stream App — standalone MJPEG stream over WiFi.
 * Separated from Settings app. Only usable when WiFi is connected.
 */

#include "phone_app_camera_stream.hpp"
#include "private/esp_brookesia_utils.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "camera_stream.hpp"
#include "example_config.h"
#include "uorb.h"
#include "topics.h"
#include <cstring>

static const char *TAG = "CamStreamApp";

/* Use built-in launcher icon */
extern const lv_image_dsc_t esp_brookesia_image_large_app_launcher_default_112_112;

/*============================================================================
 * Constructor / Destructor
 *============================================================================*/
/* uORB subscriber for FPS stats */
static orb_sub_t s_fps_sub = -1;

PhoneAppCameraStream::PhoneAppCameraStream(bool use_status_bar, bool use_navigation_bar) :
    ESP_Brookesia_PhoneApp("Camera Stream", &esp_brookesia_image_large_app_launcher_default_112_112,
                           true, use_status_bar, use_navigation_bar),
    _scr_main(nullptr),
    _label_wifi_status(nullptr), _label_wifi_ip(nullptr),
    _sw_cam_stream(nullptr), _label_cam_status(nullptr),
    _label_stream_info(nullptr), _label_stream_bytes(nullptr),
    _timer(nullptr),
    _wifi_connected(false),
    _prev_frame_count(0), _prev_total_bytes(0), _fps(0)
{
    memset(_wifi_ip, 0, sizeof(_wifi_ip));
    /* Subscribe to fps_stats once */
    if (s_fps_sub < 0) {
        s_fps_sub = orb_subscribe(ORB_ID(fps_stats));
    }
}

PhoneAppCameraStream::~PhoneAppCameraStream()
{
    CameraStream::instance().stop();
}

/*============================================================================
 * App Lifecycle
 *============================================================================*/
bool PhoneAppCameraStream::run(void)
{
    ESP_LOGI(TAG, "Camera Stream app starting...");

    _create_ui();

    /* Register WiFi event handler to auto-stop stream on disconnect */
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                _wifi_event_handler, this);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                _wifi_event_handler, this);

    /* UI refresh timer */
    _timer = lv_timer_create(_ui_timer_cb, UI_REFRESH_MS, this);

    ESP_LOGI(TAG, "Camera Stream app running");
    return true;
}

bool PhoneAppCameraStream::back(void)
{
    ESP_LOGI(TAG, "Camera Stream app back");
    ESP_BROOKESIA_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
    return true;
}

bool PhoneAppCameraStream::close(void)
{
    ESP_LOGI(TAG, "Camera Stream app closing...");

    /* Stop camera stream if active */
    CameraStream::instance().stop();

    /* Unregister WiFi event handlers */
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                  _wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                  _wifi_event_handler);

    /* Unsubscribe from fps_stats uORB topic */
    if (s_fps_sub >= 0) {
        orb_unsubscribe(s_fps_sub);
        s_fps_sub = -1;
    }

    /* Clean up timer */
    if (_timer) {
        lv_timer_delete(_timer);
        _timer = nullptr;
    }

    /* Clear LVGL pointers */
    _scr_main = nullptr;
    _label_wifi_status = nullptr;
    _label_wifi_ip = nullptr;
    _sw_cam_stream = nullptr;
    _label_cam_status = nullptr;
    _label_stream_info = nullptr;
    _label_stream_bytes = nullptr;

    return true;
}

/*============================================================================
 * UI Creation
 *============================================================================*/
void PhoneAppCameraStream::_create_ui(void)
{
    _scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(_scr_main, lv_color_hex(0xFFFFFF), 0);

    /* Title */
    lv_obj_t *title = lv_label_create(_scr_main);
    lv_label_set_text(title, "Camera Stream");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x000000), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    /* Back button */
    lv_obj_t *btn_back = lv_btn_create(_scr_main);
    lv_obj_set_size(btn_back, 50, 50);
    lv_obj_align(btn_back, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_radius(btn_back, 25, 0);
    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(btn_back, [](lv_event_t *e) {
        PhoneAppCameraStream *app = (PhoneAppCameraStream *)lv_event_get_user_data(e);
        app->back();
    }, LV_EVENT_CLICKED, this);

    /* --- WiFi Status Row --- */
    lv_obj_t *cont_wifi = lv_obj_create(_scr_main);
    lv_obj_set_size(cont_wifi, 620, 60);
    lv_obj_align(cont_wifi, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_border_width(cont_wifi, 0, 0);
    lv_obj_set_style_bg_opa(cont_wifi, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(cont_wifi, lv_color_hex(0xE0E0E0), 0);

    lv_obj_t *icon_wifi = lv_label_create(cont_wifi);
    lv_label_set_text(icon_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(icon_wifi, LV_ALIGN_LEFT_MID, 15, 0);

    _label_wifi_status = lv_label_create(cont_wifi);
    lv_label_set_text(_label_wifi_status, "Wi-Fi: checking...");
    lv_obj_set_style_text_color(_label_wifi_status, lv_color_hex(0x000000), 0);
    lv_obj_align(_label_wifi_status, LV_ALIGN_LEFT_MID, 55, 0);

    _label_wifi_ip = lv_label_create(cont_wifi);
    lv_label_set_text(_label_wifi_ip, "");
    lv_obj_set_style_text_color(_label_wifi_ip, lv_color_hex(0x4CAF50), 0);
    lv_obj_align(_label_wifi_ip, LV_ALIGN_RIGHT_MID, -15, 0);

    /* --- Camera Stream Toggle Row --- */
    lv_obj_t *cont_cam = lv_obj_create(_scr_main);
    lv_obj_set_size(cont_cam, 620, 60);
    lv_obj_align(cont_cam, LV_ALIGN_TOP_MID, 0, 145);
    lv_obj_set_style_border_width(cont_cam, 0, 0);
    lv_obj_set_style_bg_opa(cont_cam, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(cont_cam, lv_color_hex(0xE0E0E0), 0);

    lv_obj_t *icon_cam = lv_label_create(cont_cam);
    lv_label_set_text(icon_cam, LV_SYMBOL_IMAGE);
    lv_obj_align(icon_cam, LV_ALIGN_LEFT_MID, 15, 0);

    _label_cam_status = lv_label_create(cont_cam);
    lv_label_set_text(_label_cam_status, "MJPEG Stream");
    lv_obj_set_style_text_color(_label_cam_status, lv_color_hex(0x000000), 0);
    lv_obj_align(_label_cam_status, LV_ALIGN_LEFT_MID, 55, 0);

    _sw_cam_stream = lv_switch_create(cont_cam);
    lv_obj_align(_sw_cam_stream, LV_ALIGN_RIGHT_MID, -15, 0);
    lv_obj_clear_state(_sw_cam_stream, LV_STATE_CHECKED);
    lv_obj_add_event_cb(_sw_cam_stream, _on_toggle_clicked, LV_EVENT_VALUE_CHANGED, this);

    /* --- Stream Info Row --- */
    lv_obj_t *cont_info = lv_obj_create(_scr_main);
    lv_obj_set_size(cont_info, 620, 110);
    lv_obj_align(cont_info, LV_ALIGN_TOP_MID, 0, 220);
    lv_obj_set_style_border_width(cont_info, 0, 0);
    lv_obj_set_style_bg_opa(cont_info, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(cont_info, lv_color_hex(0xE0E0E0), 0);

    _label_stream_info = lv_label_create(cont_info);
    lv_label_set_text(_label_stream_info, "Stream: idle");
    lv_obj_set_style_text_color(_label_stream_info, lv_color_hex(0x333333), 0);
    lv_obj_align(_label_stream_info, LV_ALIGN_TOP_LEFT, 15, 15);

    _label_stream_bytes = lv_label_create(cont_info);
    lv_label_set_text(_label_stream_bytes, "");
    lv_obj_set_style_text_color(_label_stream_bytes, lv_color_hex(0x888888), 0);
    lv_obj_align(_label_stream_bytes, LV_ALIGN_TOP_LEFT, 15, 55);

    lv_scr_load(_scr_main);

    /* Initial data fill */
    _update_wifi_status();
}

/*============================================================================
 * UI Update Methods
 *============================================================================*/
void PhoneAppCameraStream::_update_wifi_status(void)
{
    bool was_connected = _wifi_connected;

    /* Check WiFi connection via NETIF */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            _wifi_connected = true;
            snprintf(_wifi_ip, sizeof(_wifi_ip), IPSTR, IP2STR(&ip.ip));
        } else {
            _wifi_connected = false;
            _wifi_ip[0] = '\0';
        }
    } else {
        _wifi_connected = false;
        _wifi_ip[0] = '\0';
    }

    if (!_label_wifi_status) return;

    if (_wifi_connected) {
        lv_label_set_text(_label_wifi_status, "Wi-Fi: Connected");
        lv_obj_set_style_text_color(_label_wifi_status, lv_color_hex(0x4CAF50), 0);
        if (_label_wifi_ip) {
            lv_label_set_text(_label_wifi_ip, _wifi_ip);
        }
    } else {
        lv_label_set_text(_label_wifi_status, "Wi-Fi: Not connected");
        lv_obj_set_style_text_color(_label_wifi_status, lv_color_hex(0xF44336), 0);
        if (_label_wifi_ip) {
            lv_label_set_text(_label_wifi_ip, "");
        }
    }

    /* If WiFi was just lost, auto-stop the stream */
    if (was_connected && !_wifi_connected && CameraStream::instance().isRunning()) {
        ESP_LOGW(TAG, "WiFi disconnected — stopping camera stream");
        CameraStream::instance().stop();
        if (_sw_cam_stream) {
            lv_obj_clear_state(_sw_cam_stream, LV_STATE_CHECKED);
        }
        _update_stream_info();
    }
}

void PhoneAppCameraStream::_update_stream_info(void)
{
    CameraStream &cs = CameraStream::instance();
    bool running = cs.isRunning();

    if (!_label_stream_info) return;

    if (running) {
        /* Read latest FPS stats from uORB topic (non-blocking) */
        bool updated = false;
        struct fps_stats_s fps_data = {};
        if (s_fps_sub >= 0 && orb_check(s_fps_sub, &updated) == 0 && updated) {
            orb_copy(ORB_ID(fps_stats), s_fps_sub, &fps_data);
            _prev_frame_count = fps_data.frame_count;
            _prev_total_bytes = fps_data.fps_total_bytes;
        }

        /* Calculate FPS since last update */
        _fps = (_fps > 0) ? _fps : 0;  /* Keep previous FPS if no new data */

        lv_label_set_text_fmt(_label_stream_info,
            "Stream: ACTIVE  %lux%lu",
            (unsigned long)cs._cam_width,
            (unsigned long)cs._cam_height);

        if (_label_stream_bytes) {
            lv_label_set_text_fmt(_label_stream_bytes,
                "Total frames: %lu",
                (unsigned long)_prev_frame_count);
        }
    } else {
        _fps = 0;
        lv_label_set_text(_label_stream_info, "Stream: idle");
        if (_label_stream_bytes) {
            lv_label_set_text(_label_stream_bytes, "");
        }
    }
}

/*============================================================================
 * Timer Callback
 *============================================================================*/
void PhoneAppCameraStream::_ui_timer_cb(lv_timer_t *timer)
{
    PhoneAppCameraStream *app = (PhoneAppCameraStream *)timer->user_data;
    if (!app || !app->_scr_main) return;

    app->_update_wifi_status();
    app->_update_stream_info();
}

/*============================================================================
 * Event Callbacks
 *============================================================================*/
void PhoneAppCameraStream::_on_toggle_clicked(lv_event_t *e)
{
    PhoneAppCameraStream *app = (PhoneAppCameraStream *)lv_event_get_user_data(e);
    if (!app) return;

    bool on = lv_obj_has_state(app->_sw_cam_stream, LV_STATE_CHECKED);

    if (on) {
        /* Check WiFi is connected */
        if (!app->_wifi_connected) {
            ESP_LOGW(TAG, "Cannot start stream: WiFi not connected");
            lv_obj_clear_state(app->_sw_cam_stream, LV_STATE_CHECKED);
            return;
        }

        ESP_LOGI(TAG, "Starting camera stream...");
        if (!CameraStream::instance().start()) {
            ESP_LOGE(TAG, "Camera stream start failed");
            lv_obj_clear_state(app->_sw_cam_stream, LV_STATE_CHECKED);
            return;
        }
        ESP_LOGI(TAG, "Camera stream started — http://%s/stream", app->_wifi_ip);
    } else {
        ESP_LOGI(TAG, "Stopping camera stream...");
        CameraStream::instance().stop();
    }

    app->_prev_frame_count = 0;
    app->_prev_total_bytes = 0;
    app->_update_stream_info();
}

void PhoneAppCameraStream::_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                                int32_t event_id, void *event_data)
{
    PhoneAppCameraStream *app = (PhoneAppCameraStream *)arg;
    if (!app) return;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi DISCONNECT event — stopping stream if active");
        /* Stop camera stream (safe to call from event context).
         * LVGL switch state will be updated by UI timer (_update_wifi_status),
         * which runs in LVGL context and already handles the disconnect case. */
        if (CameraStream::instance().isRunning()) {
            CameraStream::instance().stop();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        snprintf(app->_wifi_ip, sizeof(app->_wifi_ip), IPSTR, IP2STR(&evt->ip_info.ip));
        ESP_LOGI(TAG, "WiFi got IP: %s", app->_wifi_ip);
        /* Update mDNS delegated hostname IP now that WiFi has an address */
        shared_mdns_update_delegate_ip();
    }
}
