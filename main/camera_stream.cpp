/*
 * Camera Stream over WiFi — based on simple_video_server reference.
 * V4L2 camera → JPEG encoding → HTTP MJPEG → mDNS
 */

#include "camera_stream.hpp"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "example_video_common.h"
#include "mdns.h"
#include "cJSON.h"
#include "lwip/apps/netbiosns.h"
#include "driver/i2c_master.h"
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

static const char *TAG = "CameraStream";

/* Forward declaration: BSP I2C bus handle getter (provided by Waveshare BSP) */
extern "C" i2c_master_bus_handle_t bsp_i2c_get_handle(void);

/*============================================================================
 * OV5647 VTS helper: reduce sensor frame rate from ~50fps to ~10fps
 *============================================================================*/
void ov5647_set_vts_10fps(void)
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

    const uint16_t NEW_VTS = 4920;  /* 5x original 984 → ~10fps */
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
            ESP_LOGI(TAG, "OV5647 VTS: 984→%u (target ~10 fps)", vts);
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
    _jpeg_quality(30),  // Lower quality = faster encode + less WiFi/SDIO traffic
    _encoder_sem(nullptr),
    _frame_count(0), _fps_frame_count(0),
    _fps_window_start{0, 0},
    _fps_total_bytes(0),
    _httpd_80(nullptr), _httpd_81(nullptr),
    _running(false),
    _mdns_running(false)
{}

CameraStream::~CameraStream()
{
    stop();
}

/*============================================================================
 * Public API
 *============================================================================*/
bool CameraStream::start(void)
{
    if (_running) return true;

    if (!_init_video()) {
        ESP_LOGE(TAG, "Video init failed");
        return false;
    }

    if (!_start_http_server()) {
        ESP_LOGE(TAG, "HTTP server init failed");
        _deinit_video();
        return false;
    }

    _init_mdns();

    /* Reset FPS counters for new streaming session */
    _frame_count = 0;
    _fps_frame_count = 0;
    _fps_total_bytes = 0;
    clock_gettime(CLOCK_MONOTONIC, &_fps_window_start);

    _running = true;
    ESP_LOGI(TAG, "Stream started → http://esp-web.local/stream (port 81)");
    return true;
}

void CameraStream::stop(void)
{
    if (!_running) return;

    _running = false;
    _stop_http_server();
    _deinit_mdns();
    _deinit_video();

    ESP_LOGI(TAG, "Stream stopped");
}

/*============================================================================
 * V4L2 Video Init / Deinit
 *============================================================================*/
