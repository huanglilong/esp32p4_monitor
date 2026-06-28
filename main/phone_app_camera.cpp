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
    _cam_running(false)
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
    /* Use sensor resolution for buffer (CSI writes RAW8 800x800, ISP outputs RGB565 800x800) */
    _cam_buf_size = EXAMPLE_CAM_SENSOR_HRES * EXAMPLE_CAM_SENSOR_VRES * EXAMPLE_RGB565_BYTES_PER_PIXEL;

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
        .output_data_color_type = ISP_COLOR_RGB565,
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
 * Frame Update Timer (30fps)
 *============================================================================*/

void PhoneAppCamera::_frame_update_timer_cb(lv_timer_t *timer)
{
    PhoneAppCamera *app = (PhoneAppCamera *)timer->user_data;
    if (!app || !app->_cam_running || !app->_cam_buffer) {
        return;
    }

    /* Sync cache: ISP DMA writes to PSRAM, ensure cache coherence for LVGL display DMA reads.
     * byte_swap_en=1 in ISP config handles LE byte order for LVGL/LCD. */
    esp_cache_msync(app->_cam_buffer, app->_cam_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /* Trigger LVGL to redraw canvas from camera buffer */
    lv_obj_invalidate(app->_cam_canvas);
}
