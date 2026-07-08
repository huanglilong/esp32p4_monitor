/*
 * Camera Stream over WiFi — based on simple_video_server reference.
 * V4L2 camera → JPEG encoding → HTTP MJPEG → mDNS
 */

#include "camera_stream.hpp"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "example_video_common.h"
#include "example_config.h"
#include "mdns.h"
#include "cJSON.h"
#include "lwip/apps/netbiosns.h"
#include "driver/i2c_master.h"
#include "coco_detect.hpp"
#include "dl_image_define.hpp"
#include "ppa_preprocessor.hpp"
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

/* uORB */
#include "uorb.h"
#include "topics.h"
#include "camera_driver.hpp"

static const char *TAG = "CameraStream";

/*============================================================================
 * OV5647 VTS helper: reduce sensor frame rate from ~50fps to ~2fps
 *============================================================================*/
void ov5647_set_vts_2fps(void)
{
    i2c_master_bus_handle_t i2c_handle = bsp_i2c_get_handle();
    if (!i2c_handle) {
        ESP_LOGW(TAG, "I2C bus not available, cannot set OV5647 VTS");
        return;
    }

    i2c_master_dev_handle_t dev_handle = nullptr;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x36,  /* OV5647 SCCB address */
        .scl_speed_hz = 100000,
    };

    esp_err_t ret = i2c_master_bus_add_device(i2c_handle, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add I2C device for OV5647 VTS write: %s", esp_err_to_name(ret));
        return;
    }

    const uint16_t NEW_VTS = 24600;  /* 25x original 984 → ~2fps */
    uint8_t data[3];

    /* Write VTS high byte (register 0x380E) */
    data[0] = 0x38; data[1] = 0x0E; data[2] = (NEW_VTS >> 8) & 0xFF;
    ret = i2c_master_transmit(dev_handle, data, 3, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OV5647 VTS high write failed: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(dev_handle);
        return;
    }

    /* Write VTS low byte (register 0x380F) */
    data[0] = 0x38; data[1] = 0x0F; data[2] = NEW_VTS & 0xFF;
    ret = i2c_master_transmit(dev_handle, data, 3, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OV5647 VTS low write failed: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(dev_handle);
        return;
    }

    /* Read back VTS to verify */
    uint8_t rd_cmd[2] = {0x38, 0x0E};
    uint8_t rd_val[1] = {0};
    ret = i2c_master_transmit_receive(dev_handle, rd_cmd, 2, rd_val, 1, 100);
    if (ret == ESP_OK) {
        uint16_t vts_high = rd_val[0];
        rd_cmd[1] = 0x0F;
        ret = i2c_master_transmit_receive(dev_handle, rd_cmd, 2, rd_val, 1, 100);
        if (ret == ESP_OK) {
            uint16_t vts = (vts_high << 8) | rd_val[0];
            ESP_LOGI(TAG, "OV5647 VTS: 984→%u (target ~2 fps)", vts);
        }
    }

    i2c_master_bus_rm_device(dev_handle);
}

/* MJPEG stream constants */
#define PART_BOUNDARY  "123456789000000000000987654321"
#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=" PART_BOUNDARY
#define STREAM_BOUNDARY     "\r\n--" PART_BOUNDARY "\r\n"
#define STREAM_PART         "Content-Type: image/jpeg\r\nContent-Length: %" PRIu32 "\r\nX-Timestamp: %d.%06d\r\n\r\n"

/*============================================================================
 * Singleton
 *============================================================================*/
CameraStream& CameraStream::instance(void)
{
    static CameraStream s;
    return s;
}

CameraStream::CameraStream() :
    _video_fd(-1),
    _cam_width(0), _cam_height(0), _cam_pixel_format(0),
    _v4l2_bufs{nullptr, nullptr},
    _v4l2_buf_len{0, 0},
    _v4l2_buf_count(0),
    _encoder_handle(nullptr),
    _jpeg_out_buf(nullptr), _jpeg_out_size(0),
    _jpeg_quality{30},  // Lower quality = faster encode + less WiFi/SDIO traffic
    _encoder_sem(nullptr),
    _encoder_initialized(false),
    _encoder_init_in_progress(false),
    _last_jpeg_mutex(nullptr),
    _last_jpeg_buf(nullptr), _last_jpeg_size(0), _last_jpeg_capacity(0),
    _frame_count(0), _fps_frame_count(0),
    _fps_window_start{0, 0},
    _fps_total_bytes(0),
    _fps_pub(ORB_ADVERT_INVALID),
    _detector(nullptr),
    _detect_in_buf(nullptr), _detect_in_size(0),
    _detect_results(),
    _detect_mutex(nullptr),
    _detect_available(false),
    _model_ready(false),
    _model_load_task(nullptr),
    _model_load_task_exited(false),
    _model_load_stack(nullptr), _model_load_tcb(nullptr),
    _ppa(nullptr),
    _stream_enc_width(0), _stream_enc_height(0), _stream_enc_format(0),
    _httpd_80(nullptr), _httpd_81(nullptr),
    _running(false),
    _recording_enabled(false),
    _frame_pub(ORB_ADVERT_INVALID),
    _mdns_running(false)
{
    _detect_mutex = xSemaphoreCreateMutex();
    _last_jpeg_mutex = xSemaphoreCreateMutex();
}

CameraStream::~CameraStream()
{
    stop();
    if (_last_jpeg_mutex) {
        vSemaphoreDelete(_last_jpeg_mutex);
        _last_jpeg_mutex = nullptr;
    }
    free(_last_jpeg_buf);
    _last_jpeg_buf = nullptr;
    if (_detect_mutex) {
        vSemaphoreDelete(_detect_mutex);
        _detect_mutex = nullptr;
    }
}

/*============================================================================
 * Public API
 *============================================================================*/
bool CameraStream::start(void)
{
    /* Atomic compare-and-set: if already running, return immediately.
     * This prevents concurrent start() calls from double-initializing. */
    bool expected = false;
    if (!_running.compare_exchange_strong(expected, true)) {
        return true;  // already running
    }

    /* Claim camera hardware via CameraDriver BEFORE any resource allocation.
     * This ensures cross-module mutual exclusion from the earliest point. */
    if (!CameraDriver::instance().claim("stream")) {
        ESP_LOGW(TAG, "Camera hardware in use, cannot start stream");
        _running = false;
        return false;
    }

    if (!_init_video()) {
        ESP_LOGE(TAG, "Video init failed");
        _running = false;
        CameraDriver::instance().release("stream");
        return false;
    }

    if (!_init_detection()) {
        ESP_LOGW(TAG, "Detection init failed, continuing without detection");
    }

    if (!_start_http_server()) {
        ESP_LOGE(TAG, "HTTP server init failed");
        _deinit_video();
        _deinit_detection();
        _running = false;
        CameraDriver::instance().release("stream");
        return false;
    }

    _init_mdns();

    /* Reset FPS counters for new streaming session */
    _frame_count = 0;
    _fps_frame_count = 0;
    _fps_total_bytes = 0;
    clock_gettime(CLOCK_MONOTONIC, &_fps_window_start);

    ESP_LOGI(TAG, "Stream started → http://%s.local/stream (port 81)", shared_mdns_hostname());
    return true;
}

void CameraStream::stop(void)
{
    /* Use CAS to prevent double-cleanup — compare_exchange_strong ensures
     * only one caller transitions from running→stopped, matching start()'s pattern. */
    bool expected = true;
    if (!_running.compare_exchange_strong(expected, false)) return;

    /* _running is now false — stream handler loop (while(isRunning())) will exit.
     * httpd_stop() waits for active connections to close; if the MJPEG
     * stream loop never exits, httpd_stop() blocks indefinitely. */

    CameraDriver::instance().release("stream");
    _stop_http_server();
    _deinit_mdns();
    _deinit_detection();
    _deinit_video();

    /* Reset FPS publisher handle — will be re-created on next start() */
    _fps_pub = ORB_ADVERT_INVALID;

    /* Reset camera frame publisher handle */
    _frame_pub = ORB_ADVERT_INVALID;
    _recording_enabled.store(false, std::memory_order_release);

    ESP_LOGI(TAG, "Stream stopped");
}

/*============================================================================
 * V4L2 Video Init / Deinit
 *============================================================================*/