bool CameraStream::_init_video(void)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_format fmt = {};      // Declared early to avoid goto-cross-init
    struct v4l2_streamparm sparm = {}; // Same

    /* Step 1: Init video pipeline via esp_video (safe to call multiple times) */
    if (example_video_init() != ESP_OK) {
        ESP_LOGE(TAG, "example_video_init failed");
        return false;
    }

    /* Step 1b: Reduce sensor frame rate from 50fps → ~10fps by increasing VTS.
     * This reduces ISP DMA bandwidth from ~32 MB/s to ~6.4 MB/s,
     * relieving PSRAM pressure during WiFi streaming. */
    ov5647_set_vts_10fps();

    /* Step 2: Open V4L2 device */
    _video_fd = open(EXAMPLE_CAM_DEV_PATH, O_RDWR);
    if (_video_fd < 0) {
        ESP_LOGE(TAG, "Failed to open %s", EXAMPLE_CAM_DEV_PATH);
        return false;
    }
    ESP_LOGI(TAG, "V4L2 device opened: %s", EXAMPLE_CAM_DEV_PATH);

    /* Step 3: Read frame rate (matches reference: VIDIOC_G_PARM).
     * Note: Driver caches the original preset value (50fps); actual sensor
     * frame rate is ~10fps due to VTS=4920 set in ov5647_set_vts_10fps(). */
    sparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(_video_fd, VIDIOC_G_PARM, &sparm) == 0) {
        struct v4l2_fract *tpf = &sparm.parm.capture.timeperframe;
        uint32_t fps = (tpf->denominator && tpf->numerator) ? tpf->denominator / tpf->numerator : 0;
        ESP_LOGI(TAG, "V4L2 frame rate: %" PRIu32 " fps (driver cached, actual ~10 fps from VTS=4920)", fps);
    } else {
        ESP_LOGW(TAG, "VIDIOC_G_PARM failed, frame rate unknown");
    }

    /* Step 4: REQBUFS (matches reference: REQBUFS before G_FMT) */
    struct v4l2_requestbuffers req = {};
    req.count = 2;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_LOGI(TAG, "V4L2 REQBUFS: count=%" PRIu32, req.count);
    if (ioctl(_video_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
        ::close(_video_fd); _video_fd = -1;
        return false;
    }
    _v4l2_buf_count = req.count;
    ESP_LOGI(TAG, "V4L2 REQBUFS ok: got %" PRIu32 " buffers", _v4l2_buf_count);

    /* Step 5: mmap + QBUF each buffer (matches reference) */
    for (uint32_t i = 0; i < _v4l2_buf_count; i++) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(_video_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF[%" PRIu32 "] failed", i);
            goto fail;
        }
        ESP_LOGI(TAG, "V4L2 buf[%" PRIu32 "]: offset=0x%x len=%" PRIu32, i, buf.m.offset, buf.length);
        _v4l2_bufs[i] = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                          MAP_SHARED, _video_fd, buf.m.offset);
        if (_v4l2_bufs[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap[%" PRIu32 "] failed", i);
            _v4l2_bufs[i] = nullptr;
            goto fail;
        }
        _v4l2_buf_len[i] = buf.length;
        if (ioctl(_video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF[%" PRIu32 "] failed", i);
            goto fail;
        }
    }

    /* Step 6: G_FMT to read pixel format (matches reference: after REQBUFS) */
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(_video_fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
        goto fail;
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

    /* Step 7: Init JPEG encoder for non-JPEG formats (matches reference) */
    if (_cam_pixel_format != V4L2_PIX_FMT_JPEG) {
        example_encoder_config_t enc_cfg = {
            .width = _cam_width,
            .height = _cam_height,
            .pixel_format = _cam_pixel_format,
            .quality = _jpeg_quality,
        };
        ESP_LOGI(TAG, "Encoder init: %" PRIu32 "x%" PRIu32 " quality=%d",
                 _cam_width, _cam_height, _jpeg_quality);
        if (example_encoder_init(&enc_cfg, &_encoder_handle) != ESP_OK) {
            ESP_LOGE(TAG, "Encoder init failed for format 0x%08" PRIx32, _cam_pixel_format);
            goto fail;
        }
        if (example_encoder_alloc_output_buffer(_encoder_handle, &_jpeg_out_buf, &_jpeg_out_size) != ESP_OK) {
            ESP_LOGE(TAG, "Encoder output buffer alloc failed");
            goto fail;
        }
        ESP_LOGI(TAG, "Encoder output buffer: %" PRIu32 " bytes", _jpeg_out_size);
        _encoder_sem = xSemaphoreCreateBinary();
        if (!_encoder_sem) {
            ESP_LOGE(TAG, "Encoder semaphore create failed");
            goto fail;
        }
        xSemaphoreGive(_encoder_sem);
    }

    /* Step 8: VIDIOC_STREAMON (matches reference) */
    ESP_LOGI(TAG, "V4L2 STREAMON...");
    if (ioctl(_video_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        goto fail;
    }

    ESP_LOGI(TAG, "V4L2 pipeline active: %" PRIu32 "x%" PRIu32 " streaming", _cam_width, _cam_height);
    return true;

fail:
    if (_encoder_sem) { vSemaphoreDelete(_encoder_sem); _encoder_sem = nullptr; }
    if (_jpeg_out_buf) { example_encoder_free_output_buffer(_encoder_handle, _jpeg_out_buf); _jpeg_out_buf = nullptr; }
    if (_encoder_handle) { example_encoder_deinit(_encoder_handle); _encoder_handle = nullptr; }
    for (uint32_t i = 0; i < _v4l2_buf_count; i++) {
        if (_v4l2_bufs[i]) { munmap(_v4l2_bufs[i], _v4l2_buf_len[i]); _v4l2_bufs[i] = nullptr; }
    }
    ::close(_video_fd); _video_fd = -1;
    return false;
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
}

/*============================================================================
 * HTTP Handlers
 *============================================================================*/

/** MJPEG stream handler (port 81) — continuous multipart JPEG stream */
static esp_err_t stream_handler(httpd_req_t *req)
{
    CameraStream *cs = (CameraStream *)req->user_ctx;
    char part_buf[128];
    struct v4l2_buffer buf;
    struct timespec ts;

    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (cs->isRunning()) {
        /* Dequeue frame */
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(cs->_video_fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGW(TAG, "V4L2 DQBUF failed (errno=%d)", errno);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
            ESP_LOGD(TAG, "V4L2 DQBUF buf[%d] not done, requeue", buf.index);
            ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Invalidate CPU cache on V4L2 mmap buffer: ISP DMA writes here,
         * CPU reads for encoding. ESP32-P4 PSRAM requires explicit sync. */
        esp_cache_msync(cs->_v4l2_bufs[buf.index], cs->_v4l2_buf_len[buf.index],
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        /* Send boundary */
        if (httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)) != ESP_OK) {
            break;
        }

        uint32_t jpeg_size;
        uint8_t *jpeg_data;

        if (cs->_cam_pixel_format == V4L2_PIX_FMT_JPEG) {
            jpeg_data = cs->_v4l2_bufs[buf.index];
            jpeg_size = buf.bytesused;
        } else {
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
            /* HW encoder output buffer from jpeg_alloc_encoder_mem() is
             * non-cacheable DMA memory — no cache sync needed. */
            jpeg_data = cs->_jpeg_out_buf;
        }

        /* Send part header + JPEG data */
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, jpeg_size,
                            (int)ts.tv_sec, (int)(ts.tv_nsec / 1000));
        if (httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK ||
            httpd_resp_send_chunk(req, (char *)jpeg_data, jpeg_size) != ESP_OK) {
            if (cs->_cam_pixel_format != V4L2_PIX_FMT_JPEG) {
                xSemaphoreGive(cs->_encoder_sem);
            }
            ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
            break;
        }
        if (cs->_cam_pixel_format != V4L2_PIX_FMT_JPEG) {
            xSemaphoreGive(cs->_encoder_sem);
        }

        /* Return buffer */
        ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);

        /* FPS tracking — count frames only (no debug log) */
        cs->_frame_count++;

        /* Yield CPU to prevent SDIO/WiFi starvation.
         * JPEG SW encoding of 800x800 RGB565 takes significant time
         * without yielding, causing esp_hosted SDIO timeouts.
         * A delay between frames gives SDIO task time to process WiFi TX. */
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return ESP_OK;
}

/** Single JPEG snapshot handler */
static esp_err_t capture_handler(httpd_req_t *req)
{
    CameraStream *cs = (CameraStream *)req->user_ctx;
    struct v4l2_buffer buf;
    uint32_t jpeg_size;
    uint8_t *jpeg_data;
    esp_err_t http_ret;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(cs->_video_fd, VIDIOC_DQBUF, &buf) != 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
        ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");

    if (cs->_cam_pixel_format == V4L2_PIX_FMT_JPEG) {
        jpeg_data = cs->_v4l2_bufs[buf.index];
        jpeg_size = buf.bytesused;
    } else {
        if (xSemaphoreTake(cs->_encoder_sem, pdMS_TO_TICKS(500)) != pdPASS) {
            ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        esp_err_t ret = example_encoder_process(cs->_encoder_handle,
                                                 cs->_v4l2_bufs[buf.index], buf.bytesused,
                                                 cs->_jpeg_out_buf, cs->_jpeg_out_size, &jpeg_size);
        if (ret != ESP_OK) {
            xSemaphoreGive(cs->_encoder_sem);
            ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        jpeg_data = cs->_jpeg_out_buf;
    }

    http_ret = httpd_resp_send(req, (char *)jpeg_data, jpeg_size);

    if (cs->_cam_pixel_format != V4L2_PIX_FMT_JPEG) {
        xSemaphoreGive(cs->_encoder_sem);
    }
    ioctl(cs->_video_fd, VIDIOC_QBUF, &buf);

    return http_ret;
}

/** Camera info JSON handler */
static esp_err_t camera_info_handler(httpd_req_t *req)
{
    CameraStream *cs = (CameraStream *)req->user_ctx;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "width", cs->_cam_width);
    cJSON_AddNumberToObject(root, "height", cs->_cam_height);
    cJSON_AddNumberToObject(root, "jpeg_quality", cs->_jpeg_quality);
    cJSON_AddNumberToObject(root, "frame_rate", 10);  /* ~10fps from VTS=4920 */
    cJSON_AddNumberToObject(root, "total_frames", cs->_frame_count);

    const char *fmt_str = "UNKNOWN";
    if (cs->_cam_pixel_format == V4L2_PIX_FMT_JPEG) fmt_str = "JPEG";
    else if (cs->_cam_pixel_format == V4L2_PIX_FMT_YUV422P) fmt_str = "YUV422P";
    cJSON_AddStringToObject(root, "pixel_format", fmt_str);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
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
    cs->_jpeg_quality = (uint8_t)q;

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
        cs->_jpeg_quality = (uint8_t)quality;
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
        "img#stream{max-width:100vw;max-height:80vh}"
        ".panel{position:fixed;bottom:0;left:0;right:0;background:rgba(0,0,0,.8);padding:8px 16px;display:flex;gap:20px;justify-content:center;align-items:center;flex-wrap:wrap}"
        ".stat{color:#ccc;font-size:12px} .stat b{color:#4CAF50}"
        "label{color:#fff;font-size:12px} label b{color:#4CAF50}"
        "input[type=range]{width:80px;accent-color:#4CAF50}"
        "a{color:#4CAF50;text-decoration:none;font-size:11px}"
        "</style></head><body>"
        "<img id='stream'>"
        "<div class='panel'>"
        "<span class='stat'>Res: <b id='res'>--</b></span>"
        "<span class='stat'>FPS: <b id='fps'>--</b></span>"
        "<span class='stat'>Frames: <b id='fr'>0</b></span>"
        "<label>Quality: <b id='ql'>30</b></label>"
        "<input type='range' id='qs' min='1' max='100' value='30' oninput=\"setQ(this.value)\">"
        "<a href='http://' + window.location.hostname + ':81/stream'>Direct</a>"
        "<a href='/api/capture_image'>Shot</a>"
        "</div><script>"
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
    config.uri_match_fn = httpd_uri_match_wildcard;

    /* Port 80: API + info */
    ESP_LOGI(TAG, "Starting HTTP server on port 80");
    if (httpd_start(&_httpd_80, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server port 80 start failed");
        return false;
    }

    httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = this };
    httpd_uri_t uri_info = { .uri = "/api/get_camera_info", .method = HTTP_GET, .handler = camera_info_handler, .user_ctx = this };
    httpd_uri_t uri_capture = { .uri = "/api/capture_image", .method = HTTP_GET, .handler = capture_handler, .user_ctx = this };
    httpd_uri_t uri_quality = { .uri = "/api/set_quality", .method = HTTP_GET, .handler = set_quality_handler, .user_ctx = this };
    httpd_uri_t uri_config  = { .uri = "/api/set_camera_config", .method = HTTP_POST, .handler = set_camera_config_handler, .user_ctx = this };
    httpd_register_uri_handler(_httpd_80, &uri_index);
    httpd_register_uri_handler(_httpd_80, &uri_info);
    httpd_register_uri_handler(_httpd_80, &uri_capture);
    httpd_register_uri_handler(_httpd_80, &uri_quality);
    httpd_register_uri_handler(_httpd_80, &uri_config);

    /* Port 81: MJPEG stream */
    config.server_port += 1;   // 80 → 81 (matches reference: config.server_port += 1)
    config.ctrl_port += 1;     // Must differ from port 80's ctrl port
    config.stack_size = 1024 * 6;
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

    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed");
        return;
    }

    mdns_hostname_set("esp-web");
    mdns_instance_name_set("web-cam");  // matches reference

    /* NetBIOS (matches reference: netbiosns_init + netbiosns_set_name) */
    netbiosns_init();
    netbiosns_set_name("esp-web");

    mdns_txt_item_t txt[] = {
        {(char *)"board", (char *)CONFIG_IDF_TARGET},
        {(char *)"path",  (char *)"/"},
    };
    if (mdns_service_add("ESP32-WebServer", "_http", "_tcp", 80, txt,
                         sizeof(txt) / sizeof(txt[0])) != ESP_OK) {
        ESP_LOGW(TAG, "mDNS service add failed");
    }

    _mdns_running = true;
    ESP_LOGI(TAG, "mDNS: esp-web.local (NetBIOS: esp-web)");
}

void CameraStream::_deinit_mdns(void)
{
    if (!_mdns_running) return;
    mdns_service_remove("_http", "_tcp");
    mdns_free();
    _mdns_running = false;
}
