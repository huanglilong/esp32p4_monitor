#include "phone_app_camera.hpp"
#include "private/esp_brookesia_utils.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "sys/mman.h"
#include <fcntl.h>
#include <unistd.h>
#include "example_video_common.h"
extern "C" {
#include "bsp/esp-bsp.h"
}
#include "example_config.h"
#include "camera_stream.hpp"
#include "camera_driver.hpp"

#include <cstring>

static const char *TAG = "PhoneAppCamera";

/* Use built-in brookesia launcher icon for camera */
extern const lv_image_dsc_t esp_brookesia_image_large_app_launcher_default_112_112;

PhoneAppCamera::PhoneAppCamera(bool use_status_bar, bool use_navigation_bar) :
    ESP_Brookesia_PhoneApp("Camera", &esp_brookesia_image_large_app_launcher_default_112_112,
                           true /* use_default_screen */,
                           use_status_bar, use_navigation_bar),
    _video_fd(-1),
    _cam_buffer(nullptr), _cam_buf_size(0),
    _v4l2_buffers{nullptr, nullptr},
    _v4l2_buf_len{0, 0},
    _v4l2_buf_count(0),
    _cam_width(0), _cam_height(0), _cam_pixel_format(0),
    _cam_canvas(nullptr), _refresh_timer(nullptr), _btn_back(nullptr),
    _cam_running{false},
    _video_initialized(false)
{
}

PhoneAppCamera::~PhoneAppCamera()
{
    /* Signal stop and deinit (defensive — close() normally does this) */
    _cam_running = false;
    _deinit_camera();
}

