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
    _shared_jpeg_mutex(nullptr),
    _shared_jpeg_buf(nullptr), _shared_jpeg_size(0), _shared_jpeg_capacity(0),
    _frame_generation(0),
    _frame_ready_sem(nullptr),
    _frame_count(0), _fps_frame_count(0),
    _fps_window_start{0, 0},
    _fps_total_bytes(0),
    _fps_pub(ORB_ADVERT_INVALID),
    _stream_enc_width(0), _stream_enc_height(0), _stream_enc_format(0),
    _ppa(nullptr),
    _capture_task(nullptr),
    _capture_stack(nullptr), _capture_tcb(nullptr),
    _httpd_80(nullptr), _httpd_81(nullptr),
    _running(false),
    _start_stop_mutex(nullptr),
    _recording_enabled(false),
    _frame_pub(ORB_ADVERT_INVALID),
    _mdns_running(false)
{
    _shared_jpeg_mutex = xSemaphoreCreateMutex();
    _frame_ready_sem = xSemaphoreCreateCounting(2, 0);  /* Max 2 MJPEG clients */
    _start_stop_mutex = xSemaphoreCreateMutex();  /* Serializes start()/stop() */

    /* Pre-allocate TCB — reused across start/stop cycles.
     * ~340B internal SRAM, avoids TCB use-after-free race with idle task. */
    _capture_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

CameraStream::~CameraStream()
{
    stop();
    if (_shared_jpeg_mutex) {
        vSemaphoreDelete(_shared_jpeg_mutex);
        _shared_jpeg_mutex = nullptr;
    }
    heap_caps_free(_shared_jpeg_buf);
    _shared_jpeg_buf = nullptr;
    if (_frame_ready_sem) {
        vSemaphoreDelete(_frame_ready_sem);
        _frame_ready_sem = nullptr;
    }
    if (_start_stop_mutex) {
        vSemaphoreDelete(_start_stop_mutex);
        _start_stop_mutex = nullptr;
    }
    /* Defensive: stop() should have freed stacks, but ensure no leak */
    if (_capture_stack) { heap_caps_free(_capture_stack); _capture_stack = nullptr; }
    /* TCBs are pre-allocated at construction and never freed — ~340B each,
     * negligible cost for permanent singletons in embedded firmware.
     * Avoids TCB use-after-free race with idle task entirely. */
}

/*============================================================================
 * Public API
 *============================================================================*/
bool CameraStream::start(void)
{
    /* Serialize with stop() — prevents race when a cam_stop task is still
     * cleaning up (releasing CameraDriver, stopping httpd, freeing V4L2/PPA
     * resources) while a new cam_start task begins re-initialization.
     * Without this, start() can succeed its _running CAS (stop already set
     * it false), claim the camera (re-entrant, same owner), then fail on
     * EADDRINUSE (old httpd not stopped yet) — leading to double-free
     * and the SPI DMA crash observed in the field.
     * Timeout: 20s — stop() may wait up to 5s for capture task.
     * If stop() hasn't finished in 20s, something is seriously wrong
     * and we should fail rather than hang forever. */
    if (!_start_stop_mutex) return false;
    if (xSemaphoreTake(_start_stop_mutex, pdMS_TO_TICKS(20000)) != pdTRUE) {
        ESP_LOGE(TAG, "start() timed out waiting for stop() to finish — giving up");
        return false;
    }

    /* Atomic compare-and-set: if already running, return immediately.
     * This prevents concurrent start() calls from double-initializing. */
    bool expected = false;
    if (!_running.compare_exchange_strong(expected, true)) {
        xSemaphoreGive(_start_stop_mutex);
        return true;  // already running
    }

    /* Claim camera hardware via CameraDriver BEFORE any resource allocation.
     * This ensures cross-module mutual exclusion from the earliest point. */
    if (!CameraDriver::instance().claim("stream")) {
        ESP_LOGW(TAG, "Camera hardware in use, cannot start stream");
        _running = false;
        xSemaphoreGive(_start_stop_mutex);
        return false;
    }

    if (!_init_video()) {
        ESP_LOGE(TAG, "Video init failed");
        _running = false;
        CameraDriver::instance().release("stream");
        xSemaphoreGive(_start_stop_mutex);
        return false;
    }

    /* Init PPA hardware preprocessor for 300×300 stream resolution */
    if (!_init_ppa()) {
        ESP_LOGW(TAG, "PPA init failed, encoding at camera resolution");
    }

    /* Init JPEG encoder — must be after _init_ppa() because encoder
     * dimensions depend on PPA state. For JPEG sensors, this is a no-op. */
    if (!_init_encoder()) {
        ESP_LOGE(TAG, "JPEG encoder init failed");
        _deinit_ppa();
        _deinit_video();
        _running = false;
        CameraDriver::instance().release("stream");
        xSemaphoreGive(_start_stop_mutex);
        return false;
    }

    /* Start independent capture task — runs DQBUF/encode/publish loop
     * regardless of HTTP client connections.
     * Use static task creation with PSRAM stack to save ~32KB internal SRAM. */
    _capture_stack = (StackType_t *)heap_caps_malloc(8192 * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_capture_stack || !_capture_tcb) {
        ESP_LOGE(TAG, "Capture task PSRAM stack alloc failed or TCB is null");
        if (_capture_stack) { heap_caps_free(_capture_stack); _capture_stack = nullptr; }
        _deinit_encoder();
        _deinit_video();
        _deinit_ppa();
        _running = false;
        CameraDriver::instance().release("stream");
        xSemaphoreGive(_start_stop_mutex);
        return false;
    }
    /* Pin to Core 0: in v0.0.3 the capture/encode loop ran inside the httpd
     * stream_handler, which is bound to Core 0 (S176). The Music GMF/ASP task
     * runs on Core 1 at priority 3; pinning this task to Core 1 would let it
     * (priority 5) preempt the music decoder and starve the I2S TX DMA,
     * causing audible music stutter whenever Camera Stream is enabled. */
    TaskHandle_t task_handle = xTaskCreateStaticPinnedToCore(
        _capture_task_fn, "cam_capture", 8192, this, 5,
        _capture_stack, _capture_tcb, 0);  /* Core 0, priority 5 */
    if (!task_handle) {
        ESP_LOGE(TAG, "Capture task create failed");
        heap_caps_free(_capture_stack); _capture_stack = nullptr;
        _deinit_encoder();
        _deinit_video();
        _deinit_ppa();
        _running = false;
        CameraDriver::instance().release("stream");
        xSemaphoreGive(_start_stop_mutex);
        return false;
    }
    _capture_task.store(task_handle, std::memory_order_release);

    if (!_start_http_server()) {
        ESP_LOGE(TAG, "HTTP server init failed");
        _running = false;  /* Signal capture task to exit */
        /* STREAMOFF to unblock DQBUF if capture task is waiting */
        if (_video_fd >= 0) {
            int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(_video_fd, VIDIOC_STREAMOFF, &type);
        }
        /* Wait for capture task to finish (with timeout) */
        int wait_ms = 0;
        while (_capture_task.load(std::memory_order_acquire) != nullptr && wait_ms < 3000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_ms += 10;
        }
        if (_capture_task.load(std::memory_order_acquire) != nullptr) {
            ESP_LOGW(TAG, "Capture task did not exit after 3s in start() failure path, force-killing");
            TaskHandle_t t = _capture_task.exchange(nullptr, std::memory_order_acq_rel);
            if (t) vTaskDelete(t);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        _deinit_encoder();
        _deinit_video();
        _deinit_ppa();
        CameraDriver::instance().release("stream");
        xSemaphoreGive(_start_stop_mutex);
        return false;
    }

    _init_mdns();

    /* Reset FPS counters for new streaming session */
    _frame_count = 0;
    _fps_frame_count = 0;
    _fps_total_bytes = 0;
    _frame_generation = 0;
    clock_gettime(CLOCK_MONOTONIC, &_fps_window_start);

    /* Auto-enable camera frame recording — recording is now tied to stream state */
    _recording_enabled.store(true, std::memory_order_release);

    ESP_LOGI(TAG, "Stream started → http://%s.local/stream (port 81)", shared_mdns_hostname());
    xSemaphoreGive(_start_stop_mutex);
    return true;
}

void CameraStream::stop(void)
{
    /* Serialize with start() — prevents race when a cam_start task begins
     * re-initialization while this stop() is still cleaning up resources.
     * See start() header comment for the full race scenario. */
    if (!_start_stop_mutex) return;
    xSemaphoreTake(_start_stop_mutex, portMAX_DELAY);

    /* Use CAS to prevent double-cleanup — compare_exchange_strong ensures
     * only one caller transitions from running→stopped, matching start()'s pattern. */
    bool expected = true;
    if (!_running.compare_exchange_strong(expected, false)) {
        xSemaphoreGive(_start_stop_mutex);
        return;
    }

    /* _running is now false — capture task loop will exit on next iteration.
     * stream_handler loop also exits via isRunning().
     * 
     * CRITICAL: We must call VIDIOC_STREAMOFF before waiting for capture task,
     * because the task may be blocked in VIDIOC_DQBUF (blocking call, no timeout).
     * STREAMOFF causes DQBUF to return an error, unblocking the task.
     * We only do the V4L2 STREAMOFF here (not full _deinit_video), so the
     * capture task can still safely exit without crashing on partially-freed resources. */

    /* Step 1: Stop V4L2 streaming to unblock DQBUF in capture task */
    if (_video_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(_video_fd, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "VIDIOC_STREAMOFF failed in stop()");
        }
    }

    /* Step 2: Wait for capture task to finish (now unblocked from DQBUF).
     * Timeout after 5s — if VIDIOC_STREAMOFF failed, the capture task may
     * be stuck in VIDIOC_DQBUF and never exit. Force-kill after timeout
     * to prevent stop() from hanging forever (which would also block
     * start() via _start_stop_mutex). */
    TaskHandle_t task = _capture_task.load(std::memory_order_acquire);
    if (task) {
        /* Give semaphore to unblock any waiting stream_handler clients */
        xSemaphoreGive(_frame_ready_sem);
        xSemaphoreGive(_frame_ready_sem);
        int wait_ms = 0;
        while (_capture_task.load(std::memory_order_acquire) != nullptr && wait_ms < 5000) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_ms += 10;
        }
        if (_capture_task.load(std::memory_order_acquire) != nullptr) {
            ESP_LOGW(TAG, "Capture task did not exit after 5s, force-killing (may corrupt heap)");
            vTaskDelete(task);
            _capture_task.store(nullptr, std::memory_order_release);
            /* Yield to let idle task reclaim TCB before we free the stack */
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    /* Step 3: Full cleanup — now safe to free all resources */
    CameraDriver::instance().release("stream");
    _stop_http_server();
    _deinit_mdns();
    _deinit_ppa();
    _deinit_video();

    /* Free capture task PSRAM stack.
     * TCB is pre-allocated and reused — not freed here.
     * Stack is safe to free immediately: FreeRTOS doesn't reference it
     * after the task exits (idle task only reclaims the TCB). */
    if (_capture_stack) {
        heap_caps_free(_capture_stack);
        _capture_stack = nullptr;
    }

    /* Reset shared JPEG buffer */
    if (_shared_jpeg_mutex && xSemaphoreTake(_shared_jpeg_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _shared_jpeg_size = 0;
        xSemaphoreGive(_shared_jpeg_mutex);
    }

    /* Reset FPS publisher handle — will be re-created on next start() */
    _fps_pub = ORB_ADVERT_INVALID;

    /* Reset camera frame publisher handle */
    _frame_pub = ORB_ADVERT_INVALID;
    _recording_enabled.store(false, std::memory_order_release);

    ESP_LOGI(TAG, "Stream stopped");
    xSemaphoreGive(_start_stop_mutex);
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
        if (_v4l2_buf_count > 2) {
            ESP_LOGE(TAG, "VIDIOC_REQBUFS returned %" PRIu32 " buffers, max 2 supported", _v4l2_buf_count);
            _v4l2_buf_count = 2;
        }
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

        /* Step 7: JPEG encoder init is done in start() before capture task.
         * See _init_encoder() for retry logic with backoff. */

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
 * JPEG Encoder Init/Deinit
 *
 * The JPEG hardware encoder's DMA descriptors (rxlink, txlink) require
 * MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL — they MUST be in internal SRAM.
 * On LCD-4B, internal SRAM is scarce because LVGL draw buffers also live
 * there.  Encoder is now initialized upfront in start() (no longer lazy)
 * because the independent capture task needs it immediately.
 * Retry with backoff if internal memory is temporarily fragmented.
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
        _stream_enc_width = _cam_width;
        _stream_enc_height = _cam_height;
        _stream_enc_format = V4L2_PIX_FMT_JPEG;
        _encoder_initialized.store(true, std::memory_order_release);
        _encoder_init_in_progress.store(false, std::memory_order_release);
        return true;
    }

    /* Determine encoder input: if PPA is active, encode from PPA output
     * (300×300 BGR24) instead of full 800×800 RGB565. This reduces:
     *   - JPEG encode time: ~200ms → ~30ms
     *   - JPEG size: ~30-50KB → ~5-8KB
     *   - WiFi bandwidth: ~80% reduction */
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
 * PPA Preprocessor — hardware-accelerated resize 800×800 → 300×300
 * Reduces JPEG encode time (~200ms→30ms), size (~30-50KB→5-8KB),
 * and WiFi bandwidth (~80% reduction).
 *============================================================================*/

bool CameraStream::_init_ppa(void)
{
#if CONFIG_SOC_PPA_SUPPORTED
    _ppa = new (std::nothrow) PPAPreprocessor();
    if (_ppa) {
        if (!_ppa->init((uint16_t)_cam_width, (uint16_t)_cam_height, 300, 300)) {
            ESP_LOGW(TAG, "PPA init failed for %" PRIu32 "x%" PRIu32 " → 300x300", _cam_width, _cam_height);
            delete _ppa;
            _ppa = nullptr;
            return false;
        }
        ESP_LOGI(TAG, "PPA enabled: %" PRIu32 "×%" PRIu32 " RGB565 → %d×%d BGR888",
                 _cam_width, _cam_height, _ppa->actual_width(), _ppa->actual_height());
        return true;
    }
    ESP_LOGW(TAG, "PPA alloc failed");
#endif
    return false;
}

void CameraStream::_deinit_ppa(void)
{
#if CONFIG_SOC_PPA_SUPPORTED
    if (_ppa) {
        delete _ppa;
        _ppa = nullptr;
        ESP_LOGI(TAG, "PPA deinitialized");
    }
#endif
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
    if (hlen < 0 || (size_t)hlen >= part_buf_size) return false;
    if (httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)) != ESP_OK ||
        httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK ||
        httpd_resp_send_chunk(req, (char *)jpeg_data, jpeg_size) != ESP_OK) {
        return false;
    }
    return true;
}

/* Store latest JPEG in shared buffer and signal waiting HTTP clients.
 * Called by capture task after encoding each frame. */
void CameraStream::_store_shared_jpeg(uint8_t *jpeg_data, uint32_t jpeg_size)
{
    /* Use short timeout (10ms) instead of non-blocking: if a stream_handler
     * is copying from the shared buffer, we wait briefly rather than silently
     * dropping the frame and leaving clients without a semaphore signal. */
    if (!_shared_jpeg_mutex || xSemaphoreTake(_shared_jpeg_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        /* Mutex unavailable — still signal clients so they don't timeout.
         * They'll see the same _frame_generation and skip (no duplicate send). */
        xSemaphoreGive(_frame_ready_sem);
        xSemaphoreGive(_frame_ready_sem);
        return;
    }
    if (_shared_jpeg_capacity < jpeg_size) {
        uint8_t *new_buf = (uint8_t *)heap_caps_realloc(_shared_jpeg_buf, jpeg_size, MALLOC_CAP_SPIRAM);
        if (new_buf) {
            _shared_jpeg_buf = new_buf;
            _shared_jpeg_capacity = jpeg_size;
        }
    }
    if (_shared_jpeg_buf && _shared_jpeg_capacity >= jpeg_size) {
        memcpy(_shared_jpeg_buf, jpeg_data, jpeg_size);
        _shared_jpeg_size = jpeg_size;
    }
    _frame_generation.fetch_add(1, std::memory_order_release);
    xSemaphoreGive(_shared_jpeg_mutex);

    /* Signal stream handlers (counting sem: give once per waiting client slot) */
    xSemaphoreGive(_frame_ready_sem);
    xSemaphoreGive(_frame_ready_sem);  /* Second slot for potential 2nd client */
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

    /* JPEG size must fit in camera_frame_s.jpeg_data[10240] */
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

    /* camera_frame_s is ~10KB — too large for httpd task stack (4KB default).
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

/*============================================================================
 * Independent Capture Task — DQBUF → encode → publish uORB → store shared JPEG
 *
 * This task runs continuously while _running is true, regardless of whether
 * any HTTP client is connected. This ensures uORB topics (fps_stats,
 * camera_frame) are always published for ULog recording.
 *============================================================================*/
void CameraStream::_capture_task_fn(void *arg)
{
    CameraStream *cs = (CameraStream *)arg;
    struct v4l2_buffer buf;
    int dqbuf_fail_count = 0;

    ESP_LOGI(TAG, "Capture task started");

    while (cs->_running.load(std::memory_order_relaxed)) {
        /* Dequeue frame */
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(cs->_video_fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGW(TAG, "V4L2 DQBUF failed (errno=%d)", errno);
            dqbuf_fail_count++;
            if (dqbuf_fail_count >= 100) {
                ESP_LOGE(TAG, "V4L2 DQBUF failed %d times consecutively, stopping", dqbuf_fail_count);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        dqbuf_fail_count = 0;
        if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
            ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Bounds check */
        if (buf.index >= cs->_v4l2_buf_count) {
            ESP_LOGW(TAG, "V4L2 DQBUF invalid index %u (max %u)", buf.index, cs->_v4l2_buf_count);
            ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Clamp bytesused */
        if (buf.bytesused > cs->_v4l2_buf_len[buf.index]) {
            buf.bytesused = cs->_v4l2_buf_len[buf.index];
        }

        /* Invalidate CPU cache on V4L2 mmap buffer */
        esp_cache_msync(cs->_v4l2_bufs[buf.index], cs->_v4l2_buf_len[buf.index],
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        bool buf_returned = false;

        /*--- PPA resize: 800×800 RGB565 → 300×300 BGR24 ---*/
        bool ppa_ok = false;
#if CONFIG_SOC_PPA_SUPPORTED
        if (cs->_ppa && cs->_ppa->is_initialized()) {
            ppa_ok = cs->_ppa->process(cs->_v4l2_bufs[buf.index]);
        }
#endif

        /* Encode JPEG */
        uint32_t jpeg_size = 0;
        uint8_t *jpeg_data = nullptr;
        bool encoder_held = false;

        if (cs->_cam_pixel_format == V4L2_PIX_FMT_JPEG) {
            /* JPEG sensor: use V4L2 buffer directly (no software encoder) */
            jpeg_data = cs->_v4l2_bufs[buf.index];
            jpeg_size = buf.bytesused;
        } else if (ppa_ok) {
            /* PPA output → encode at 300×300 BGR24 */
            ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
            buf_returned = true;
            if (xSemaphoreTake(cs->_encoder_sem, pdMS_TO_TICKS(500)) != pdPASS) {
                goto capture_done;
            }
            encoder_held = true;
            esp_err_t ret = example_encoder_process(cs->_encoder_handle,
                                                     cs->_ppa->out_buf(),
                                                     cs->_ppa->actual_width() * cs->_ppa->actual_height() * 3,
                                                     cs->_jpeg_out_buf, cs->_jpeg_out_size, &jpeg_size);
            if (ret != ESP_OK) {
                xSemaphoreGive(cs->_encoder_sem);
                encoder_held = false;
                goto capture_done;
            }
            jpeg_data = cs->_jpeg_out_buf;
        } else {
            /* No PPA: encode at camera resolution */
            if (xSemaphoreTake(cs->_encoder_sem, pdMS_TO_TICKS(500)) != pdPASS) {
                goto capture_done;
            }
            encoder_held = true;
            esp_err_t ret = example_encoder_process(cs->_encoder_handle,
                                                     cs->_v4l2_bufs[buf.index], buf.bytesused,
                                                     cs->_jpeg_out_buf, cs->_jpeg_out_size, &jpeg_size);
            if (ret != ESP_OK) {
                xSemaphoreGive(cs->_encoder_sem);
                encoder_held = false;
                goto capture_done;
            }
            jpeg_data = cs->_jpeg_out_buf;
            ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
            buf_returned = true;
        }

        /* Publish & store */
        if (jpeg_data && jpeg_size > 0) {
            cs->_publish_camera_frame(jpeg_data, jpeg_size);
            cs->_store_shared_jpeg(jpeg_data, jpeg_size);
        }

        /* Release encoder semaphore */
        if (encoder_held && cs->_encoder_sem) {
            xSemaphoreGive(cs->_encoder_sem);
            encoder_held = false;
        }
        /* JPEG sensor: return V4L2 buffer after all consumers done */
        if (!buf_returned && cs->_cam_pixel_format == V4L2_PIX_FMT_JPEG) {
            ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
            buf_returned = true;
        }

        cs->_update_fps_stats(jpeg_size);
capture_done:
        /* Ensure V4L2 buffer is returned on any error/early-exit path */
        if (!buf_returned) {
            ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* Task is exiting — clear handle so stop() knows we're done */
    cs->_capture_task.store(nullptr, std::memory_order_release);
    ESP_LOGI(TAG, "Capture task exiting");
    vTaskDelete(nullptr);
}

/** MJPEG stream handler (port 81) — pure consumer, reads shared JPEG buffer */
static esp_err_t stream_handler(httpd_req_t *req)
{
    CameraStream *cs = (CameraStream *)req->user_ctx;
    char part_buf[128];
    uint32_t last_gen = 0;

    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (cs->isRunning()) {
        /* Wait for new frame from capture task (max 2s timeout) */
        if (xSemaphoreTake(cs->_frame_ready_sem, pdMS_TO_TICKS(2000)) != pdPASS) {
            continue;
        }

        /* Skip if we already sent this generation (another client consumed our signal) */
        uint32_t cur_gen = cs->_frame_generation.load(std::memory_order_acquire);
        if (cur_gen == last_gen) continue;
        last_gen = cur_gen;

        /* Read latest JPEG from shared buffer */
        uint8_t *jpeg_data = nullptr;
        uint32_t jpeg_size = 0;

        if (cs->_shared_jpeg_mutex &&
            xSemaphoreTake(cs->_shared_jpeg_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (cs->_shared_jpeg_buf && cs->_shared_jpeg_size > 0) {
                /* Allocate a local copy — shared buffer may be overwritten by
                 * capture task before HTTP send completes. At ~5-8KB per JPEG,
                 * this is affordable from PSRAM. */
                jpeg_data = (uint8_t *)heap_caps_malloc(cs->_shared_jpeg_size, MALLOC_CAP_SPIRAM);
                if (jpeg_data) {
                    memcpy(jpeg_data, cs->_shared_jpeg_buf, cs->_shared_jpeg_size);
                    jpeg_size = cs->_shared_jpeg_size;
                }
            }
            xSemaphoreGive(cs->_shared_jpeg_mutex);
        }

        if (!jpeg_data || jpeg_size == 0) continue;

        /* Send MJPEG part */
        bool ok = cs->_send_mjpeg_part(req, jpeg_data, jpeg_size, part_buf, sizeof(part_buf));
        heap_caps_free(jpeg_data);

        if (!ok) break;
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
    cJSON_AddNumberToObject(root, "width", cs->_stream_enc_width ? cs->_stream_enc_width : cs->_cam_width);
    cJSON_AddNumberToObject(root, "height", cs->_stream_enc_height ? cs->_stream_enc_height : cs->_cam_height);
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
    /** Detection info JSON handler — detection disabled */
static esp_err_t detection_info_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"detection_enabled\":false}");
    return ESP_OK;
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
        "document.getElementById('qs').value=d.jpeg_quality}).catch(function(){})}"
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
    httpd_register_uri_handler(_httpd_80, &uri_index);
    httpd_register_uri_handler(_httpd_80, &uri_info);
    httpd_register_uri_handler(_httpd_80, &uri_quality);
    httpd_register_uri_handler(_httpd_80, &uri_config);
    httpd_register_uri_handler(_httpd_80, &uri_detect);

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
