#include "phone_app_camera.hpp"
#include "private/esp_brookesia_utils.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "driver/isp.h"
#include "driver/isp_core.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_ctlr.h"
#include "hal/color_types.h"
extern "C" {
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_i2c.h"
}
#include "bsp/esp-bsp.h"
#include "example_config.h"
#include "coco_detect.hpp"
#include "dl_image_define.hpp"
#include "dl_image_ppa.hpp"

#include <cstring>

static const char *TAG = "PhoneAppCamera";

/* External SD card state and init function from main.cpp */
extern sdmmc_card_t *s_card;
extern void monitor_init_sdcard(void);

/* Use built-in brookesia launcher icon for camera */
extern const lv_image_dsc_t esp_brookesia_image_large_app_launcher_default_112_112;

PhoneAppCamera::PhoneAppCamera(bool use_status_bar, bool use_navigation_bar) :
    ESP_Brookesia_PhoneApp("Camera", &esp_brookesia_image_large_app_launcher_default_112_112,
                           true /* use_default_screen */,
                           use_status_bar, use_navigation_bar),
    _cam_buffer(nullptr), _cam_buf_size(0), _isp_proc(nullptr), _cam_ctlr(nullptr),
    _cam_sensor(nullptr), _sccb_handle(nullptr),
    _cam_canvas(nullptr), _refresh_timer(nullptr), _btn_back(nullptr),
    _cam_running(false),
    _detector(nullptr),
    _detect_available(false), _detect_task_handle(nullptr), _detect_mutex(nullptr),
    _ppa_handle(nullptr), _ppa_buf(nullptr), _ppa_buf_size(0)
{
    memset(&_cam_trans, 0, sizeof(_cam_trans));
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
    _deinit_detection();
    _deinit_camera();

    /* Re-mount SD card (was unmounted due to GPIO pin conflict with CSI) */
    ESP_LOGI(TAG, "Re-mounting SD card...");
    monitor_init_sdcard();

    ESP_LOGI(TAG, "Camera app closed");
    return true;
}

/*============================================================================
 * Camera Hardware Init / Deinit
 *============================================================================*/

