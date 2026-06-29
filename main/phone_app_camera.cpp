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
#include "coco_detect.hpp"
#include "dl_image_define.hpp"
#include "dl_image_ppa.hpp"

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
    _cam_running(false),
    _detector(nullptr),
    _detect_available(false), _detect_task_handle(nullptr), _detect_mutex(nullptr),
    _ppa_handle(nullptr), _ppa_buf(nullptr), _ppa_buf_size(0)
{
}

PhoneAppCamera::~PhoneAppCamera()
{
    _deinit_camera();
}

bool PhoneAppCamera::run(void)
{
    ESP_LOGI(TAG, "Camera app starting...");
    lv_obj_t *screen = lv_scr_act();

    /* Camera preview canvas (full sensor resolution, clipped to display) */
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

    /* Set canvas buffer to camera frame buffer (800x800, display clips to 720x720) */
    lv_canvas_set_buffer(_cam_canvas, _cam_buffer, EXAMPLE_CAM_SENSOR_HRES, EXAMPLE_CAM_SENSOR_VRES,
                         LV_COLOR_FORMAT_RGB888);

    /* Initialize person detection */
    if (!_init_detection()) {
        ESP_LOGW(TAG, "Detection init failed, continuing without detection");
    }

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

    /* Signal detection task to stop BEFORE deinit (avoid use-after-free) */
    _cam_running = false;

    /* Wait for detection task to finish current inference and self-terminate.
     * Worst case: mid-inference (~560ms) + one cycle. 3s timeout is safe. */
    if (_detect_task_handle) {
        int timeout = 0;
        while (_detect_task_handle && timeout < 30) {
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout++;
        }
        if (_detect_task_handle) {
            ESP_LOGW(TAG, "Detection task did not exit, force-killing");
            vTaskDelete(_detect_task_handle);
        }
        _detect_task_handle = nullptr;
    }

    /* Now safe to free resources */
    _deinit_detection();
    _deinit_camera();

    ESP_LOGI(TAG, "Camera app closed");
    return true;
}

/*============================================================================
 * Camera Hardware Init / Deinit (V4L2 via esp_video)
 *============================================================================*/

bool PhoneAppCamera::_init_camera(void)
{
    int stream_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;  // Declared early to avoid goto-cross-init
    esp_err_t ret;

    /* Init video pipeline (CSI + ISP) via esp_video. Safe to call multiple times. */
    ESP_RETURN_ON_FALSE(example_video_init() == ESP_OK, false, TAG, "example_video_init failed");

    /* Open V4L2 device */
    _video_fd = open(EXAMPLE_CAM_DEV_PATH, O_RDWR);
    if (_video_fd < 0) {
        ESP_LOGE(TAG, "Failed to open %s", EXAMPLE_CAM_DEV_PATH);
        return false;
    }

    /* Get current format (sensor resolution from esp_video_init) */
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(_video_fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
        ::close(_video_fd); _video_fd = -1;
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

    /* Allocate display/detection buffer (RGB888, 3 bytes per pixel) */
    _cam_buf_size = _cam_width * _cam_height * 3;
    _cam_buffer = heap_caps_aligned_alloc(128, _cam_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_cam_buffer) {
        ESP_LOGE(TAG, "Failed to allocate camera buffer (%zu bytes)", _cam_buf_size);
        ::close(_video_fd);
        _video_fd = -1;
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
        free(_cam_buffer); _cam_buffer = nullptr;
        ::close(_video_fd); _video_fd = -1;
        return false;
    }
    _v4l2_buf_count = req.count;
    ESP_LOGI(TAG, "V4L2 buffers allocated: %" PRIu32, _v4l2_buf_count);

    /* Map and enqueue each buffer */
    for (uint32_t i = 0; i < _v4l2_buf_count; i++) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(_video_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF[%" PRIu32 "] failed", i);
            goto fail;
        }
        _v4l2_buffers[i] = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                             MAP_SHARED, _video_fd, buf.m.offset);
        if (_v4l2_buffers[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap[%" PRIu32 "] failed", i);
            _v4l2_buffers[i] = nullptr;
            goto fail;
        }
        _v4l2_buf_len[i] = buf.length;
        if (ioctl(_video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF[%" PRIu32 "] failed", i);
            goto fail;
        }
    }

    /* Start streaming */
    if (ioctl(_video_fd, VIDIOC_STREAMON, &stream_type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        goto fail;
    }

    _cam_running = true;
    ESP_LOGI(TAG, "V4L2 camera pipeline started (%" PRIu32 "x%" PRIu32 ")", _cam_width, _cam_height);
    return true;

fail:
    for (uint32_t i = 0; i < _v4l2_buf_count; i++) {
        if (_v4l2_buffers[i]) {
            munmap(_v4l2_buffers[i], _v4l2_buf_len[i]);
            _v4l2_buffers[i] = nullptr;
        }
    }
    free(_cam_buffer); _cam_buffer = nullptr;
    ::close(_video_fd); _video_fd = -1;
    return false;
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

    return true;
}

/*============================================================================
 * Detection Subsystem
 *============================================================================*/

bool PhoneAppCamera::_init_detection(void)
{
    /* Create mutex for detection results */
    _detect_mutex = xSemaphoreCreateMutex();
    if (!_detect_mutex) {
        ESP_LOGE(TAG, "Failed to create detect mutex");
        return false;
    }

    /* Create COCODetect instance (YOLO11n 320x320 for P4) */
    _detector = new COCODetect(COCODetect::YOLO11N_320_S8_V1, true);  // lazy load

    /* Register PPA SRM client for hardware-accelerated image resize */
    ppa_client_config_t ppa_cfg = {PPA_OPERATION_SRM, 0, PPA_DATA_BURST_LENGTH_128};
    ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, (ppa_client_handle_t *)&_ppa_handle));
    ESP_LOGI(TAG, "PPA SRM client registered");

    /* Allocate PPA output buffer (320x320 RGB888) */
    _ppa_buf_size = 320 * 320 * 3;
    _ppa_buf = dl::image::alloc_ppa_outbuf(_ppa_buf_size);
    if (!_ppa_buf) {
        ESP_LOGE(TAG, "Failed to allocate PPA output buffer, falling back to CPU resize");
        ppa_unregister_client((ppa_client_handle_t)_ppa_handle);
        _ppa_handle = nullptr;
    }
    if (!_detector) {
        ESP_LOGE(TAG, "Failed to create COCODetect instance");
        if (_ppa_buf) { free(_ppa_buf); _ppa_buf = nullptr; }
        if (_ppa_handle) { ppa_unregister_client((ppa_client_handle_t)_ppa_handle); _ppa_handle = nullptr; }
        vSemaphoreDelete(_detect_mutex);
        _detect_mutex = nullptr;
        return false;
    }
    _detector->set_score_thr(PERSON_SCORE_THRESHOLD);

    /* Create detection task on core 0 (high-performance core for NPU inference) */
    BaseType_t ret = xTaskCreatePinnedToCore(
        _detection_task, "detect", 16 * 1024, this, 2, &_detect_task_handle, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create detection task");
        delete _detector;
        _detector = nullptr;
        if (_ppa_buf) { free(_ppa_buf); _ppa_buf = nullptr; }
        if (_ppa_handle) { ppa_unregister_client((ppa_client_handle_t)_ppa_handle); _ppa_handle = nullptr; }
        vSemaphoreDelete(_detect_mutex);
        _detect_mutex = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "Detection initialized (YOLO11n 320x320)");
    return true;
}

