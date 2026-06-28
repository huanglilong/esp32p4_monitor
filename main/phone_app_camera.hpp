#pragma once

#include "esp_brookesia.hpp"
#include "esp_cam_ctlr.h"
#include "dl_detect_define.hpp"
#include <list>

class COCODetect;  // Forward declaration

class PhoneAppCamera : public ESP_Brookesia_PhoneApp {
public:
    PhoneAppCamera(bool use_status_bar = false, bool use_navigation_bar = false);
    ~PhoneAppCamera();

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;

private:
    static void _frame_update_timer_cb(lv_timer_t *timer);
    static void _detection_task(void *arg);
    bool _init_camera(void);
    bool _deinit_camera(void);
    void _init_sensor(void);

    /* Detection init/deinit */
    bool _init_detection(void);
    void _deinit_detection(void);

    /* Camera hardware handles */
    void               *_cam_buffer;       // Camera frame buffer in PSRAM (RGB888)
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

    /* Detection subsystem */
    COCODetect                 *_detector;           // COCO detection instance
    void                       *_detect_buf;         // Snapshot buffer for detection (RGB565 copy)
    size_t                      _detect_buf_size;    // Size of snapshot buffer
    std::list<dl::detect::result_t> _detect_results; // Latest detection results
    bool                        _detect_available;   // New results available?
    TaskHandle_t                _detect_task_handle; // Detection FreeRTOS task
    SemaphoreHandle_t           _detect_mutex;       // Mutex for results
    static constexpr float      PERSON_SCORE_THRESHOLD = 0.35f;
    static constexpr int        DETECT_INTERVAL_MS = 600;
    static constexpr int        BOX_LINE_WIDTH = 2;

    /* Draw helper: draw hollow rectangle directly on canvas buffer */
    void _draw_box_on_canvas(int x1, int y1, int x2, int y2, lv_color_t color);

    /* PPA hardware accelerator for image resize (RGB888 800x→320x) */
    void                       *_ppa_handle;        // ppa_client_handle_t
    void                       *_ppa_buf;           // PPA output buffer (320x320x3 RGB888)
    size_t                      _ppa_buf_size;
};
