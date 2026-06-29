#pragma once

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

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

private:
    CameraStream();
    ~CameraStream();

    CameraStream(const CameraStream&) = delete;
    CameraStream& operator=(const CameraStream&) = delete;

    /* Internal */
    bool _init_video(void);
    void _deinit_video(void);
    bool _start_http_server(void);
    void _stop_http_server(void);
    void _init_mdns(void);
    void _deinit_mdns(void);

    /* HTTP server */
    httpd_handle_t         _httpd_80;          // Port 80: API + info
    httpd_handle_t         _httpd_81;          // Port 81: MJPEG stream

    /* State */
    volatile bool          _running;

    /* mDNS */
    bool                   _mdns_running;
};

#ifdef __cplusplus
}
#endif