bool PhoneAppCamera::run(void)
{
    ESP_LOGI(TAG, "Camera app starting...");
    lv_obj_t *screen = lv_scr_act();

    /* Camera preview canvas (sensor resolution, clipped to display) */
    _cam_canvas = lv_canvas_create(screen);
    lv_obj_set_size(_cam_canvas, EXAMPLE_CAM_SENSOR_HRES, EXAMPLE_CAM_SENSOR_VRES);
    lv_obj_align(_cam_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(_cam_canvas, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_cam_canvas, LV_OPA_COVER, 0);

    /* Back button overlay (top-left corner) */
    _btn_back = lv_btn_create(screen);
    lv_obj_set_size(_btn_back, 60, 60);
    lv_obj_align(_btn_back, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_bg_color(_btn_back, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(_btn_back, LV_OPA_70, 0);
    lv_obj_set_style_radius(_btn_back, 30, 0);
    lv_obj_add_flag(_btn_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(_btn_back, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_label = lv_label_create(_btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_CLOSE);
    lv_obj_center(back_label);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);

    lv_obj_add_event_cb(_btn_back, [](lv_event_t *e) {
        PhoneAppCamera *app = (PhoneAppCamera *)e->user_data;
        app->back();
    }, LV_EVENT_CLICKED, this);

    /* Initialize camera hardware */
    if (!_init_camera()) {
        ESP_LOGE(TAG, "Camera init failed");
        return false;
    }

    /* Set canvas buffer to V4L2 frame buffer (RGB565, 2 bytes/px).
     * Display clips to 720x720. */
    lv_canvas_set_buffer(_cam_canvas, _cam_buffer, (int32_t)_cam_width, (int32_t)_cam_height,
                         LV_COLOR_FORMAT_RGB565);

    /* Frame refresh timer (~30 fps) */
    _refresh_timer = lv_timer_create(_frame_update_timer_cb, 33, this);

    ESP_LOGI(TAG, "Camera app running");
    return true;
}

bool PhoneAppCamera::back(void)
{
    ESP_LOGI(TAG, "Camera app back");
    ESP_BROOKESIA_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
    return true;
}

bool PhoneAppCamera::close(void)
{
    ESP_LOGI(TAG, "Camera app closing...");
    if (_refresh_timer) {
        lv_timer_delete(_refresh_timer);
        _refresh_timer = nullptr;
    }

    /* Release camera hardware via CameraDriver */
    CameraDriver::instance().release("camera_app");

    /* Signal stop BEFORE deinit */
    _cam_running = false;

    /* Now safe to free resources */
    _deinit_camera();

    /* Release CSI/ISP pipeline (esp_video) ONLY if this app successfully
     * initialized it. If Camera App failed to claim the camera (e.g.,
     * CameraStream is running), we must NOT deinit the pipeline — it
     * belongs to the other module. Deiniting it would crash the streamer. */
    if (_video_initialized) {
        if (example_video_deinit() != ESP_OK) {
            ESP_LOGW(TAG, "example_video_deinit failed (may already be deinited)");
        }
        _video_initialized = false;
    } else {
        ESP_LOGI(TAG, "Skipping example_video_deinit — camera was not initialized by this app");
    }

    ESP_LOGI(TAG, "Camera app closed");
    return true;
}

/*============================================================================
 * Camera Hardware Init / Deinit (V4L2 via esp_video)
 *============================================================================*/

bool PhoneAppCamera::_init_camera(void)
{
    int stream_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    /* Claim camera hardware via CameraDriver FIRST, before any V4L2 operations.
     * This ensures atomic check-and-claim (no TOCTOU gap with CameraStream).
     * CameraStream::start() also claims before setting _running=true. */
    if (!CameraDriver::instance().claim("camera_app")) {
        ESP_LOGW(TAG, "Camera hardware in use, cannot open Camera App");
        return false;
    }

    /* Init video pipeline (CSI + ISP) via esp_video. Safe to call multiple times. */
    if (example_video_init() != ESP_OK) {
        ESP_LOGE(TAG, "example_video_init failed");
        CameraDriver::instance().release("camera_app");
        return false;
    }

    /* Reduce sensor frame rate from ~50fps → ~2fps (VTS: 984→24600).
     * ISP DMA: ~32 MB/s → ~2.6 MB/s. */
    ov5647_set_vts_2fps();

    /* Open V4L2 device */
    _video_fd = open(EXAMPLE_CAM_DEV_PATH, O_RDWR);
    if (_video_fd < 0) {
        ESP_LOGE(TAG, "Failed to open %s", EXAMPLE_CAM_DEV_PATH);
        _cleanup_camera_init();
        return false;
    }

    /* Get current format (sensor resolution from esp_video_init) */
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(_video_fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
        _cleanup_camera_init();
        return false;
    }

    _cam_width = fmt.fmt.pix.width;
    _cam_height = fmt.fmt.pix.height;
    _cam_pixel_format = fmt.fmt.pix.pixelformat;
    ESP_LOGI(TAG, "V4L2 format: %" PRIu32 "x%" PRIu32 " %c%c%c%c",
             _cam_width, _cam_height,
             (char)(_cam_pixel_format & 0xFF),
             (char)((_cam_pixel_format >> 8) & 0xFF),
             (char)((_cam_pixel_format >> 16) & 0xFF),
             (char)((_cam_pixel_format >> 24) & 0xFF));

    /* Allocate display/detection buffer (RGB565, 2 bytes per pixel) */
    _cam_buf_size = _cam_width * _cam_height * 2;
    _cam_buffer = heap_caps_aligned_alloc(128, _cam_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_cam_buffer) {
        ESP_LOGE(TAG, "Failed to allocate camera buffer (%zu bytes)", _cam_buf_size);
        _cleanup_camera_init();
        return false;
    }
    memset(_cam_buffer, 0x00, _cam_buf_size);

    /* Request V4L2 buffers (double-buffer) */
    struct v4l2_requestbuffers req = {};
    req.count = 2;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(_video_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
        _cleanup_camera_init();
        return false;
    }
    _v4l2_buf_count = req.count;
    ESP_LOGI(TAG, "V4L2 buffers allocated: %" PRIu32, _v4l2_buf_count);

    /* Map and enqueue each buffer */
    bool buffers_ok = true;
    for (uint32_t i = 0; i < _v4l2_buf_count && buffers_ok; i++) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(_video_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF[%" PRIu32 "] failed", i);
            buffers_ok = false;
            break;
        }
        _v4l2_buffers[i] = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                             MAP_SHARED, _video_fd, buf.m.offset);
        if (_v4l2_buffers[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap[%" PRIu32 "] failed", i);
            _v4l2_buffers[i] = nullptr;
            buffers_ok = false;
            break;
        }
        _v4l2_buf_len[i] = buf.length;
        if (ioctl(_video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF[%" PRIu32 "] failed", i);
            buffers_ok = false;
            break;
        }
    }
    if (!buffers_ok) {
        _cleanup_camera_init();
        return false;
    }

    /* Start streaming */
    if (ioctl(_video_fd, VIDIOC_STREAMON, &stream_type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        _cleanup_camera_init();
        return false;
    }

    _cam_running = true;
    _video_initialized = true;
    ESP_LOGI(TAG, "V4L2 camera pipeline started (%" PRIu32 "x%" PRIu32 ")", _cam_width, _cam_height);

    return true;
}

/* Helper: clean up partial camera init state on failure. */
void PhoneAppCamera::_cleanup_camera_init(void)
{
    for (uint32_t i = 0; i < _v4l2_buf_count; i++) {
        if (_v4l2_buffers[i]) {
            munmap(_v4l2_buffers[i], _v4l2_buf_len[i]);
            _v4l2_buffers[i] = nullptr;
        }
    }
    free(_cam_buffer); _cam_buffer = nullptr;
    if (_video_fd >= 0) { ::close(_video_fd); _video_fd = -1; }
    CameraDriver::instance().release("camera_app");
    /* Only deinit the video pipeline if this app successfully initialized it.
     * Unconditional deinit would tear down a pipeline that another module
     * (e.g. CameraStream) depends on, even though claim/release prevents
     * concurrent access. */
    if (_video_initialized) {
        example_video_deinit();
        _video_initialized = false;
    }
}

bool PhoneAppCamera::_deinit_camera(void)
{
    _cam_running = false;

    if (_video_fd >= 0) {
        /* Stop streaming */
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(_video_fd, VIDIOC_STREAMOFF, &type);

        /* Unmap buffers */
        for (uint32_t i = 0; i < _v4l2_buf_count; i++) {
            if (_v4l2_buffers[i]) {
                munmap(_v4l2_buffers[i], _v4l2_buf_len[i]);
                _v4l2_buffers[i] = nullptr;
            }
        }
        _v4l2_buf_count = 0;

        ::close(_video_fd);
        _video_fd = -1;
    }

    /* Free display buffer */
    if (_cam_buffer) {
        free(_cam_buffer);
        _cam_buffer = nullptr;
    }

    /* Delete refresh timer if still alive (prevents leak when close() not called) */
    if (_refresh_timer) {
        lv_timer_delete(_refresh_timer);
        _refresh_timer = nullptr;
    }

    return true;
}

/*============================================================================
 * Frame Update Timer (30fps) — V4L2 DQBUF → copy to canvas → QBUF
 *============================================================================*/

void PhoneAppCamera::_frame_update_timer_cb(lv_timer_t *timer)
{
    PhoneAppCamera *app = (PhoneAppCamera *)timer->user_data;
    if (!app || !app->_cam_running || app->_video_fd < 0 || !app->_cam_buffer) {
        return;
    }

    /* Dequeue a frame from V4L2 */
    struct v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(app->_video_fd, VIDIOC_DQBUF, &buf) != 0) {
        return;  // No frame available yet
    }
    if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
        ioctl(app->_video_fd, VIDIOC_QBUF, &buf);
        return;
    }

    /* Copy frame to display buffer */
    uint32_t copy_size = buf.bytesused;
    if (copy_size > app->_cam_buf_size) copy_size = app->_cam_buf_size;
    memcpy(app->_cam_buffer, app->_v4l2_buffers[buf.index], copy_size);

    /* Return buffer to V4L2 queue */
    ioctl(app->_video_fd, VIDIOC_QBUF, &buf);

    /* Cache sync: CPU wrote to _cam_buffer, make it visible to display DMA */
    esp_cache_msync(app->_cam_buffer, app->_cam_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /* Trigger LVGL to redraw canvas */
    lv_obj_invalidate(app->_cam_canvas);
}
