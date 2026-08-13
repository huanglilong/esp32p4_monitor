#pragma once

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "uorb.h"
#include "generated/camera_frame_chunk.h"
#include <time.h>
#include <atomic>

class PPAPreprocessor;  // Forward declaration

/**
 * @brief Write OV5647 VTS registers via I2C to set sensor frame rate.
 *
 * VTS (Vertical Total Size) is the number of lines per frame including blanking.
 * Frame Rate = PCLK / (HTS × VTS)
 *   Default VTS=984 → ~50fps
 *   VTS=9840 → ~5fps (10x)
 *
 * Benefits of reduced frame rate:
 *   - ISP DMA bandwidth reduced proportionally
 *   - MIPI CSI bandwidth usage is reduced (more idle time between frames)
 *   - JPEG encoder CPU load is reduced
 *
 * Must be called after example_video_init() (sensor I2C bus is available).
 */
#ifdef __cplusplus
extern "C"
#endif
void ov5647_set_vts_5fps(void);

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
    bool isRunning(void) const { return _running.load(std::memory_order_acquire); }

    /** Enable/disable camera frame recording to ULog.
     *  Frames are published as camera_frame_chunk uORB topic when recording is enabled.
     *  ULogWriter must be configured to subscribe to camera_frame_chunk for this to take effect. */
    void set_recording(bool enabled);
    bool is_recording(void) const { return _recording_enabled.load(std::memory_order_acquire); }

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
    std::atomic<uint8_t>   _jpeg_quality;
    SemaphoreHandle_t      _encoder_sem;
    SemaphoreHandle_t      _encoder_lock;            /* Guards encoder lifetime (init/deinit) vs HTTP handlers */
    std::atomic<bool>      _encoder_initialized;      /* Atomic: init done, safe to use _encoder_handle/_encoder_sem */
    std::atomic<bool>      _encoder_init_in_progress; /* Atomic: prevents double-init, notifies waiters */

    /* Shared JPEG buffer — written by capture task, read by stream handler(s) */
    SemaphoreHandle_t      _shared_jpeg_mutex;     /* Protects _shared_jpeg_buf/size */
    uint8_t               *_shared_jpeg_buf;       /* Latest JPEG data (PSRAM) */
    uint32_t               _shared_jpeg_size;      /* Latest JPEG size */
    uint32_t               _shared_jpeg_capacity;  /* Buffer capacity */
    std::atomic<uint32_t>  _frame_generation;      /* Incremented each frame — stream handlers detect new frames */
    SemaphoreHandle_t      _frame_ready_sem;       /* Signaled by capture task (counting sem, max 2 = max MJPEG clients) */

    /* JPEG encoder init/deinit — called from capture task */
    bool _init_encoder(void);
    void _deinit_encoder(void);

    /* FPS tracking — cross-core: capture task writes, LVGL timer reads */
    std::atomic<uint32_t>  _frame_count;       /* Total frames captured */
    std::atomic<uint32_t>  _fps_frame_count;   /* Frames in current FPS window */
    struct timespec        _fps_window_start;  /* Start of current FPS window */
    std::atomic<uint32_t>  _fps_total_bytes;   /* JPEG bytes in current FPS window */
    std::atomic<orb_advert_t>  _fps_pub;           /* uORB publisher for fps_stats — atomic for lazy advertise CAS */
    static constexpr int   FPS_LOG_INTERVAL_S = 2;  /* Log FPS every 2s */

    /* Stream handler helpers */
    bool _send_mjpeg_part(httpd_req_t *req, uint8_t *jpeg_data, uint32_t jpeg_size,
                          char *part_buf, size_t part_buf_size);  /* Send MJPEG boundary+part, return false on error */
    void _update_fps_stats(uint32_t jpeg_size);   /* Update FPS counters and publish uORB */
    void _publish_camera_frame(uint8_t *jpeg_data, uint32_t jpeg_size);  /* Publish camera_frame_chunk uORB topics for ULog recording */
    void _store_shared_jpeg(uint8_t *jpeg_data, uint32_t jpeg_size);  /* Copy JPEG to shared buffer + signal _frame_ready_sem */

    /* PPA output dimensions — accessed by camera_info_handler */
    uint32_t               _stream_enc_width;    /* JPEG encoder input width (PPA output or cam width) */
    uint32_t               _stream_enc_height;   /* JPEG encoder input height (PPA output or cam height) */
    uint32_t               _stream_enc_format;   /* JPEG encoder input pixel format */

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

    /* PPA hardware preprocessor: resize camera frames for lower-bandwidth JPEG encoding */
    bool _init_ppa(void);
    void _deinit_ppa(void);
    PPAPreprocessor             *_ppa;                 /* PPA hardware preprocessor (resize, RGB565→RGB565) */

    /* Capture task: independent frame capture/encode/publish loop */
    static void _capture_task_fn(void *arg);
    std::atomic<TaskHandle_t>  _capture_task;       /* Atomic: task writes nullptr on exit, stop() reads */
    std::atomic<bool>          _capture_task_exited{false}; /* Atomic: task sets true before vTaskDelete, stop() waits for this before freeing stack */
    std::atomic<StackType_t *> _capture_stack{nullptr}; /* Atomic: start() writes (release), stop() frees (acquire). Protects against concurrent _capture_task_fn self-delete and stop() freeing stack. */
    StaticTask_t              *_capture_tcb;        /* TCB buffer for static task (internal SRAM) */

    /* HTTP server */
    httpd_handle_t         _httpd_80;          // Port 80: API + info
    httpd_handle_t         _httpd_81;          // Port 81: MJPEG stream

    /* State */
    std::atomic<bool>          _running;
    SemaphoreHandle_t          _start_stop_mutex;  /* Prevents concurrent start()/stop() — avoids race when
                                                    * cam_stop task hasn't finished cleanup and cam_start
                                                    * task begins re-initialization (EADDRINUSE, double-free) */

    /* Camera frame recording to ULog (chunked JPEG) */
    std::atomic<bool>          _recording_enabled;  /* True when camera frame recording is active */
    std::atomic<orb_advert_t>  _chunk_pub;          /* uORB publisher for camera_frame_chunk — atomic for lazy advertise CAS */

    /* mDNS */
    bool                   _mdns_running;
};