bool CameraStream::_init_video(void)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_format fmt = {};
    struct v4l2_streamparm sparm = {};
    struct v4l2_requestbuffers req = {};

    /* Step 1: Init video pipeline via esp_video (safe to call multiple times) */
    if (example_video_init() != ESP_OK) {
        ESP_LOGE(TAG, "example_video_init failed");
        return false;
    }

    /* Step 1b: Reduce sensor frame rate from ~50fps → ~2fps by increasing VTS. */
    ov5647_set_vts_2fps();

    bool ok = false;
    do {
        /* Step 2: Open V4L2 device */
        _video_fd = open(EXAMPLE_CAM_DEV_PATH, O_RDWR);
        if (_video_fd < 0) {
            ESP_LOGE(TAG, "Failed to open %s", EXAMPLE_CAM_DEV_PATH);
            break;
        }
        ESP_LOGI(TAG, "V4L2 device opened: %s", EXAMPLE_CAM_DEV_PATH);

        /* Step 3: Read frame rate */
        sparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(_video_fd, VIDIOC_G_PARM, &sparm) == 0) {
            struct v4l2_fract *tpf = &sparm.parm.capture.timeperframe;
            uint32_t fps = (tpf->denominator && tpf->numerator) ? tpf->denominator / tpf->numerator : 0;
            ESP_LOGI(TAG, "V4L2 frame rate: %" PRIu32 " fps (driver cached, actual ~2 fps from VTS=24600)", fps);
        } else {
            ESP_LOGW(TAG, "VIDIOC_G_PARM failed, frame rate unknown");
        }

        /* Step 4: REQBUFS */
        req = {};
        req.count = 2;  // Double buffer: at 2fps, processing (~80ms) completes well before next frame (500ms)
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        ESP_LOGI(TAG, "V4L2 REQBUFS: count=%" PRIu32, req.count);
        if (ioctl(_video_fd, VIDIOC_REQBUFS, &req) != 0) {
            ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
            break;
        }
        _v4l2_buf_count = req.count;
        ESP_LOGI(TAG, "V4L2 REQBUFS ok: got %" PRIu32 " buffers", _v4l2_buf_count);

        /* Step 5: mmap + QBUF each buffer */
        bool all_bufs_ok = true;
        for (uint32_t i = 0; i < _v4l2_buf_count; i++) {
            struct v4l2_buffer buf = {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (ioctl(_video_fd, VIDIOC_QUERYBUF, &buf) != 0) {
                ESP_LOGE(TAG, "VIDIOC_QUERYBUF[%" PRIu32 "] failed", i);
                all_bufs_ok = false;
                break;
            }
            ESP_LOGI(TAG, "V4L2 buf[%" PRIu32 "]: offset=0x%x len=%" PRIu32, i, buf.m.offset, buf.length);
            _v4l2_bufs[i] = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                              MAP_SHARED, _video_fd, buf.m.offset);
            if (_v4l2_bufs[i] == MAP_FAILED) {
                ESP_LOGE(TAG, "mmap[%" PRIu32 "] failed", i);
                _v4l2_bufs[i] = nullptr;
                all_bufs_ok = false;
                break;
            }
            _v4l2_buf_len[i] = buf.length;
            if (ioctl(_video_fd, VIDIOC_QBUF, &buf) != 0) {
                ESP_LOGE(TAG, "VIDIOC_QBUF[%" PRIu32 "] failed", i);
                all_bufs_ok = false;
                break;
            }
        }
        if (!all_bufs_ok) break;

        /* Step 6: G_FMT to read pixel format */
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(_video_fd, VIDIOC_G_FMT, &fmt) != 0) {
            ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
            break;
        }
        _cam_width = fmt.fmt.pix.width;
        _cam_height = fmt.fmt.pix.height;
        _cam_pixel_format = fmt.fmt.pix.pixelformat;
        ESP_LOGI(TAG, "V4L2 G_FMT: %" PRIu32 "x%" PRIu32 " %c%c%c%c (0x%08" PRIx32 ")",
                 _cam_width, _cam_height,
                 (char)(_cam_pixel_format & 0xFF),
                 (char)((_cam_pixel_format >> 8) & 0xFF),
                 (char)((_cam_pixel_format >> 16) & 0xFF),
                 (char)((_cam_pixel_format >> 24) & 0xFF),
                 _cam_pixel_format);

        /* Step 7: Skip JPEG encoder init here — it's done lazily on first
         * MJPEG client connection via _init_encoder().  The encoder's DMA
         * descriptors require MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL (internal SRAM),
         * which may be scarce on LCD-4B when LVGL draw buffers are also in
         * internal RAM.  Deferring to first client gives the system time to
         * settle and potentially free transient allocations. */

        /* Step 8: VIDIOC_STREAMON */
        ESP_LOGI(TAG, "V4L2 STREAMON...");
        if (ioctl(_video_fd, VIDIOC_STREAMON, &type) != 0) {
            ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
            break;
        }

        ok = true;
    } while (0);

    if (!ok) {
        for (uint32_t i = 0; i < _v4l2_buf_count; i++) {
            if (_v4l2_bufs[i]) { munmap(_v4l2_bufs[i], _v4l2_buf_len[i]); _v4l2_bufs[i] = nullptr; }
        }
        ::close(_video_fd); _video_fd = -1;
        /* Unregister CSI/ISP VFS device so next start can re-register */
        example_video_deinit();
        return false;
    }

    ESP_LOGI(TAG, "V4L2 pipeline active: %" PRIu32 "x%" PRIu32 " streaming", _cam_width, _cam_height);
    return true;
}

void CameraStream::_deinit_video(void)
{
    ESP_LOGI(TAG, "V4L2 deinit...");
    if (_video_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ESP_LOGI(TAG, "V4L2 STREAMOFF...");
        if (ioctl(_video_fd, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "VIDIOC_STREAMOFF failed");
        }

        for (uint32_t i = 0; i < _v4l2_buf_count; i++) {
            if (_v4l2_bufs[i]) {
                munmap(_v4l2_bufs[i], _v4l2_buf_len[i]);
                _v4l2_bufs[i] = nullptr;
            }
        }
        ::close(_video_fd);
        _video_fd = -1;
    }

    _deinit_encoder();

    /* Release CSI/ISP pipeline (esp_video). This unregisters the VFS device
     * so that the next Camera App or Camera Stream start can re-register it. */
    if (example_video_deinit() != ESP_OK) {
        ESP_LOGW(TAG, "example_video_deinit failed (may already be deinited)");
    }
}

/*============================================================================
 * Lazy JPEG Encoder Init/Deinit
 *
 * The JPEG hardware encoder's DMA descriptors (rxlink, txlink) require
 * MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL — they MUST be in internal SRAM.
 * On LCD-4B, internal SRAM is scarce because LVGL draw buffers also live
 * there.  By deferring encoder creation to the first MJPEG client, we:
 *   1. Give the system time to settle (transient allocations freed)
 *   2. Allow PSRAM-based draw buffers to be used (if SPIRAM_TRY_ALLOCATE_DMA_BUFFER)
 *   3. Retry with backoff if internal memory is temporarily fragmented
 *============================================================================*/
