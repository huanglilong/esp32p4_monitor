#pragma once

#include "esp_brookesia.hpp"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

/**
 * @brief Camera Stream App — standalone MJPEG stream over WiFi.
 *
 * Shows WiFi status, enables/disables CameraStream, monitors system resources.
 * Only available when WiFi is connected.
 */
class PhoneAppCameraStream : public ESP_Brookesia_PhoneApp {
public:
    PhoneAppCameraStream(bool use_status_bar = false, bool use_navigation_bar = false);
    ~PhoneAppCameraStream();

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;

private:
    /* UI creation */
    void _create_ui(void);

    /* Update UI state */
    void _update_wifi_status(void);
    void _update_stream_info(void);

    /* Timer callback */
    static void _ui_timer_cb(lv_timer_t *timer);

    /* Event callbacks */
    static void _on_toggle_clicked(lv_event_t *e);
    static void _wifi_event_handler(void *arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data);

    /* LVGL objects - WiFi row */
    lv_obj_t *_scr_main;
    lv_obj_t *_label_wifi_status;
    lv_obj_t *_label_wifi_ip;

    /* LVGL objects - Camera stream toggle */
    lv_obj_t *_sw_cam_stream;
    lv_obj_t *_label_cam_status;

    /* LVGL objects - Stream info */
    lv_obj_t *_label_stream_info;     /* Resolution, FPS */
    lv_obj_t *_label_stream_bytes;    /* Total frames + JPEG info */

    /* Timer */
    lv_timer_t *_timer;

    /* WiFi state (polled from esp_netif) */
    char _wifi_ip[16];
    bool _wifi_connected;

    /* CameraStream FPS tracking (display only) */
    uint32_t _prev_frame_count;
    uint32_t _prev_total_bytes;
    uint32_t _fps;

    static constexpr int UI_REFRESH_MS = 2000;   /* Update UI every 2s */
    static constexpr int TASK_STACK = 4 * 1024;
};
