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
#include "camera_stream.hpp"
#include "dl_image_define.hpp"

/* uORB */
#include "uorb.h"
#include "topics.h"
#include "esp_timer.h"
#include "camera_driver.hpp"

#include <cstring>

static const char *TAG = "PhoneAppCamera";

/* Use built-in brookesia launcher icon for camera */
extern const lv_image_dsc_t esp_brookesia_image_large_app_launcher_default_112_112;

/* uORB publishers */
static orb_advert_t s_detect_pub   = ORB_ADVERT_INVALID;

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
    _video_initialized(false),
    _detector(nullptr),
    _detect_in_buf(nullptr), _detect_in_size(0),
    _detect_available(false), _detect_task_handle(nullptr), _detect_stack(nullptr), _detect_tcb(nullptr), _detect_mutex(nullptr)
{
}

PhoneAppCamera::~PhoneAppCamera()
{
    /* Signal detection task to stop (same as close()) */
    _cam_running = false;

    /* Wait for detection task to finish current inference and self-terminate.
     * If close() was already called, the task should already be gone. */
    if (_detect_task_handle) {
        int timeout = 0;
        while (timeout < 10) {
            if (!_detect_task_handle) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout++;
        }
        if (_detect_task_handle) {
            ESP_LOGW(TAG, "Detection task did not exit, force-killing");
            vTaskDelete(_detect_task_handle);
            _detect_task_handle = nullptr;
        }
    }

    /* Deinit detection resources (defensive — close() normally does this) */
    _deinit_detection();

    /* Deinit camera hardware */
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

    /* Release camera hardware via CameraDriver */
    CameraDriver::instance().release("camera_app");

    /* Signal detection task to stop BEFORE deinit (avoid use-after-free) */
    _cam_running = false;

    /* Wait for detection task to finish current inference and self-terminate.
     * Worst case: mid-inference (~560ms) + one cycle. 3s timeout is safe.
     * Task self-deletes with vTaskDelete(NULL) and clears _detect_task_handle.
     * Check handle instead of eTaskGetState() — the latter races with the
     * idle task reclaiming the TCB after vTaskDelete(NULL). */
    if (_detect_task_handle) {
        int timeout = 0;
        while (timeout < 30) {
            if (!_detect_task_handle) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout++;
        }
        if (_detect_task_handle) {
            ESP_LOGW(TAG, "Detection task did not exit, force-killing");
            vTaskDelete(_detect_task_handle);
            _detect_task_handle = nullptr;
        }
    }

    /* Yield to let idle task fully reclaim the deleted task's TCB.
     * The detection task clears _detect_task_handle before vTaskDelete(NULL),
     * but there is a small window where the task is still executing its last
     * instructions (e.g., semaphore give). This delay ensures the task has
     * fully exited before we delete _detect_mutex in _deinit_detection(). */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Now safe to free resources */
    _deinit_detection();
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

    /* Reduce sensor frame rate from ~50fps → ~5fps (VTS: 984→9840).
     * ISP DMA: ~32 MB/s → ~6.4 MB/s. */
    ov5647_set_vts_10fps();

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

    /* Advertise detection_result topic */
    s_detect_pub = orb_advertise(ORB_ID(detection_result));

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
    ::close(_video_fd); _video_fd = -1;
    CameraDriver::instance().release("camera_app");
    example_video_deinit();
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

    /* Allocate private copy buffer for detection inference.
     * This allows us to memcpy the frame under mutex, then release
     * the mutex before running 560ms inference — no frame tearing. */
    _detect_in_size = _cam_buf_size;
    _detect_in_buf = (uint8_t *)heap_caps_malloc(_detect_in_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_detect_in_buf) {
        ESP_LOGE(TAG, "Failed to allocate detection input buffer (%zu bytes)", _detect_in_size);
        vSemaphoreDelete(_detect_mutex);
        _detect_mutex = nullptr;
        return false;
    }

    /* Create COCODetect instance (YOLO11n 320x320 for P4).
     * Use nothrow to prevent std::bad_alloc on OOM (no C++ exception support). */
    _detector = new (std::nothrow) COCODetect(COCODetect::YOLO11N_320_S8_V1, true);  // lazy load

    if (!_detector) {
        ESP_LOGE(TAG, "Failed to create COCODetect instance");
        vSemaphoreDelete(_detect_mutex);
        _detect_mutex = nullptr;
        return false;
    }
    _detector->set_score_thr(PERSON_SCORE_THRESHOLD);

    /* Create detection task on core 0 (high-performance core for NPU inference).
     * Use static allocation to place the 16KB stack in PSRAM, freeing internal SRAM. */
    const uint32_t detect_stack_words = 16 * 1024 / sizeof(StackType_t);
    _detect_stack = (StackType_t *)heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    _detect_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!_detect_stack || !_detect_tcb) {
        ESP_LOGE(TAG, "Failed to allocate detect task stack in PSRAM or TCB");
        if (_detect_stack) { heap_caps_free(_detect_stack); _detect_stack = nullptr; }
        if (_detect_tcb) { heap_caps_free(_detect_tcb); _detect_tcb = nullptr; }
        delete _detector; _detector = nullptr;
        vSemaphoreDelete(_detect_mutex); _detect_mutex = nullptr;
        return false;
    }
    _detect_task_handle = xTaskCreateStaticPinnedToCore(
        _detection_task, "detect", detect_stack_words, this, 2, _detect_stack, _detect_tcb, 0);
    if (_detect_task_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to create detection task");
        heap_caps_free(_detect_stack); _detect_stack = nullptr;
        heap_caps_free(_detect_tcb); _detect_tcb = nullptr;
        delete _detector; _detector = nullptr;
        vSemaphoreDelete(_detect_mutex); _detect_mutex = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "Detection initialized (YOLO11n 320x320)");
    return true;
}