bool CameraStream::_init_encoder(void)
{
    /* Fast path: already fully initialized */
    if (_encoder_initialized.load(std::memory_order_acquire)) return true;

    /* Claim init ownership via in-progress flag.
     * Winner: proceeds to do the actual initialization.
     * Loser: spins until init is complete, then returns the result. */
    bool expected = false;
    if (!_encoder_init_in_progress.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        /* Another thread owns initialization — wait for it to finish */
        while (_encoder_init_in_progress.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return _encoder_initialized.load(std::memory_order_acquire);
    }

    /* JPEG format from sensor needs no software encoder */
    if (_cam_pixel_format == V4L2_PIX_FMT_JPEG) {
        _encoder_initialized.store(true, std::memory_order_release);
        _encoder_init_in_progress.store(false, std::memory_order_release);
        return true;
    }

    /* Determine encoder input: if PPA is active, encode from PPA output
     * (300×300 BGR24) instead of full 800×800 RGB565. This reduces:
     *   - JPEG encode time: ~200ms → ~30ms
     *   - JPEG size: ~30-50KB → ~5-8KB
     *   - WiFi bandwidth: ~80% reduction
     * PPA outputs BGR24 (PPA_SRM_COLOR_MODE_RGB888 = ESP_COLOR_FOURCC_BGR24),
     * and JPEG encoder accepts JPEG_ENCODE_IN_FORMAT_RGB888 = BGR24 — perfect match. */
#if CONFIG_SOC_PPA_SUPPORTED
    if (_ppa && _ppa->is_initialized()) {
        _stream_enc_width = _ppa->actual_width();
        _stream_enc_height = _ppa->actual_height();
        _stream_enc_format = V4L2_PIX_FMT_RGB24;  /* BGR24 in memory, matches PPA output */
    } else
#endif
    {
        _stream_enc_width = _cam_width;
        _stream_enc_height = _cam_height;
        _stream_enc_format = _cam_pixel_format;
    }

    example_encoder_config_t enc_cfg = {
        .width = _stream_enc_width,
        .height = _stream_enc_height,
        .pixel_format = _stream_enc_format,
        .quality = _jpeg_quality.load(),
    };

    /* Retry up to 3 times with increasing delay.
     * Internal SRAM may be temporarily fragmented; a short delay lets
     * other tasks free buffers (e.g., LVGL flush completes). */
    const int MAX_RETRIES = 3;
    for (int attempt = 1; attempt <= MAX_RETRIES; attempt++) {
        ESP_LOGI(TAG, "Encoder init attempt %d/%d: %" PRIu32 "x%" PRIu32 " fmt=0x%08" PRIx32 " quality=%d",
                 attempt, MAX_RETRIES, _stream_enc_width, _stream_enc_height, _stream_enc_format, _jpeg_quality.load());

        if (example_encoder_init(&enc_cfg, &_encoder_handle) == ESP_OK) {
            break;  // success
        }

        ESP_LOGW(TAG, "JPEG encoder alloc failed (attempt %d/%d), internal SRAM may be low", attempt, MAX_RETRIES);

        if (attempt < MAX_RETRIES) {
            /* Log internal memory state for debugging */
            ESP_LOGI(TAG, "Free internal: %zu bytes, free PSRAM: %zu bytes",
                     heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            vTaskDelay(pdMS_TO_TICKS(500 * attempt));  // increasing backoff
        }
    }

    if (!_encoder_handle) {
        ESP_LOGE(TAG, "Encoder init failed for format 0x%08" PRIx32 " after %d attempts",
                 _cam_pixel_format, MAX_RETRIES);
        _encoder_init_in_progress.store(false, std::memory_order_release);
        return false;
    }

    if (example_encoder_alloc_output_buffer(_encoder_handle, &_jpeg_out_buf, &_jpeg_out_size) != ESP_OK) {
        ESP_LOGE(TAG, "Encoder output buffer alloc failed");
        example_encoder_deinit(_encoder_handle);
        _encoder_handle = nullptr;
        _encoder_init_in_progress.store(false, std::memory_order_release);
        return false;
    }
    ESP_LOGI(TAG, "Encoder output buffer: %" PRIu32 " bytes", _jpeg_out_size);

    _encoder_sem = xSemaphoreCreateBinary();
    if (!_encoder_sem) {
        ESP_LOGE(TAG, "Encoder semaphore create failed");
        example_encoder_free_output_buffer(_encoder_handle, _jpeg_out_buf);
        _jpeg_out_buf = nullptr;
        example_encoder_deinit(_encoder_handle);
        _encoder_handle = nullptr;
        _encoder_init_in_progress.store(false, std::memory_order_release);
        return false;
    }
    xSemaphoreGive(_encoder_sem);

    /* All resources ready — mark initialized and release progress lock */
    _encoder_initialized.store(true, std::memory_order_release);
    _encoder_init_in_progress.store(false, std::memory_order_release);
    ESP_LOGI(TAG, "JPEG encoder initialized successfully");
    return true;
}

void CameraStream::_deinit_encoder(void)
{
    if (_encoder_handle) {
        if (_jpeg_out_buf) {
            example_encoder_free_output_buffer(_encoder_handle, _jpeg_out_buf);
            _jpeg_out_buf = nullptr;
        }
        example_encoder_deinit(_encoder_handle);
        _encoder_handle = nullptr;
    }

    if (_encoder_sem) {
        vSemaphoreDelete(_encoder_sem);
        _encoder_sem = nullptr;
    }

    /* Reset both flags — init-in-progress must be cleared so next start()
     * can re-enter _init_encoder() without seeing stale progress state. */
    _encoder_initialized.store(false, std::memory_order_release);
    _encoder_init_in_progress.store(false, std::memory_order_release);
}

/*============================================================================
 * Detection Subsystem — inline (no separate task, no extra buffer overhead)
 *============================================================================*/

/*============================================================================
 * Detection: Background Model Loader
 *============================================================================*/
void CameraStream::_model_load_task_fn(void *arg)
{
    CameraStream *cs = static_cast<CameraStream *>(arg);

    ESP_LOGI(TAG, "Model load task starting...");

    /* Create COCODetect instance (YOLO11n 320x320 for P4, lazy load) */
    cs->_detector = new (std::nothrow) COCODetect(COCODetect::YOLO11N_320_S8_V1, true);
    if (!cs->_detector) {
        ESP_LOGE(TAG, "Failed to create COCODetect instance");
        vTaskDelete(NULL);
        return;
    }
    cs->_detector.load()->set_score_thr(cs->PERSON_SCORE_THRESHOLD);

    /* Warm-up inference: trigger model loading now so the stream loop
     * isn't blocked later. PPA output buffer (all zeros) is used as
     * dummy input — COCODetect just needs any valid img to trigger load.
     * Without PPA, allocate a small dummy buffer temporarily. */
    ESP_LOGI(TAG, "Loading model (warmup inference)...");
    {
        dl::image::img_t img = {};
        uint8_t *dummy_buf = nullptr;
#if CONFIG_SOC_PPA_SUPPORTED
        if (cs->_ppa && cs->_ppa->is_initialized()) {
            img = {
                .data = cs->_ppa->out_buf(),
                .width = cs->_ppa->actual_width(),
                .height = cs->_ppa->actual_height(),
                .pix_type = dl::image::DL_IMAGE_PIX_TYPE_BGR888,
            };
        } else
#endif
        {
            dummy_buf = (uint8_t *)heap_caps_calloc(1, cs->_cam_width * cs->_cam_height * 2,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            img = {
                .data = dummy_buf,
                .width = (uint16_t)cs->_cam_width,
                .height = (uint16_t)cs->_cam_height,
                .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
            };
        }
        cs->_detector.load()->run(img);  /* First call loads model (~11s) */
        if (dummy_buf) heap_caps_free(dummy_buf);
    }
    cs->_model_ready = true;
    cs->_detect_available.store(false, std::memory_order_release);

    ESP_LOGI(TAG, "Model loaded and ready for inference");
    cs->_model_load_task_exited.store(true, std::memory_order_release);
    cs->_model_load_task.store(nullptr, std::memory_order_release);  /* Clear handle before self-deleting */
    vTaskDelete(NULL);
}

bool CameraStream::_init_detection(void)
{
    /* With PPA active: V4L2 mmap buffer is passed directly to PPA (DMA read),
     * so no separate copy buffer is needed. PPA output (BGR24) is used for
     * both COCODetect and JPEG encoding.
     * Without PPA: allocate _detect_in_buf for CPU fallback path. */
#if CONFIG_SOC_PPA_SUPPORTED
    _ppa = new (std::nothrow) PPAPreprocessor();
    if (_ppa) {
        if (!_ppa->init((uint16_t)_cam_width, (uint16_t)_cam_height, 320, 320)) {
            ESP_LOGW(TAG, "PPA init failed, falling back to CPU preprocessing");
            delete _ppa;
            _ppa = nullptr;
        } else {
            ESP_LOGI(TAG, "PPA preprocessing enabled: %d×%d RGB565 → %d×%d BGR888",
                     _cam_width, _cam_height, _ppa->actual_width(), _ppa->actual_height());
        }
    } else {
        ESP_LOGW(TAG, "PPA alloc failed, falling back to CPU preprocessing");
    }

    if (!_ppa)
#endif
    {
        _detect_in_size = _cam_width * _cam_height * 2;  // RGB565
        _detect_in_buf = (uint8_t *)heap_caps_aligned_alloc(128, _detect_in_size,
                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!_detect_in_buf) {
            ESP_LOGE(TAG, "Failed to allocate detect buffer (%" PRIu32 " bytes)", _detect_in_size);
            return false;
        }
    }

    _model_ready = false;

    /* Start background task to load model asynchronously.
     * This keeps CameraStream::start() fast and the stream loop unblocked.
     * Use static allocation to place the 8KB stack in PSRAM. */
    const uint32_t load_stack_words = 8 * 1024 / sizeof(StackType_t);
    _model_load_stack = (StackType_t *)heap_caps_malloc(8 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    _model_load_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!_model_load_stack || !_model_load_tcb) {
        ESP_LOGE(TAG, "Failed to allocate model_load task stack in PSRAM or TCB");
        if (_model_load_stack) { heap_caps_free(_model_load_stack); _model_load_stack = nullptr; }
        if (_model_load_tcb) { heap_caps_free(_model_load_tcb); _model_load_tcb = nullptr; }
        if (_detect_in_buf) { heap_caps_free(_detect_in_buf); _detect_in_buf = nullptr; }
        return false;
    }
    _model_load_task_exited.store(false, std::memory_order_release);
    _model_load_task.store(xTaskCreateStatic(
        _model_load_task_fn, "model_load", load_stack_words, this, 1, _model_load_stack, _model_load_tcb),
        std::memory_order_release);
    if (_model_load_task.load(std::memory_order_relaxed) == nullptr) {
        ESP_LOGE(TAG, "Failed to create model load task");
        heap_caps_free(_model_load_stack); _model_load_stack = nullptr;
        heap_caps_free(_model_load_tcb); _model_load_tcb = nullptr;
#if CONFIG_SOC_PPA_SUPPORTED
        if (_ppa) { delete _ppa; _ppa = nullptr; }
#endif
        if (_detect_in_buf) { heap_caps_free(_detect_in_buf); _detect_in_buf = nullptr; }
        return false;
    }

    ESP_LOGI(TAG, "Detection init done — model loading in background (PPA=%s)", _ppa ? "ON" : "OFF");
    return true;
}

void CameraStream::_deinit_detection(void)
{
    /* Stop background model-loading task if still running.
     * The task self-deletes and clears _model_load_task on completion.
     * Use atomic exchange to safely read-and-nullify the handle.
     * Poll _model_load_task_exited to avoid eTaskGetState race with idle task TCB cleanup. */
    TaskHandle_t t = _model_load_task.exchange(nullptr, std::memory_order_acq_rel);
    if (t) {
        vTaskDelete(t);
        /* Yield to let idle task reclaim TCB before we free the stack */
        int poll = 0;
        while (!_model_load_task_exited.load(std::memory_order_acquire) && poll < 50) {
            vTaskDelay(pdMS_TO_TICKS(10));
            poll++;
        }
    } else {
        /* Handle already cleared by the task — yield to let any in-flight
         * vTaskDelete(NULL) complete before freeing the stack below. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Free statically-allocated task stack (PSRAM) and TCB */
    if (_model_load_stack) {
        heap_caps_free(_model_load_stack);
        _model_load_stack = nullptr;
    }
    if (_model_load_tcb) {
        heap_caps_free(_model_load_tcb);
        _model_load_tcb = nullptr;
    }

#if CONFIG_SOC_PPA_SUPPORTED
    if (_ppa) {
        delete _ppa;
        _ppa = nullptr;
    }
#endif

    if (_detector) {
        delete _detector;
        _detector = nullptr;
    }
    if (_detect_in_buf) {
        heap_caps_free(_detect_in_buf);
        _detect_in_buf = nullptr;
    }
    _detect_in_size = 0;
    _detect_available.store(false, std::memory_order_release);
    _model_ready = false;
    _detect_results.clear();
    ESP_LOGI(TAG, "Detection deinitialized");
}

void CameraStream::_run_inference(uint8_t *buffer, uint32_t size)
{
    if (!_detector || !_model_ready) return;

    dl::image::img_t img = {};
    bool ppa_ok = false;

#if CONFIG_SOC_PPA_SUPPORTED
    /* PPA path: PPA was already called by stream_handler on the V4L2 buffer.
     * Re-use PPA output directly — no need to re-process. */
    if (_ppa && _ppa->is_initialized()) {
        img = {
            .data = _ppa->out_buf(),
            .width = _ppa->actual_width(),
            .height = _ppa->actual_height(),
            .pix_type = dl::image::DL_IMAGE_PIX_TYPE_BGR888,
        };
        ppa_ok = true;
    }
#endif

    /* CPU fallback: pass RGB565 directly, COCODetect does full preprocess */
    if (!ppa_ok) {
        if (!buffer || !_detect_in_buf) return;
        uint32_t copy_sz = (size < _detect_in_size) ? size : _detect_in_size;
        memcpy(_detect_in_buf, buffer, copy_sz);
        img = {
            .data = _detect_in_buf,
            .width = (uint16_t)_cam_width,
            .height = (uint16_t)_cam_height,
            .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
        };
    }

    /* Run detection. First call loads model (~11s), subsequent ~560ms.
     * Filter for person class (COCO class 0). */
    std::list<dl::detect::result_t> &results = _detector.load()->run(img);

#if CONFIG_SOC_PPA_SUPPORTED
    /* With PPA path, COCODetect sees actual_w×actual_h input (e.g. 300×300),
     * so result coordinates are in that space. Rescale to camera resolution. */
    const bool need_rescale = ppa_ok;
    const float rescale_x = need_rescale ? (float)_cam_width / (float)_ppa->actual_width() : 1.0f;
    const float rescale_y = need_rescale ? (float)_cam_height / (float)_ppa->actual_height() : 1.0f;
#endif

    if (_detect_mutex &&
        xSemaphoreTake(_detect_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _detect_results.clear();
        for (auto &r : results) {
            if (r.category == 0 && r.score >= PERSON_SCORE_THRESHOLD) {
#if CONFIG_SOC_PPA_SUPPORTED
                if (need_rescale) {
                    dl::detect::result_t scaled_r = r;
                    scaled_r.box[0] = (int)(r.box[0] * rescale_x);
                    scaled_r.box[1] = (int)(r.box[1] * rescale_y);
                    scaled_r.box[2] = (int)(r.box[2] * rescale_x);
                    scaled_r.box[3] = (int)(r.box[3] * rescale_y);
                    _detect_results.push_back(scaled_r);
                } else
#endif
                {
                    _detect_results.push_back(r);
                }
            }
        }
        _detect_available.store(true, std::memory_order_release);
        xSemaphoreGive(_detect_mutex);
    }
}

/*============================================================================
 * Draw helper: hollow rectangle directly on pixel buffer (RGB565)
 *============================================================================*/

void CameraStream::_draw_box_on_buffer(uint8_t *buffer, uint32_t width, uint32_t height,
                                       int x1, int y1, int x2, int y2, uint16_t color)
{
    /* Clamp to buffer bounds */
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= (int)width) x2 = (int)width - 1;
    if (y2 >= (int)height) y2 = (int)height - 1;
    if (x1 > x2 || y1 > y2) return;

    uint16_t *buf = (uint16_t *)buffer;
    int stride = (int)width;

    /* Top and bottom horizontal lines */
    for (int w = 0; w < BOX_LINE_WIDTH; w++) {
        int row_top = y1 + w;
        int row_bot = y2 - w;
        for (int x = x1; x <= x2; x++) {
            buf[row_top * stride + x] = color;
            buf[row_bot * stride + x] = color;
        }
    }

    /* Left and right vertical lines (between top/bottom borders) */
    int y_start = y1 + BOX_LINE_WIDTH;
    int y_end = y2 - BOX_LINE_WIDTH;
    for (int w = 0; w < BOX_LINE_WIDTH; w++) {
        int col_l = x1 + w;
        int col_r = x2 - w;
        for (int y = y_start; y <= y_end; y++) {
            buf[y * stride + col_l] = color;
            buf[y * stride + col_r] = color;
        }
    }
}

/*============================================================================
 * Draw helper: hollow rectangle directly on pixel buffer (BGR24 / RGB888)
 *============================================================================*/

void CameraStream::_draw_box_on_bgr24(uint8_t *buffer, uint32_t width, uint32_t height,
                                       int x1, int y1, int x2, int y2,
                                       uint8_t b, uint8_t g, uint8_t r)
{
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= (int)width) x2 = (int)width - 1;
    if (y2 >= (int)height) y2 = (int)height - 1;
    if (x1 > x2 || y1 > y2) return;

    int stride = (int)width * 3;  /* 3 bytes per pixel (BGR24) */

    /* Top and bottom horizontal lines */
    for (int w = 0; w < BOX_LINE_WIDTH; w++) {
        int row_top = y1 + w;
        int row_bot = y2 - w;
        for (int x = x1; x <= x2; x++) {
            uint8_t *px_top = buffer + row_top * stride + x * 3;
            px_top[0] = b; px_top[1] = g; px_top[2] = r;
            uint8_t *px_bot = buffer + row_bot * stride + x * 3;
            px_bot[0] = b; px_bot[1] = g; px_bot[2] = r;
        }
    }

    /* Left and right vertical lines */
    int y_start = y1 + BOX_LINE_WIDTH;
    int y_end = y2 - BOX_LINE_WIDTH;
    for (int w = 0; w < BOX_LINE_WIDTH; w++) {
        int col_l = x1 + w;
        int col_r = x2 - w;
        for (int y = y_start; y <= y_end; y++) {
            uint8_t *px_l = buffer + y * stride + col_l * 3;
            px_l[0] = b; px_l[1] = g; px_l[2] = r;
            uint8_t *px_r = buffer + y * stride + col_r * 3;
            px_r[0] = b; px_r[1] = g; px_r[2] = r;
        }
    }
}

/*============================================================================
 * Stream handler helpers — extracted from stream_handler for readability
 *============================================================================*/

/* Draw detection boxes on PPA BGR24 output buffer + cache msync */
void CameraStream::_draw_detection_boxes_on_ppa(void)
{
    if (!_detect_available.load(std::memory_order_acquire) || !_detect_mutex ||
        xSemaphoreTake(_detect_mutex, 0) != pdTRUE) return;
    if (!_detect_results.empty()) {
        for (auto &r : _detect_results) {
            int bx1 = (int)(r.box[0] * (float)_ppa->actual_width() / (float)_cam_width);
            int by1 = (int)(r.box[1] * (float)_ppa->actual_height() / (float)_cam_height);
            int bx2 = (int)(r.box[2] * (float)_ppa->actual_width() / (float)_cam_width);
            int by2 = (int)(r.box[3] * (float)_ppa->actual_height() / (float)_cam_height);
            _draw_box_on_bgr24(_ppa->out_buf(), _ppa->actual_width(), _ppa->actual_height(),
                               bx1, by1, bx2, by2, 0, 255, 0);
        }
        esp_cache_msync(_ppa->out_buf(), _ppa->out_buf_size(), ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    xSemaphoreGive(_detect_mutex);
}

/* Draw detection boxes on RGB565 V4L2 buffer + cache msync */
void CameraStream::_draw_detection_boxes_on_rgb565(uint8_t *buf, uint32_t len)
{
    if (!_detect_in_buf || !_detect_available.load(std::memory_order_acquire) || !_detect_mutex ||
        xSemaphoreTake(_detect_mutex, 0) != pdTRUE) return;
    if (!_detect_results.empty()) {
        for (auto &r : _detect_results) {
            _draw_box_on_buffer(buf, _cam_width, _cam_height,
                                r.box[0], r.box[1], r.box[2], r.box[3], 0x07E0);
        }
        esp_cache_msync(buf, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    xSemaphoreGive(_detect_mutex);
}

/* Send MJPEG boundary + part header + JPEG data.
 * Returns true on success, false on client disconnect (caller should break). */
bool CameraStream::_send_mjpeg_part(httpd_req_t *req, uint8_t *jpeg_data, uint32_t jpeg_size,
                                     char *part_buf, size_t part_buf_size)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int hlen = snprintf(part_buf, part_buf_size, STREAM_PART, jpeg_size,
                        (int)ts.tv_sec, (int)(ts.tv_nsec / 1000));
    if (httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)) != ESP_OK ||
        httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK ||
        httpd_resp_send_chunk(req, (char *)jpeg_data, jpeg_size) != ESP_OK) {
        return false;
    }
    return true;
}

/* Save latest JPEG snapshot for /api/capture_image endpoint */
void CameraStream::_save_jpeg_snapshot(uint8_t *jpeg_data, uint32_t jpeg_size)
{
    if (!_last_jpeg_mutex || xSemaphoreTake(_last_jpeg_mutex, 0) != pdTRUE) return;
    if (_last_jpeg_capacity < jpeg_size) {
        uint8_t *new_buf = (uint8_t *)realloc(_last_jpeg_buf, jpeg_size);
        if (new_buf) {
            _last_jpeg_buf = new_buf;
            _last_jpeg_capacity = jpeg_size;
        }
    }
    if (_last_jpeg_buf && _last_jpeg_capacity >= jpeg_size) {
        memcpy(_last_jpeg_buf, jpeg_data, jpeg_size);
        _last_jpeg_size = jpeg_size;
    }
    xSemaphoreGive(_last_jpeg_mutex);
}

/* Update FPS counters and publish uORB stats every FPS_LOG_INTERVAL_S seconds */
void CameraStream::_update_fps_stats(uint32_t jpeg_size)
{
    _frame_count = _frame_count + 1;
    _fps_frame_count = _fps_frame_count + 1;
    _fps_total_bytes = _fps_total_bytes + jpeg_size;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - _fps_window_start.tv_sec) +
                     (now.tv_nsec - _fps_window_start.tv_nsec) / 1e9;
    if (elapsed >= FPS_LOG_INTERVAL_S) {
        float fps = (float)_fps_frame_count / (float)elapsed;
        /* Lazy-init publisher with CAS — orb_advertise in local to avoid leak on CAS failure */
        if (_fps_pub.load(std::memory_order_relaxed) < 0) {
            orb_advert_t new_pub = orb_advertise(ORB_ID(fps_stats));
            orb_advert_t expected = ORB_ADVERT_INVALID;
            _fps_pub.compare_exchange_strong(expected, new_pub,
                    std::memory_order_acq_rel, std::memory_order_acquire);
        }
        if (_fps_pub.load(std::memory_order_relaxed) >= 0) {
            struct fps_stats_s fps_msg = {};
            fps_msg.timestamp      = esp_timer_get_time();
            fps_msg.frame_count    = _frame_count;
            fps_msg.fps_total_bytes = _fps_total_bytes;
            fps_msg.fps            = fps;
            orb_publish(ORB_ID(fps_stats), _fps_pub, &fps_msg);
        }
        ESP_LOGD(TAG, "FPS: %.1f, bytes/s: %.0f", fps, (float)_fps_total_bytes / elapsed);
        _fps_window_start = now;
        _fps_frame_count = 0;
        _fps_total_bytes = 0;
    }
}

void CameraStream::set_recording(bool enabled)
{
    _recording_enabled.store(enabled, std::memory_order_release);
    ESP_LOGI(TAG, "Camera frame recording %s", enabled ? "enabled" : "disabled");
}

void CameraStream::_publish_camera_frame(uint8_t *jpeg_data, uint32_t jpeg_size)
{
    if (!_recording_enabled.load(std::memory_order_acquire)) return;
    if (!jpeg_data || jpeg_size == 0) return;

    /* JPEG size must fit in camera_frame_s.jpeg_data[8192] */
    if (jpeg_size > sizeof(((camera_frame_s *)0)->jpeg_data)) {
        ESP_LOGW(TAG, "JPEG frame too large for ULog (%u > %u), skipping",
                 (unsigned)jpeg_size, (unsigned)sizeof(((camera_frame_s *)0)->jpeg_data));
        return;
    }

    /* Lazy-init publisher (atomic CAS prevents double-advertise) */
    if (_frame_pub.load(std::memory_order_relaxed) < 0) {
        orb_advert_t new_pub = orb_advertise(ORB_ID(camera_frame));
        orb_advert_t expected = ORB_ADVERT_INVALID;
        _frame_pub.compare_exchange_strong(expected, new_pub,
                std::memory_order_acq_rel, std::memory_order_acquire);
    }
    if (_frame_pub.load(std::memory_order_relaxed) < 0) return;

    /* camera_frame_s is ~8KB — too large for httpd task stack (4KB default).
     * Allocate from PSRAM heap to avoid stack overflow. */
    camera_frame_s *frame = (camera_frame_s *)heap_caps_malloc(sizeof(camera_frame_s), MALLOC_CAP_SPIRAM);
    if (!frame) {
        ESP_LOGW(TAG, "Failed to allocate camera_frame_s, skipping publish");
        return;
    }

    memset(frame, 0, sizeof(*frame));
    frame->timestamp   = esp_timer_get_time();
    frame->frame_index = _frame_count.load(std::memory_order_relaxed);
    frame->width       = _stream_enc_width;
    frame->height      = _stream_enc_height;
    frame->format      = 0;  /* 0 = JPEG */
    frame->jpeg_size   = (uint16_t)jpeg_size;
    memcpy(frame->jpeg_data, jpeg_data, jpeg_size);

    orb_publish(ORB_ID(camera_frame), _frame_pub, frame);
    heap_caps_free(frame);
}

/** MJPEG stream handler (port 81) — continuous multipart JPEG stream */
static esp_err_t stream_handler(httpd_req_t *req)
{
    CameraStream *cs = (CameraStream *)req->user_ctx;
    char part_buf[128];
    struct v4l2_buffer buf;

    /* Lazy-init JPEG encoder on first MJPEG client connection.
     * This defers the internal-SRAM-heavy DMA descriptor allocation
     * until after V4L2 pipeline is stable and transient memory freed. */
    if (!cs->_encoder_initialized.load(std::memory_order_acquire) && cs->_cam_pixel_format != V4L2_PIX_FMT_JPEG) {
        if (!cs->_init_encoder()) {
            ESP_LOGE(TAG, "Cannot start MJPEG stream: JPEG encoder init failed");
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    int dqbuf_fail_count = 0;
    while (cs->isRunning()) {
        /* Dequeue frame */
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(cs->_video_fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGW(TAG, "V4L2 DQBUF failed (errno=%d)", errno);
            dqbuf_fail_count++;
            if (dqbuf_fail_count >= 100) {
                ESP_LOGE(TAG, "V4L2 DQBUF failed %d times consecutively, stopping stream", dqbuf_fail_count);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        dqbuf_fail_count = 0;  /* Reset on success */
        if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
            ESP_LOGD(TAG, "V4L2 DQBUF buf[%d] not done, requeue", buf.index);
            ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Bounds check: defensive guard against out-of-range buffer index */
        if (buf.index >= cs->_v4l2_buf_count) {
            ESP_LOGW(TAG, "V4L2 DQBUF returned invalid index %u (max %u), requeue", buf.index, cs->_v4l2_buf_count);
            ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Invalidate CPU cache on V4L2 mmap buffer */
        esp_cache_msync(cs->_v4l2_bufs[buf.index], cs->_v4l2_buf_len[buf.index],
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        /* Every N frames: PPA process + encode JPEG + run inference.
         * With PPA: V4L2 buffer is passed directly to PPA (DMA read, ~1ms),
         * then returned. No copy buffer needed. */
        bool detection_run_this_frame = false;
        if (cs->_detector && cs->_model_ready
            && cs->_frame_count % cs->DETECT_INTERVAL_FRAMES == 0) {

            /* Run PPA directly on V4L2 mmap buffer (if available) */
            bool ppa_ok = false;
#if CONFIG_SOC_PPA_SUPPORTED
            if (cs->_ppa && cs->_ppa->is_initialized()) {
                ppa_ok = cs->_ppa->process(cs->_v4l2_bufs[buf.index]);
            }
#endif

            /* Copy V4L2 buffer for CPU fallback (PPA output is independent of V4L2) */
            if (!ppa_ok && cs->_detect_in_buf) {
                uint32_t copy_sz = buf.bytesused;
                if (copy_sz > cs->_detect_in_size) copy_sz = cs->_detect_in_size;
                memcpy(cs->_detect_in_buf, cs->_v4l2_bufs[buf.index], copy_sz);
            }

            /* Determine source data BEFORE returning V4L2 buffer.
             * For JPEG sensors: use raw JPEG directly.
             * For PPA path: use PPA BGR24 output.
             * For CPU fallback: use V4L2 RGB565 buffer. */
            uint8_t *jpeg_src = nullptr;
            uint32_t jpeg_src_size = 0;

            if (cs->_cam_pixel_format == V4L2_PIX_FMT_JPEG) {
                /* JPEG sensor — no re-encoding needed, use raw V4L2 data.
                 * Must read pointer BEFORE QBUF (buffer returned to driver). */
                jpeg_src = cs->_v4l2_bufs[buf.index];
                jpeg_src_size = buf.bytesused;
            }

            /* Return V4L2 buffer immediately after PPA (or copy).
             * For JPEG sensor path: defer QBUF — the JPEG data IS the V4L2
             * buffer, so we must keep it until after HTTP send/snapshot/publish.
             * For non-JPEG: PPA/copy detached data from V4L2, QBUF early is safe. */
            if (cs->_cam_pixel_format != V4L2_PIX_FMT_JPEG) {
                ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
            }

            /* Encode JPEG from PPA output (300×300 BGR24) or V4L2 buffer (800×800 RGB565) */
            uint32_t jpeg_size;
            uint8_t *jpeg_data;

            if (jpeg_src) {
                /* JPEG sensor path: use pre-saved data */
                jpeg_data = jpeg_src;
                jpeg_size = jpeg_src_size;
            } else if (ppa_ok) {
                /* PPA path: encode from PPA BGR24 output (300×300) */
                if (xSemaphoreTake(cs->_encoder_sem, pdMS_TO_TICKS(500)) != pdPASS) {
                    continue;
                }
                /* Draw detection boxes on PPA output buffer (300×300 BGR24) */
                cs->_draw_detection_boxes_on_ppa();
                esp_err_t ret = example_encoder_process(cs->_encoder_handle,
                                                         cs->_ppa->out_buf(),
                                                         cs->_ppa->actual_width() * cs->_ppa->actual_height() * 3,
                                                         cs->_jpeg_out_buf, cs->_jpeg_out_size, &jpeg_size);
                if (ret != ESP_OK) {
                    xSemaphoreGive(cs->_encoder_sem);
                    continue;
                }
                jpeg_data = cs->_jpeg_out_buf;
            } else {
                /* CPU fallback: PPA not active — encoder is configured for cam format.
                 * Encode from _detect_in_buf (RGB565 copy of V4L2 buffer). */
                if (!cs->_detect_in_buf) continue;
                if (xSemaphoreTake(cs->_encoder_sem, pdMS_TO_TICKS(500)) != pdPASS) {
                    continue;
                }
                uint32_t copy_sz = cs->_detect_in_size;
                esp_err_t ret = example_encoder_process(cs->_encoder_handle,
                                                         cs->_detect_in_buf, copy_sz,
                                                         cs->_jpeg_out_buf, cs->_jpeg_out_size, &jpeg_size);
                if (ret != ESP_OK) {
                    xSemaphoreGive(cs->_encoder_sem);
                    ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
                    continue;
                }
                jpeg_data = cs->_jpeg_out_buf;
                /* Return V4L2 buffer after encoding */
                ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
            }

            /* Send MJPEG part before inference so stream stays continuous */
            if (!cs->_send_mjpeg_part(req, jpeg_data, jpeg_size, part_buf, sizeof(part_buf))) {
                if (cs->_cam_pixel_format != V4L2_PIX_FMT_JPEG) {
                    xSemaphoreGive(cs->_encoder_sem);
                } else {
                    /* JPEG sensor: buffer was not yet returned to V4L2 */
                    ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
                }
                break;
            }
            if (cs->_cam_pixel_format != V4L2_PIX_FMT_JPEG) {
                xSemaphoreGive(cs->_encoder_sem);
            }

            /* Save JPEG snapshot for /api/capture_image */
            cs->_save_jpeg_snapshot(jpeg_data, jpeg_size);

            /* Publish camera frame to uORB for ULog recording */
            cs->_publish_camera_frame(jpeg_data, jpeg_size);

            /* JPEG sensor: all consumers done, now safe to return V4L2 buffer */
            if (cs->_cam_pixel_format == V4L2_PIX_FMT_JPEG) {
                ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
            }

            /* Run inference — PPA output already available, _run_inference reuses it */
            cs->_run_inference(nullptr, 0);
            cs->_update_fps_stats(jpeg_size);
            vTaskDelay(pdMS_TO_TICKS(50));
            detection_run_this_frame = true;
        }

        if (!detection_run_this_frame) {
            /* boundary is sent by _send_mjpeg_part() below */

            uint32_t jpeg_size;
            uint8_t *jpeg_data;

            if (cs->_cam_pixel_format == V4L2_PIX_FMT_JPEG) {
                /* JPEG sensor — use raw V4L2 data directly.
                 * Must defer QBUF: jpeg_data points into V4L2 mmap buffer,
                 * which must not be returned to driver until all consumers
                 * (_send_mjpeg_part, _save_jpeg_snapshot, _publish_camera_frame)
                 * finish using it. */
                jpeg_data = cs->_v4l2_bufs[buf.index];
                jpeg_size = buf.bytesused;
            }
#if CONFIG_SOC_PPA_SUPPORTED
            else if (cs->_ppa && cs->_ppa->is_initialized()) {
                /* PPA path: pass V4L2 buffer directly to PPA (DMA read, ~1ms),
                 * then return V4L2 buffer. No copy needed. */
                if (cs->_ppa->process(cs->_v4l2_bufs[buf.index])) {
                    /* Return V4L2 buffer immediately — PPA output is independent */
                    ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);

                    /* Draw detection boxes on PPA output buffer (300×300 BGR24) */
                    cs->_draw_detection_boxes_on_ppa();

                    if (xSemaphoreTake(cs->_encoder_sem, pdMS_TO_TICKS(500)) != pdPASS) {
                        continue;
                    }
                    esp_err_t ret = example_encoder_process(cs->_encoder_handle,
                                                             cs->_ppa->out_buf(),
                                                             cs->_ppa->actual_width() * cs->_ppa->actual_height() * 3,
                                                             cs->_jpeg_out_buf, cs->_jpeg_out_size, &jpeg_size);
                    if (ret != ESP_OK) {
                        xSemaphoreGive(cs->_encoder_sem);
                        continue;
                    }
                    jpeg_data = cs->_jpeg_out_buf;
                } else {
                    /* PPA failed — return V4L2 buffer, skip this frame */
                    ESP_LOGW(TAG, "PPA process failed on non-detection frame");
                    ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
                    continue;
                }
            }
#endif
            else {
                /* CPU fallback: draw boxes on V4L2 buffer, encode from 800×800 RGB565.
                 * After encoding, jpeg_data points to _jpeg_out_buf (independent of
                 * V4L2 buffer), so QBUF is safe after encoding completes. */
                cs->_draw_detection_boxes_on_rgb565(cs->_v4l2_bufs[buf.index], cs->_v4l2_buf_len[buf.index]);

                if (xSemaphoreTake(cs->_encoder_sem, pdMS_TO_TICKS(500)) != pdPASS) {
                    ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
                    continue;
                }
                esp_err_t ret = example_encoder_process(cs->_encoder_handle,
                                                         cs->_v4l2_bufs[buf.index], buf.bytesused,
                                                         cs->_jpeg_out_buf, cs->_jpeg_out_size, &jpeg_size);
                if (ret != ESP_OK) {
                    xSemaphoreGive(cs->_encoder_sem);
                    ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
                    continue;
                }
                jpeg_data = cs->_jpeg_out_buf;
                /* Return V4L2 buffer — encoding done, jpeg_data is independent */
                ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
            }

            /* Send part header + JPEG data */
            if (!cs->_send_mjpeg_part(req, jpeg_data, jpeg_size, part_buf, sizeof(part_buf))) {
                if (cs->_cam_pixel_format != V4L2_PIX_FMT_JPEG) {
                    xSemaphoreGive(cs->_encoder_sem);
                } else {
                    /* JPEG sensor: buffer was not yet returned to V4L2 */
                    ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
                }
                break;
            }

            /* Save latest JPEG snapshot for /api/capture_image endpoint */
            cs->_save_jpeg_snapshot(jpeg_data, jpeg_size);

            /* Publish camera frame to uORB for ULog recording */
            cs->_publish_camera_frame(jpeg_data, jpeg_size);

            if (cs->_cam_pixel_format != V4L2_PIX_FMT_JPEG) {
                xSemaphoreGive(cs->_encoder_sem);
            }

            /* JPEG sensor: all consumers done, now safe to return V4L2 buffer.
             * Non-JPEG paths already returned the buffer above (PPA: after process,
             * CPU fallback: after encoding). */
            if (cs->_cam_pixel_format == V4L2_PIX_FMT_JPEG) {
                ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
            }

            cs->_update_fps_stats(jpeg_size);

            /* Yield CPU to prevent SDIO/WiFi starvation */
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    return ESP_OK;
}


/** Camera info JSON handler */
static esp_err_t camera_info_handler(httpd_req_t *req)
{
    CameraStream *cs = (CameraStream *)req->user_ctx;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    cJSON_AddNumberToObject(root, "width", cs->_stream_enc_width);
    cJSON_AddNumberToObject(root, "height", cs->_stream_enc_height);
    cJSON_AddNumberToObject(root, "sensor_width", cs->_cam_width);
    cJSON_AddNumberToObject(root, "sensor_height", cs->_cam_height);
    cJSON_AddNumberToObject(root, "jpeg_quality", cs->_jpeg_quality.load());
    cJSON_AddNumberToObject(root, "frame_rate", 2);   /* ~2fps from VTS=24600 */
    cJSON_AddNumberToObject(root, "total_frames", cs->_frame_count);

    const char *fmt_str = "UNKNOWN";
    if (cs->_cam_pixel_format == V4L2_PIX_FMT_JPEG) fmt_str = "JPEG";
    else if (cs->_cam_pixel_format == V4L2_PIX_FMT_YUV422P) fmt_str = "YUV422P";
    cJSON_AddStringToObject(root, "pixel_format", fmt_str);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);
    free(json_str);
    return ret;
}


    /** GET /api/capture_image — return the latest JPEG frame as image/jpeg */
    static esp_err_t capture_image_handler(httpd_req_t *req)
    {
        CameraStream *cs = (CameraStream *)req->user_ctx;

        if (!cs->_last_jpeg_mutex) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Snapshot not available");
            return ESP_FAIL;
        }

        esp_err_t ret = ESP_FAIL;
        if (xSemaphoreTake(cs->_last_jpeg_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (cs->_last_jpeg_buf && cs->_last_jpeg_size > 0) {
                httpd_resp_set_type(req, "image/jpeg");
                ret = httpd_resp_send(req, (char *)cs->_last_jpeg_buf, cs->_last_jpeg_size);
            } else {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No frame captured yet");
            }
            xSemaphoreGive(cs->_last_jpeg_mutex);
        } else {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Snapshot busy");
        }
        return ret;
    }


    /** Detection info JSON handler — person count + max confidence */
static esp_err_t detection_info_handler(httpd_req_t *req)
{
    CameraStream *cs = (CameraStream *)req->user_ctx;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(root, "detection_enabled", cs->_detector != nullptr);
    cJSON_AddBoolToObject(root, "model_ready", cs->_model_ready);

    float max_conf = 0.0f;
    size_t person_count = 0;
    bool detect_avail = false;
    if (cs->_detect_mutex &&
        xSemaphoreTake(cs->_detect_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (auto &r : cs->_detect_results) {
            if (r.score > max_conf) max_conf = r.score;
        }
        person_count = cs->_detect_results.size();
        detect_avail = cs->_detect_available.load(std::memory_order_relaxed);
        xSemaphoreGive(cs->_detect_mutex);
    }
    cJSON_AddNumberToObject(root, "person_count", person_count);
    cJSON_AddNumberToObject(root, "max_confidence", detect_avail ? max_conf : 0.0);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);
    free(json_str);
    return ret;
}

/** GET /api/set_quality?value=30 */
static esp_err_t set_quality_handler(httpd_req_t *req)
{
    CameraStream *cs = (CameraStream *)req->user_ctx;
    char buf[32];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
        return ESP_FAIL;
    }
    char val[8];
    if (httpd_query_key_value(buf, "value", val, sizeof(val)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing value");
        return ESP_FAIL;
    }
    int q = atoi(val);
    if (q < 1) q = 1;
    if (q > 100) q = 100;
    cs->_jpeg_quality.store((uint8_t)q);

    if (cs->_encoder_handle) {
        example_encoder_set_jpeg_quality(cs->_encoder_handle, q);
    }
    ESP_LOGI(TAG, "JPEG quality set to %d", q);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":1}");
    return ESP_OK;
}

/** POST /api/set_camera_config — Flutter app quality control */
static esp_err_t set_camera_config_handler(httpd_req_t *req)
{
    CameraStream *cs = (CameraStream *)req->user_ctx;

    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *q = cJSON_GetObjectItem(json, "jpeg_quality");
    if (q && cJSON_IsNumber(q)) {
        int quality = q->valueint;
        if (quality < 1) quality = 1;
        if (quality > 100) quality = 100;
        cs->_jpeg_quality.store((uint8_t)quality);
        if (cs->_encoder_handle) {
            example_encoder_set_jpeg_quality(cs->_encoder_handle, quality);
        }
        ESP_LOGI(TAG, "JPEG quality set to %d (via POST)", quality);
    }
    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    CameraStream *cs = (CameraStream *)req->user_ctx;
    (void)cs;

    const char *html =
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32-P4 Camera</title>"
        "<style>"
        "body{margin:0;background:#000;display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh;font-family:sans-serif}"
        "#settings-banner{position:fixed;top:0;left:0;right:0;background:#1a1a2e;color:#00d4ff;padding:8px 16px;text-align:center;font-size:13px;z-index:10}"
        "#settings-banner a{color:#00d4ff;font-weight:bold}"
        "img#stream{max-width:100vw;max-height:80vh;margin-top:40px}"
        ".panel{position:fixed;bottom:0;left:0;right:0;background:rgba(0,0,0,.8);padding:8px 16px;display:flex;gap:20px;justify-content:center;align-items:center;flex-wrap:wrap}"
        ".stat{color:#ccc;font-size:12px} .stat b{color:#4CAF50}"
        "label{color:#fff;font-size:12px} label b{color:#4CAF50}"
        "input[type=range]{width:80px;accent-color:#4CAF50}"
        "a{color:#4CAF50;text-decoration:none;font-size:11px}"
        "</style></head><body>"
        "<div id='settings-banner'>"
        "⚙ <a id='settings-link'>Settings (WiFi / Volume / Factory Reset)</a>"
        "</div>"
        "<img id='stream'>"
        "<div class='panel'>"
        "<span class='stat'>Res: <b id='res'>--</b></span>"
        "<span class='stat'>FPS: <b id='fps'>--</b></span>"
        "<span class='stat'>Frames: <b id='fr'>0</b></span>"
        "<label>Quality: <b id='ql'>30</b></label>"
        "<input type='range' id='qs' min='1' max='100' value='30' oninput=\"setQ(this.value)\">"
        "<span class='stat'>People: <b id='ppl'>--</b></span>"
        "<span class='stat'>Conf: <b id='conf'>--</b></span>"
        "</div><script>"
        "document.getElementById('settings-link').href='http://'+window.location.hostname+':8080/';"
        "var stream_url='http://'+window.location.hostname+':81/stream';"
        "document.getElementById('stream').src=stream_url;"
        "function setQ(v){document.getElementById('ql').textContent=v;fetch('/api/set_quality?value='+v)}"
        "function upd(){fetch('/api/get_camera_info').then(r=>r.json()).then(d=>{"
        "document.getElementById('res').textContent=d.width+'x'+d.height;"
        "document.getElementById('fps').textContent=d.frame_rate;"
        "document.getElementById('fr').textContent=d.total_frames;"
        "document.getElementById('ql').textContent=d.jpeg_quality;"
        "document.getElementById('qs').value=d.jpeg_quality}).catch(function(){})"
        ";fetch('/api/get_detection_info').then(r=>r.json()).then(d=>{"
        "document.getElementById('ppl').textContent=d.person_count;"
        "document.getElementById('conf').textContent=d.detection_enabled?d.max_confidence.toFixed(3):'N/A'"
        "}).catch(function(){})}"
        "setInterval(upd,3000);upd()</script></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

/*============================================================================
 * HTTP Server
 *============================================================================*/
bool CameraStream::_start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* Limit concurrent connections to save LWIP sockets (internal SRAM).
     * Default is 7 — too many for 3 httpd instances. 3 is sufficient for
     * 1 browser tab (parallel API + resource requests). */
    config.max_open_sockets = 3;
    config.core_id = 0;  /* Pin to Core 0 — Core 1 runs LVGL rendering */
    /* TCP keep-alive: detect dead connections quickly so sockets don't
     * leak when clients disconnect abruptly (ECONNRESET/EAGAIN). */
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;
    /* All registered URIs are exact paths — use default exact-match
     * instead of wildcard matching (faster, more secure). */

    /* Port 80: API + info */
    ESP_LOGI(TAG, "Starting HTTP server on port 80");
    if (httpd_start(&_httpd_80, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server port 80 start failed");
        return false;
    }

    httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = this };
    httpd_uri_t uri_info = { .uri = "/api/get_camera_info", .method = HTTP_GET, .handler = camera_info_handler, .user_ctx = this };
    httpd_uri_t uri_quality = { .uri = "/api/set_quality", .method = HTTP_GET, .handler = set_quality_handler, .user_ctx = this };
    httpd_uri_t uri_config  = { .uri = "/api/set_camera_config", .method = HTTP_POST, .handler = set_camera_config_handler, .user_ctx = this };
    httpd_uri_t uri_detect  = { .uri = "/api/get_detection_info", .method = HTTP_GET, .handler = detection_info_handler, .user_ctx = this };
    httpd_uri_t uri_capture = { .uri = "/api/capture_image", .method = HTTP_GET, .handler = capture_image_handler, .user_ctx = this };
    httpd_register_uri_handler(_httpd_80, &uri_index);
    httpd_register_uri_handler(_httpd_80, &uri_info);
    httpd_register_uri_handler(_httpd_80, &uri_quality);
    httpd_register_uri_handler(_httpd_80, &uri_config);
    httpd_register_uri_handler(_httpd_80, &uri_detect);
    httpd_register_uri_handler(_httpd_80, &uri_capture);

    /* Port 81: MJPEG stream */
    config.server_port += 1;   // 80 → 81 (matches reference: config.server_port += 1)
    config.ctrl_port += 1;     // Must differ from port 80's ctrl port
    config.stack_size = 1024 * 6;
    config.max_open_sockets = 2;  /* MJPEG stream: typically 1-2 clients */
    ESP_LOGI(TAG, "Starting MJPEG stream server on port 81");
    if (httpd_start(&_httpd_81, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server port 81 start failed");
        httpd_stop(_httpd_80);
        _httpd_80 = nullptr;
        return false;
    }

    httpd_uri_t uri_stream = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = this };
    httpd_register_uri_handler(_httpd_81, &uri_stream);

    return true;
}

void CameraStream::_stop_http_server(void)
{
    if (_httpd_81) {
        httpd_stop(_httpd_81);
        _httpd_81 = nullptr;
    }
    if (_httpd_80) {
        httpd_stop(_httpd_80);
        _httpd_80 = nullptr;
    }
}

/*============================================================================
 * mDNS
 *============================================================================*/
void CameraStream::_init_mdns(void)
{
    if (_mdns_running) return;

    /* Use shared mDNS guard — web_config_server may have already initialized it */
    if (!shared_mdns_ensure()) {
        ESP_LOGW(TAG, "mDNS init failed");
        return;
    }

    mdns_instance_name_set("web-cam");

    mdns_txt_item_t txt[] = {
        {(char *)"board", (char *)CONFIG_IDF_TARGET},
        {(char *)"path",  (char *)"/"},
    };
    if (mdns_service_add("ESP32-WebServer", "_http", "_tcp", 80, txt,
                         sizeof(txt) / sizeof(txt[0])) != ESP_OK) {
        ESP_LOGW(TAG, "mDNS service add failed");
    }

    _mdns_running = true;
    ESP_LOGI(TAG, "mDNS: esp-web.local + %s.local", shared_mdns_hostname());
}

void CameraStream::_deinit_mdns(void)
{
    if (!_mdns_running) return;
    /* Only remove CameraStream's _http service — do NOT call mdns_free(),
     * as web_config_server may still be using mDNS. */
    mdns_service_remove("_http", "_tcp");
    shared_mdns_release();
    _mdns_running = false;
}
