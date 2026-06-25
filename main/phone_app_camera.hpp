#pragma once

#include "esp_brookesia.hpp"
#include "esp_cam_ctlr.h"

class PhoneAppCamera : public ESP_Brookesia_PhoneApp {
public:
    PhoneAppCamera(bool use_status_bar = false, bool use_navigation_bar = false);
    ~PhoneAppCamera();

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;

private:
    static void _frame_update_timer_cb(lv_timer_t *timer);
    bool _init_camera(void);
    bool _deinit_camera(void);
    void _init_sensor(void);

    /* Camera hardware handles */
    void               *_cam_buffer;       // Camera frame buffer in PSRAM
    size_t              _cam_buf_size;      // Buffer size in bytes
    void               *_isp_proc;          // ISP processor handle (isp_proc_handle_t)
    void               *_cam_ctlr;          // CAM controller handle (esp_cam_ctlr_handle_t)
    void               *_cam_sensor;        // Camera sensor device (esp_cam_sensor_device_t)
    void               *_sccb_handle;       // SCCB I/O handle (esp_sccb_io_handle_t)
    esp_cam_ctlr_trans_t _cam_trans;       // Camera transaction info (must persist)

    /* LVGL objects */
    lv_obj_t           *_cam_canvas;         // LVGL canvas for camera preview
    lv_timer_t         *_refresh_timer;      // Refresh timer (30Hz)

    /* Back button */
    lv_obj_t           *_btn_back;          // Back button overlay

    bool                _cam_running;
};
