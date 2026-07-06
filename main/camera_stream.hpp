#pragma once

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "dl_detect_define.hpp"
#include "uorb.h"
#include <time.h>
#include <list>
#include <atomic>

class COCODetect;  // Forward declaration
class PPAPreprocessor;  // Forward declaration

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Write OV5647 VTS registers via I2C to reduce frame rate from ~50fps to ~2fps.
 *
 * VTS (Vertical Total Size) is the number of lines per frame including blanking.
 * Increasing VTS from 984 → 24600 (25x) proportionally reduces frame rate:
 *   Frame Rate = PCLK / (HTS × VTS) ≈ 50fps / 25 = ~2fps
 *
 * Benefits:
 *   - ISP DMA bandwidth: ~32 MB/s → ~2.6 MB/s
 *   - MIPI CSI bandwidth usage is reduced (more idle time between frames)
 *   - JPEG encoder CPU load is reduced
 *
 * Must be called after example_video_init() (sensor I2C bus is available).
 */
void ov5647_set_vts_2fps(void);

/**
 * @brief Camera stream over WiFi — V4L2 camera → JPEG encoding → HTTP MJPEG → mDNS
 *
 * Usage:
 *   CameraStream::instance().start();  // Start streaming
 *   CameraStream::instance().stop();   // Stop streaming
 *   CameraStream::instance().isRunning(); // Check state
 *
 * The stream is NOT persistent across reboots (no NVS storage).
 * Only one consumer at a time (Camera App or Camera Stream — mutually exclusive).
 */
class CameraStream {
public:
    static CameraStream& instance(void);

    /** Start: init V4L2 pipeline + HTTP server + mDNS.
     *  @return true on success */
    bool start(void);

    /** Stop: teardown HTTP server + mDNS + V4L2 pipeline */
    void stop(void);

    /** @return true if stream is currently active */
    bool isRunning(void) const { return _running; }

    /* Public members accessed by HTTP handler functions (C-style callbacks) */
    int                    _video_fd;
    uint32_t               _cam_width;
    uint32_t               _cam_height;
    uint32_t               _cam_pixel_format;
    uint8_t               *_v4l2_bufs[2];
    uint32_t               _v4l2_buf_len[2];
    uint32_t               _v4l2_buf_count;
    void                  *_encoder_handle;
    uint8_t               *_jpeg_out_buf;
    uint32_t               _jpeg_out_size;
    uint8_t                _jpeg_quality;
    SemaphoreHandle_t      _encoder_sem;
    std::atomic<bool>      _encoder_initialized;      /* Atomic: init done, safe to use _encoder_handle/_encoder_sem */
    std::atomic<bool>      _encoder_init_in_progress; /* Atomic: prevents double-init, notifies waiters */

    /* Last JPEG snapshot for /api/capture_image — written by stream handler, read by HTTP handler */
    SemaphoreHandle_t      _last_jpeg_mutex;
    uint8_t               *_last_jpeg_buf;
    uint32_t               _last_jpeg_size;
    uint32_t               _last_jpeg_capacity;

    /* Lazy JPEG encoder init — called from stream_handler (C callback, needs public access) */
    bool _init_encoder(void);
    void _deinit_encoder(void);

    /* FPS tracking — cross-core: HTTP handler (core 0) writes, LVGL timer (core 1) reads */
    std::atomic<uint32_t>  _frame_count;       /* Total frames sent */
    std::atomic<uint32_t>  _fps_frame_count;   /* Frames in current FPS window */
    struct timespec        _fps_window_start;  /* Start of current FPS window */
    std::atomic<uint32_t>  _fps_total_bytes;   /* JPEG bytes in current FPS window */
    std::atomic<orb_advert_t>  _fps_pub;           /* uORB publisher for fps_stats — atomic for lazy advertise CAS */
    static constexpr int   FPS_LOG_INTERVAL_S = 2;  /* Log FPS every 2s */

    /* Detection — inline, no separate task/buffer needed */
    std::atomic<COCODetect *>   _detector;          /* Cross-task: loader writes, handler reads */
    uint8_t                      *_detect_in_buf;   /* Copy buffer for inference */
    uint32_t                      _detect_in_size;
    std::list<dl::detect::result_t> _detect_results;
    SemaphoreHandle_t            _detect_mutex;     /* Protects _detect_results + _detect_available */
    mutable bool                 _detect_available;   /* Non-volatile: same task */
    std::atomic<bool>            _model_ready;         /* Model loaded and ready for inference (cross-task: loader→handler) */
    TaskHandle_t                 _model_load_task;     /* Background task that loads the model */
    StackType_t                 *_model_load_stack;   /* PSRAM-allocated stack (8KB) */
    StaticTask_t                *_model_load_tcb;     /* TCB buffer for static task */
    PPAPreprocessor             *_ppa;                 /* PPA hardware preprocessor (resize + RGB565→BGR888) */
    uint32_t                      _stream_enc_width;    /* JPEG encoder input width (PPA output or cam width) */
    uint32_t                      _stream_enc_height;   /* JPEG encoder input height (PPA output or cam height) */
    uint32_t                      _stream_enc_format;   /* JPEG encoder input pixel format */
    static constexpr float       PERSON_SCORE_THRESHOLD = 0.35f;
    static constexpr int         DETECT_INTERVAL_FRAMES = 3;
    static constexpr int         BOX_LINE_WIDTH = 2;

    /* Draw helpers */
    void _draw_box_on_buffer(uint8_t *buffer, uint32_t width, uint32_t height,
                             int x1, int y1, int x2, int y2, uint16_t color);
    void _draw_box_on_bgr24(uint8_t *buffer, uint32_t width, uint32_t height,
                            int x1, int y1, int x2, int y2,
                            uint8_t b, uint8_t g, uint8_t r);

    /* Run detection inference on given buffer (same task, no mutex needed) */
    void _run_inference(uint8_t *buffer, uint32_t size);

private:
    CameraStream();
    ~CameraStream();

    CameraStream(const CameraStream&) = delete;
    CameraStream& operator=(const CameraStream&) = delete;

    bool _init_video(void);
    void _deinit_video(void);
    bool _start_http_server(void);
    void _stop_http_server(void);
    void _init_mdns(void);
    void _deinit_mdns(void);

    /* Detection: init/deinit */
    bool _init_detection(void);
    void _deinit_detection(void);
    static void _model_load_task_fn(void *arg);  /* Background model loader */

    /* HTTP server */
    httpd_handle_t         _httpd_80;          // Port 80: API + info
    httpd_handle_t         _httpd_81;          // Port 81: MJPEG stream

    /* State */
    std::atomic<bool>          _running;

    /* mDNS */
    bool                   _mdns_running;
};

#ifdef __cplusplus
}
#endif
