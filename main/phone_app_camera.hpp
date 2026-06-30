#pragma once

#include "esp_brookesia.hpp"
#include "dl_detect_define.hpp"
#include "linux/videodev2.h"
#include <list>

class COCODetect;  // Forward declaration

class PhoneAppCamera : public ESP_Brookesia_PhoneApp {
public:
    PhoneAppCamera(bool use_status_bar = false, bool use_navigation_bar = false);
    ~PhoneAppCamera();

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;

    /* Public accessor for CameraStream to check if app is active */
    bool isCameraRunning(void) const { return _cam_running; }

private:
    static void _frame_update_timer_cb(lv_timer_t *timer);
    static void _detection_task(void *arg);
    bool _init_camera(void);
    bool _deinit_camera(void);

    /* Detection init/deinit */
    bool _init_detection(void);
    void _deinit_detection(void);

    /* V4L2 camera handles */
    int                  _video_fd;           // V4L2 device file descriptor
    void               *_cam_buffer;          // Camera frame buffer in PSRAM (RGB888, display+detection)
    size_t              _cam_buf_size;        // Buffer size in bytes
    uint8_t            *_v4l2_buffers[2];     // V4L2 mmap'd buffers
    uint32_t             _v4l2_buf_len[2];    // V4L2 buffer lengths
    uint32_t             _v4l2_buf_count;     // Number of V4L2 buffers
    uint32_t             _cam_width;
    uint32_t             _cam_height;
    uint32_t             _cam_pixel_format;

    /* LVGL objects */
    lv_obj_t           *_cam_canvas;          // LVGL canvas for camera preview
    lv_timer_t         *_refresh_timer;       // Refresh timer (30Hz)

    /* Back button */
    lv_obj_t           *_btn_back;            // Back button overlay

    volatile bool        _cam_running;

    /* Detection subsystem */
    COCODetect                 *_detector;           // COCO detection instance
    std::list<dl::detect::result_t> _detect_results; // Latest detection results
    volatile bool               _detect_available;   // New results available? (cross-core)
    TaskHandle_t                _detect_task_handle; // Detection FreeRTOS task
    SemaphoreHandle_t           _detect_mutex;       // Mutex for results
    static constexpr float      PERSON_SCORE_THRESHOLD = 0.35f;
    static constexpr int        DETECT_INTERVAL_MS = 600;
    static constexpr int        BOX_LINE_WIDTH = 2;

    /* Draw helper: draw hollow rectangle directly on canvas buffer */
    void _draw_box_on_canvas(int x1, int y1, int x2, int y2, lv_color_t color);
};