bool PhoneAppCamera::_init_camera(void)
{
    /* Use sensor resolution for buffer (CSI writes RAW8 800x800, ISP outputs RGB888 800x800) */
    _cam_buf_size = EXAMPLE_CAM_SENSOR_HRES * EXAMPLE_CAM_SENSOR_VRES * 3;  // RGB888 = 3 bytes/px

    /* Allocate frame buffer in PSRAM (128-byte aligned for cache) */
    _cam_buffer = heap_caps_aligned_alloc(128, _cam_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_cam_buffer) {
        ESP_LOGE(TAG, "Failed to allocate camera buffer (%zu bytes)", _cam_buf_size);
        return false;
    }
    memset(_cam_buffer, 0x00, _cam_buf_size);  // Black frame
    esp_cache_msync(_cam_buffer, _cam_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /* Unmount SD card if mounted (pin conflict: GPIO39/43/44) */
    if (s_card) {
        ESP_LOGW(TAG, "Unmounting SD card (pin conflict with camera)...");
        esp_vfs_fat_sdcard_unmount(SDMMC_MOUNT_POINT, s_card);
        s_card = nullptr;
    }

    /* Initialize MIPI CSI */
    esp_err_t ret;
    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id = 0,
        .clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .h_res = EXAMPLE_CAM_SENSOR_HRES,
        .v_res = EXAMPLE_CAM_SENSOR_VRES,
        .data_lane_num = 2,
        .lane_bit_rate_mbps = EXAMPLE_MIPI_CSI_LANE_BITRATE_MBPS,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RAW8,
        .data_type = {0},
        .queue_items = 1,
    };

    ret = esp_cam_new_csi_ctlr(&csi_config, (esp_cam_ctlr_handle_t *)&_cam_ctlr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CSI init failed: 0x%x", ret);
        return false;
    }

    _cam_trans.buffer = _cam_buffer;
    _cam_trans.buflen = _cam_buf_size;
    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = [](esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *t, void *ud) -> bool {
            esp_cam_ctlr_trans_t *nt = (esp_cam_ctlr_trans_t *)ud;
            t->buffer = nt->buffer;
            t->buflen = nt->buflen;
            return false;
        },
        .on_trans_finished = [](esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *t, void *ud) -> bool {
            return false;
        },
    };
    ESP_ERROR_CHECK(esp_cam_ctlr_register_event_callbacks((esp_cam_ctlr_handle_t)_cam_ctlr, &cbs, &_cam_trans));
    ESP_ERROR_CHECK(esp_cam_ctlr_enable((esp_cam_ctlr_handle_t)_cam_ctlr));

    /* Initialize ISP (RAW8 → RGB565) */
    esp_isp_processor_cfg_t isp_config = {
        .clk_src = ISP_CLK_SRC_DEFAULT,
        .clk_hz = 80 * 1000 * 1000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB888,
        .yuv_range = ISP_COLOR_RANGE_FULL,
        .yuv_std = ISP_YUV_CONV_STD_BT601,
        .has_line_start_packet = false,
        .has_line_end_packet = false,
        .h_res = EXAMPLE_CAM_SENSOR_HRES,
        .v_res = EXAMPLE_CAM_SENSOR_VRES,
        .bayer_order = COLOR_RAW_ELEMENT_ORDER_GBRG,
        .intr_priority = 0,
        .flags = {
            .byte_swap_en = 1,
        },
    };
    ESP_ERROR_CHECK(esp_isp_new_processor(&isp_config, (isp_proc_handle_t *)&_isp_proc));
    ESP_ERROR_CHECK(esp_isp_enable((isp_proc_handle_t)_isp_proc));

    /* NOTE: OV5647 sensor I2C init would go here.
     * For now, camera ISP will show black frames until sensor is configured. */

    if (esp_cam_ctlr_start((esp_cam_ctlr_handle_t)_cam_ctlr) != ESP_OK) {
        ESP_LOGE(TAG, "Camera start failed");
        return false;
    }

    /* Initialize OV5647 sensor via BSP I2C */
    _init_sensor();

    _cam_running = true;
    ESP_LOGI(TAG, "MIPI CSI camera + ISP pipeline started");
    return true;
}

/*============================================================================
 * Camera Sensor Init (OV5647 via BSP I2C)
 *============================================================================*/

void PhoneAppCamera::_init_sensor(void)
{
    i2c_master_bus_handle_t i2c_handle = bsp_i2c_get_handle();
    if (!i2c_handle) {
        ESP_LOGW(TAG, "BSP I2C not available, skipping sensor init");
        return;
    }

    esp_cam_sensor_device_t *cam = nullptr;
    esp_sccb_io_handle_t sccb_handle = nullptr;

    for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start;
         p < &__esp_cam_sensor_detect_fn_array_end; ++p) {

        sccb_i2c_config_t i2c_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = p->sccb_addr,
            .scl_speed_hz = 100000,
            .addr_bits_width = 16,
            .val_bits_width = 8,
        };

        esp_err_t ret = sccb_new_i2c_io(i2c_handle, &i2c_config, &sccb_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "sccb_new_i2c_io failed for addr 0x%02x: %s",
                     p->sccb_addr, esp_err_to_name(ret));
            continue;
        }

        esp_cam_sensor_config_t cam_config = {
            .sccb_handle = sccb_handle,
            .reset_pin = (gpio_num_t)-1,
            .pwdn_pin = (gpio_num_t)-1,
            .xclk_pin = (gpio_num_t)-1,
            .sensor_port = (esp_cam_sensor_port_t)p->port,
        };

        cam = p->detect(&cam_config);
        if (cam) {
            ESP_LOGI(TAG, "Detected camera sensor at I2C addr 0x%02x", p->sccb_addr);
            _sccb_handle = sccb_handle;
            _cam_sensor = cam;
            break;
        }

        esp_sccb_del_i2c_io(sccb_handle);
        sccb_handle = nullptr;
    }

    if (!cam) {
        ESP_LOGW(TAG, "No camera sensor detected");
        if (sccb_handle) esp_sccb_del_i2c_io(sccb_handle);
        return;
    }

    /* Query and set format */
    esp_cam_sensor_format_array_t fmt_array = {0};
    esp_cam_sensor_query_format(cam, &fmt_array);

    const esp_cam_sensor_format_t *target_fmt = nullptr;
    for (int i = 0; i < fmt_array.count; i++) {
        ESP_LOGI(TAG, "Sensor format[%d]: %s", i, fmt_array.format_array[i].name);
        if (strcmp(fmt_array.format_array[i].name, EXAMPLE_CAM_FORMAT) == 0) {
            target_fmt = &fmt_array.format_array[i];
        }
    }

    if (!target_fmt) {
        ESP_LOGW(TAG, "Format '%s' not supported by sensor", EXAMPLE_CAM_FORMAT);
        return;
    }

    esp_err_t ret = esp_cam_sensor_set_format(cam, target_fmt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Sensor set format failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Sensor format set: %s", target_fmt->name);

    int enable = 1;
    ret = esp_cam_sensor_ioctl(cam, ESP_CAM_SENSOR_IOC_S_STREAM, &enable);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Sensor start stream failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "OV5647 sensor streaming started");
}