void PhoneAppCamera::_deinit_detection(void)
{
    /* Detection task should already be stopped by close() via _cam_running=false.
     * Just clean up resources here. */
    _detect_task_handle = nullptr;

    /* Delete detector */
    if (_detector) {
        delete _detector;
        _detector = nullptr;
    }

    /* Free PPA output buffer and deregister client */
    if (_ppa_buf) {
        free(_ppa_buf);
        _ppa_buf = nullptr;
    }
    if (_ppa_handle) {
        ppa_unregister_client((ppa_client_handle_t)_ppa_handle);
        _ppa_handle = nullptr;
    }

    /* Delete mutex */
    if (_detect_mutex) {
        vSemaphoreDelete(_detect_mutex);
        _detect_mutex = nullptr;
    }

    _detect_available = false;
    ESP_LOGI(TAG, "Detection deinitialized");
}

void PhoneAppCamera::_detection_task(void *arg)
{
    PhoneAppCamera *app = (PhoneAppCamera *)arg;
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        if (!app->_cam_running || !app->_detector || !app->_cam_buffer) {
            if (!app->_cam_running) {
                /* Camera is shutting down — self-terminate gracefully */
                app->_detect_task_handle = nullptr;
                vTaskDelete(NULL);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Sync cache: refresh timer (core 1) wrote to _cam_buffer via memcpy from V4L2.
         * Invalidate CPU cache to see fresh data (PSRAM needs explicit sync on P4). */
        esp_cache_msync(app->_cam_buffer, app->_cam_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        /* Use PPA hardware to resize RGB888 800x800 → 320x320 directly from camera buffer */
        dl::image::img_t img;
        if (app->_ppa_handle && app->_ppa_buf) {
            dl::image::img_t src = {
                .data = app->_cam_buffer,
                .width = EXAMPLE_CAM_SENSOR_HRES,
                .height = EXAMPLE_CAM_SENSOR_VRES,
                .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
            };
            dl::image::img_t dst = {
                .data = app->_ppa_buf,
                .width = 320,
                .height = 320,
                .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
            };
            if (dl::image::resize_ppa(src, dst, (ppa_client_handle_t)app->_ppa_handle) == ESP_OK) {
                img = dst;  // PPA output: 320x320 RGB888 → preprocessor skips resize, only quantizes
            } else {
                img = src;  // PPA failed, fall back to CPU resize in ImagePreprocessor
            }
        } else {
            img = {
                .data = app->_cam_buffer,
                .width = EXAMPLE_CAM_SENSOR_HRES,
                .height = EXAMPLE_CAM_SENSOR_VRES,
                .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888,
            };
        }

        /* Run detection (first call loads model ~11s, subsequent ~560ms) */
        std::list<dl::detect::result_t> &results = app->_detector->run(img);

        /* Filter for person class (COCO class 0) only, scale coords to 800x800 canvas */
        constexpr float SCALE = (float)EXAMPLE_CAM_SENSOR_HRES / 320.0f;  // 800/320 = 2.5
        bool ppa_used = (img.width == 320 && app->_ppa_handle != nullptr);
        if (xSemaphoreTake(app->_detect_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            app->_detect_results.clear();
            for (auto &r : results) {
                if (r.category == 0 && r.score >= PERSON_SCORE_THRESHOLD) {
                    if (ppa_used) {
                        /* Scale box coords from 320x320 → 800x800 canvas space */
                        for (int i = 0; i < 4; i++) {
                            r.box[i] = (int)(r.box[i] * SCALE);
                        }
                    }
                    app->_detect_results.push_back(r);
                }
            }
            app->_detect_available = true;
            xSemaphoreGive(app->_detect_mutex);
        }

        /* Wait for next detection cycle */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(DETECT_INTERVAL_MS));
    }
}

/*============================================================================
 * Draw helper: hollow rectangle directly on canvas buffer via set_px
 *============================================================================*/

void PhoneAppCamera::_draw_box_on_canvas(int x1, int y1, int x2, int y2, lv_color_t color)
{
    /* Clamp to canvas bounds */
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= EXAMPLE_CAM_SENSOR_HRES) x2 = EXAMPLE_CAM_SENSOR_HRES - 1;
    if (y2 >= EXAMPLE_CAM_SENSOR_VRES) y2 = EXAMPLE_CAM_SENSOR_VRES - 1;
    if (x1 > x2 || y1 > y2) return;

    /* Write directly to canvas buffer (RGB888: LVGL stores as BGR in memory) */
    uint8_t *buf = (uint8_t *)_cam_buffer;
    int stride = EXAMPLE_CAM_SENSOR_HRES * 3;  // 3 bytes per pixel
    /* LVGL stores RGB888 as BGR: data[0]=blue, data[1]=green, data[2]=red */
    uint8_t r = color.red;
    uint8_t g = color.green;
    uint8_t b = color.blue;

    /* Top and bottom horizontal lines */
    for (int w = 0; w < BOX_LINE_WIDTH; w++) {
        int row_top = y1 + w;
        int row_bot = y2 - w;
        for (int x = x1; x <= x2; x++) {
            int off_top = row_top * stride + x * 3;
            int off_bot = row_bot * stride + x * 3;
            buf[off_top + 0] = b; buf[off_top + 1] = g; buf[off_top + 2] = r;
            buf[off_bot + 0] = b; buf[off_bot + 1] = g; buf[off_bot + 2] = r;
        }
    }

    /* Left and right vertical lines (between top/bottom borders) */
    int y_start = y1 + BOX_LINE_WIDTH;
    int y_end = y2 - BOX_LINE_WIDTH;
    for (int w = 0; w < BOX_LINE_WIDTH; w++) {
        int col_l = x1 + w;
        int col_r = x2 - w;
        for (int y = y_start; y <= y_end; y++) {
            int off_l = y * stride + col_l * 3;
            int off_r = y * stride + col_r * 3;
            buf[off_l + 0] = b; buf[off_l + 1] = g; buf[off_l + 2] = r;
            buf[off_r + 0] = b; buf[off_r + 1] = g; buf[off_r + 2] = r;
        }
    }
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

    /* Copy frame to display buffer.
     * The V4L2 buffer format depends on the sensor/ISP pipeline config.
     * For OV5647 RAW8 → ISP → the output may be YUV422P or RGB888.
     * We copy raw bytes; the canvas is set up for the actual format. */
    uint32_t copy_size = buf.bytesused;
    if (copy_size > app->_cam_buf_size) copy_size = app->_cam_buf_size;
    memcpy(app->_cam_buffer, app->_v4l2_buffers[buf.index], copy_size);

    /* Return buffer to V4L2 queue */
    ioctl(app->_video_fd, VIDIOC_QBUF, &buf);

    /* Cache sync: CPU wrote to _cam_buffer, make it visible to display DMA */
    esp_cache_msync(app->_cam_buffer, app->_cam_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /* Draw detection boxes directly on canvas buffer pixels (no LVGL objects) */
    if (app->_detect_available && app->_detect_mutex &&
        xSemaphoreTake(app->_detect_mutex, 0) == pdTRUE) {

        if (!app->_detect_results.empty()) {
            lv_color_t green = lv_palette_main(LV_PALETTE_GREEN);
            for (auto &r : app->_detect_results) {
                app->_draw_box_on_canvas(r.box[0], r.box[1], r.box[2], r.box[3], green);
            }
            esp_cache_msync(app->_cam_buffer, app->_cam_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }

        xSemaphoreGive(app->_detect_mutex);
    }

    /* Trigger LVGL to redraw canvas + overlays */
    lv_obj_invalidate(app->_cam_canvas);
}