void PhoneAppCamera::_deinit_detection(void)
{
    /* Detection task should already be stopped by close() via _cam_running=false.
     * Just clean up resources here.
     * Yield to let any in-flight vTaskDelete(NULL) complete before freeing stack. */
    _detect_task_handle = nullptr;
    vTaskDelay(1);

    /* Free statically-allocated task stack (PSRAM) and TCB */
    if (_detect_stack) {
        heap_caps_free(_detect_stack);
        _detect_stack = nullptr;
    }
    if (_detect_tcb) {
        heap_caps_free(_detect_tcb);
        _detect_tcb = nullptr;
    }

    /* Reset uORB publisher handle for clean re-init lifecycle */
    s_detect_pub = ORB_ADVERT_INVALID;

    /* Delete detector */
    if (_detector) {
        delete _detector;
        _detector = nullptr;
    }

    /* Free private copy buffer */
    if (_detect_in_buf) {
        heap_caps_free(_detect_in_buf);
        _detect_in_buf = nullptr;
        _detect_in_size = 0;
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

        /* Sync cache + copy frame under mutex, then release mutex before
         * running 560ms inference. This prevents frame tearing: the LVGL
         * timer can write to _cam_buffer while we inference on our private copy. */
        dl::image::img_t img = {};  /* Declared outside if-block for scope */
        if (app->_detect_mutex &&
            xSemaphoreTake(app->_detect_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            esp_cache_msync(app->_cam_buffer, app->_cam_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

            /* Copy frame to private buffer while holding mutex */
            memcpy(app->_detect_in_buf, app->_cam_buffer, app->_detect_in_size);

            xSemaphoreGive(app->_detect_mutex);

            /* Set up image descriptor pointing to our private copy */
            img = {
                .data = app->_detect_in_buf,
                .width = (uint16_t)app->_cam_width,
                .height = (uint16_t)app->_cam_height,
                .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
            };
        } else {
            /* Skip this detection cycle if mutex unavailable */
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(DETECT_INTERVAL_MS));
            continue;
        }

        /* Run detection (first call loads model ~11s, subsequent ~560ms) */
        std::list<dl::detect::result_t> &results = app->_detector->run(img);

        /* Filter for person class (COCO class 0).
         * COCODetect::run() internally handles coordinate scaling from model
         * space to input image size, so results are already in camera resolution. */
        int person_count = 0;
        if (xSemaphoreTake(app->_detect_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            app->_detect_results.clear();
            for (auto &r : results) {
                if (r.category == 0 && r.score >= PERSON_SCORE_THRESHOLD) {
                    app->_detect_results.push_back(r);
                    person_count++;
                }
            }
            app->_detect_available = true;
            xSemaphoreGive(app->_detect_mutex);
        }

        /* Publish detection result via uORB */
        if (s_detect_pub >= 0) {
            struct detection_result_s dr = {};
            dr.timestamp    = esp_timer_get_time();
            dr.person_count = person_count;
            orb_publish(ORB_ID(detection_result), s_detect_pub, &dr);
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
    /* Clamp to canvas bounds (use actual V4L2 resolution) */
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= (int)_cam_width) x2 = (int)_cam_width - 1;
    if (y2 >= (int)_cam_height) y2 = (int)_cam_height - 1;
    if (x1 > x2 || y1 > y2) return;

    /* RGB565: 2 bytes per pixel, each pixel is uint16_t */
    uint16_t *buf = (uint16_t *)_cam_buffer;
    int stride = (int)_cam_width;
    uint16_t c = lv_color_to_u16(color);

    /* Top and bottom horizontal lines */
    for (int w = 0; w < BOX_LINE_WIDTH; w++) {
        int row_top = y1 + w;
        int row_bot = y2 - w;
        for (int x = x1; x <= x2; x++) {
            buf[row_top * stride + x] = c;
            buf[row_bot * stride + x] = c;
        }
    }

    /* Left and right vertical lines (between top/bottom borders) */
    int y_start = y1 + BOX_LINE_WIDTH;
    int y_end = y2 - BOX_LINE_WIDTH;
    for (int w = 0; w < BOX_LINE_WIDTH; w++) {
        int col_l = x1 + w;
        int col_r = x2 - w;
        for (int y = y_start; y <= y_end; y++) {
            buf[y * stride + col_l] = c;
            buf[y * stride + col_r] = c;
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
     * _cam_buffer is shared between LVGL timer (core 1) and detection
     * task (core 0). Take _detect_mutex during copy to prevent
     * detection reading a partially-written frame (tearing). */
    uint32_t copy_size = buf.bytesused;
    if (copy_size > app->_cam_buf_size) copy_size = app->_cam_buf_size;
    {
        /* Use detect mutex to synchronize with detection task.
         * Detection task takes this mutex when writing _detect_results
         * and also reads _cam_buffer for inference. By holding the mutex
         * during memcpy, we prevent the detection task from starting
         * inference on a partially-updated frame. */
        if (app->_detect_mutex &&
            xSemaphoreTake(app->_detect_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            memcpy(app->_cam_buffer, app->_v4l2_buffers[buf.index], copy_size);
            xSemaphoreGive(app->_detect_mutex);
        } else {
            /* Mutex unavailable (detection holding it for inference):
             * skip this frame copy to avoid tearing — V4L2 still gets QBUF. */
        }
    }

    /* Return buffer to V4L2 queue */
    ioctl(app->_video_fd, VIDIOC_QBUF, &buf);

    /* Cache sync: CPU wrote to _cam_buffer, make it visible to display DMA */
    esp_cache_msync(app->_cam_buffer, app->_cam_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /* Draw detection boxes directly on canvas buffer pixels (no LVGL objects) */
    if (app->_detect_available && app->_detect_mutex &&
        xSemaphoreTake(app->_detect_mutex, 0) == pdTRUE) {

        if (!app->_detect_results.empty()) {
            static const lv_color_t green = lv_palette_main(LV_PALETTE_GREEN);
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