bool PhoneAppCamera::_deinit_camera(void)
{
    _cam_running = false;

    /* Stop sensor streaming */
    if (_cam_sensor) {
        int enable = 0;
        esp_cam_sensor_ioctl((esp_cam_sensor_device_t *)_cam_sensor,
                             ESP_CAM_SENSOR_IOC_S_STREAM, &enable);
        esp_cam_sensor_del_dev((esp_cam_sensor_device_t *)_cam_sensor);
        _cam_sensor = nullptr;
    }

    /* Stop and delete CSI controller (must be in INIT state for del) */
    if (_cam_ctlr) {
        esp_cam_ctlr_stop((esp_cam_ctlr_handle_t)_cam_ctlr);
        esp_cam_ctlr_disable((esp_cam_ctlr_handle_t)_cam_ctlr);
        esp_cam_ctlr_del((esp_cam_ctlr_handle_t)_cam_ctlr);
        _cam_ctlr = nullptr;
    }

    /* Disable and delete ISP processor */
    if (_isp_proc) {
        esp_isp_disable((isp_proc_handle_t)_isp_proc);
        esp_isp_del_processor((isp_proc_handle_t)_isp_proc);
        _isp_proc = nullptr;
    }

    /* Free SCCB I/O handle (BSP I2C bus is NOT deleted - BSP owns it) */
    if (_sccb_handle) {
        esp_sccb_del_i2c_io((esp_sccb_io_handle_t)_sccb_handle);
        _sccb_handle = nullptr;
    }

    /* Free frame buffer */
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
    /* Delete detection task */
    if (_detect_task_handle) {
        vTaskDelete(_detect_task_handle);
        _detect_task_handle = nullptr;
    }

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
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Sync cache: ISP DMA wrote to _cam_buffer, invalidate CPU cache to see fresh data */
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
 * Frame Update Timer (30fps) — lightweight detection box overlay
 *============================================================================*/

void PhoneAppCamera::_frame_update_timer_cb(lv_timer_t *timer)
{
    PhoneAppCamera *app = (PhoneAppCamera *)timer->user_data;
    if (!app || !app->_cam_running || !app->_cam_buffer) {
        return;
    }

    /* Draw detection boxes directly on canvas buffer pixels (no LVGL objects) */
    if (app->_detect_available && app->_detect_mutex &&
        xSemaphoreTake(app->_detect_mutex, 0) == pdTRUE) {

        if (!app->_detect_results.empty()) {
            lv_color_t green = lv_palette_main(LV_PALETTE_GREEN);
            for (auto &r : app->_detect_results) {
                app->_draw_box_on_canvas(r.box[0], r.box[1], r.box[2], r.box[3], green);
            }
            /* Flush CPU-written box pixels to PSRAM so display DMA sees them */
            esp_cache_msync(app->_cam_buffer, app->_cam_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }

        xSemaphoreGive(app->_detect_mutex);
    }

    /* Trigger LVGL to redraw canvas + overlays */
    lv_obj_invalidate(app->_cam_canvas);
}
