#pragma once

#include "esp_brookesia.hpp"
#include "linux/videodev2.h"

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
    bool _init_camera(void);
    bool _deinit_camera(void);
    void _cleanup_camera_init(void);

    /* V4L2 camera handles */
    int                  _video_fd;
    void               *_cam_buffer;          // Camera frame buffer in PSRAM (RGB565, display only)
    size_t              _cam_buf_size;
    uint8_t            *_v4l2_buffers[2];
    uint32_t             _v4l2_buf_len[2];
    uint32_t             _v4l2_buf_count;
    uint32_t             _cam_width;
    uint32_t             _cam_height;
    uint32_t             _cam_pixel_format;

    /* LVGL objects */
    lv_obj_t           *_cam_canvas;
    lv_timer_t         *_refresh_timer;

    /* Back button */
    lv_obj_t           *_btn_back;

    volatile bool        _cam_running;
    bool                 _video_initialized;
};
