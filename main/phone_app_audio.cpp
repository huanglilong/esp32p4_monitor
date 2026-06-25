#include "phone_app_audio.hpp"
#include "private/esp_brookesia_utils.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "bsp/display.h"
#include <string.h>

static const char *TAG = "PhoneAppAudio";

/* External audio handles from main.cpp */
extern i2s_chan_handle_t s_rx_handle;              // I2S RX for mic

/* External launcher icon from brookesia */
extern const lv_image_dsc_t esp_brookesia_image_large_app_launcher_default_112_112;

/* Audio buffer: 160 samples * 2 channels * 2 bytes = 640 bytes ~10ms @16kHz */
#define AUDIO_BUF_SAMPLES   160
#define AUDIO_BUF_BYTES     (AUDIO_BUF_SAMPLES * 2 * sizeof(int16_t))

PhoneAppAudio::PhoneAppAudio(bool use_status_bar, bool use_navigation_bar) :
    ESP_Brookesia_PhoneApp("Audio", &esp_brookesia_image_large_app_launcher_default_112_112,
                           true /* use_default_screen */,
                           use_status_bar, use_navigation_bar),
    _task_handle(nullptr), _task_running(false),
    _level_left(0.0f), _level_right(0.0f),
    _bar_left(nullptr), _bar_right(nullptr),
    _label_left(nullptr), _label_right(nullptr),
    _btn_back(nullptr), _label_footer(nullptr),
    _update_timer(nullptr)
{ }

PhoneAppAudio::~PhoneAppAudio()
{
    if (_task_running) {
        _task_running = false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool PhoneAppAudio::run(void)
{
    ESP_LOGI(TAG, "Audio app starting...");
    lv_obj_t *screen = lv_scr_act();
    int32_t screen_w = BSP_LCD_H_RES;
    int32_t screen_h = BSP_LCD_V_RES;

    /* Back button (top-left) */
    _btn_back = lv_btn_create(screen);
    lv_obj_set_size(_btn_back, 50, 50);
    lv_obj_align(_btn_back, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_bg_color(_btn_back, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(_btn_back, LV_OPA_70, 0);
    lv_obj_set_style_radius(_btn_back, 25, 0);
    lv_obj_add_event_cb(_btn_back, [](lv_event_t *e) {
        ((PhoneAppAudio *)e->user_data)->back();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t *back_label = lv_label_create(_btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_CLOSE);
    lv_obj_center(back_label);

    /* Left channel section */
    _label_left = lv_label_create(screen);
    lv_label_set_text(_label_left, "Mic L");
    lv_obj_set_style_text_color(_label_left, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_text_font(_label_left, &lv_font_montserrat_16, 0);
    lv_obj_align(_label_left, LV_ALIGN_LEFT_MID, 20, -40);

    _bar_left = lv_bar_create(screen);
    lv_obj_set_size(_bar_left, 40, screen_h * 2 / 3);
    lv_obj_align(_bar_left, LV_ALIGN_LEFT_MID, 20, 40);
    lv_bar_set_range(_bar_left, 0, 100);
    lv_bar_set_value(_bar_left, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_bar_left, lv_color_hex(0x003322), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_bar_left, lv_color_hex(0x00FF44), LV_PART_INDICATOR);

    /* Right channel section */
    _label_right = lv_label_create(screen);
    lv_label_set_text(_label_right, "Mic R");
    lv_obj_set_style_text_color(_label_right, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_text_font(_label_right, &lv_font_montserrat_16, 0);
    lv_obj_align(_label_right, LV_ALIGN_RIGHT_MID, -20, -40);

    _bar_right = lv_bar_create(screen);
    lv_obj_set_size(_bar_right, 40, screen_h * 2 / 3);
    lv_obj_align(_bar_right, LV_ALIGN_RIGHT_MID, -20, 40);
    lv_bar_set_range(_bar_right, 0, 100);
    lv_bar_set_value(_bar_right, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_bar_right, lv_color_hex(0x003322), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_bar_right, lv_color_hex(0x00FF44), LV_PART_INDICATOR);

    /* Footer */
    _label_footer = lv_label_create(screen);
    lv_label_set_text(_label_footer, "Dual Mic Monitor");
    lv_obj_set_style_text_color(_label_footer, lv_color_hex(0x888888), 0);
    lv_obj_align(_label_footer, LV_ALIGN_BOTTOM_MID, 0, -30);

    /* Start audio echo task */
    _task_running = true;
    BaseType_t ret = xTaskCreate(_audio_task, "audio_echo", 4096, this, 5, &_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio task");
        _task_running = false;
        return false;
    }

    /* LVGL update timer (20Hz) */
    _update_timer = lv_timer_create(_level_update_timer_cb, 50, this);

    ESP_LOGI(TAG, "Audio app running");
    return true;
}

bool PhoneAppAudio::back(void)
{
    ESP_LOGI(TAG, "Audio app back");
    ESP_BROOKESIA_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
    return true;
}

bool PhoneAppAudio::close(void)
{
    ESP_LOGI(TAG, "Audio app closing...");

    _task_running = false;
    if (_update_timer) {
        lv_timer_delete(_update_timer);
        _update_timer = nullptr;
    }

    /* Wait for task to finish */
    vTaskDelay(pdMS_TO_TICKS(150));

    ESP_LOGI(TAG, "Audio app closed");
    return true;
}

/*============================================================================
 * Audio Echo Task
 *============================================================================*/

void PhoneAppAudio::_audio_task(void *arg)
{
    PhoneAppAudio *app = (PhoneAppAudio *)arg;
    int16_t *buf = (int16_t *)heap_caps_calloc(1, AUDIO_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        app->_task_running = false;
        vTaskDelete(nullptr);
        return;
    }

    while (app->_task_running) {
        /* Read from mic via direct I2S RX */
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(s_rx_handle, buf, AUDIO_BUF_BYTES, &bytes_read, pdMS_TO_TICKS(100));
        if (ret != ESP_OK || bytes_read == 0) {
            continue;
        }

        /* Compute peak levels from raw mic data */
        int32_t peak_l = 0, peak_r = 0;
        int32_t samples = (int32_t)(bytes_read / (2 * sizeof(int16_t)));
        for (int32_t i = 0; i < samples; i++) {
            int32_t abs_l = abs((int32_t)buf[i * 2]);
            int32_t abs_r = abs((int32_t)buf[i * 2 + 1]);
            if (abs_l > peak_l) peak_l = abs_l;
            if (abs_r > peak_r) peak_r = abs_r;
        }
        app->_level_left  = (float)peak_l / 32767.0f;
        app->_level_right = (float)peak_r / 32767.0f;
    }

    free(buf);
    vTaskDelete(nullptr);
}

/*============================================================================
 * LVGL Level Update Timer (20Hz)
 *============================================================================*/

void PhoneAppAudio::_level_update_timer_cb(lv_timer_t *timer)
{
    PhoneAppAudio *app = (PhoneAppAudio *)timer->user_data;
    if (!app || !app->_task_running) return;

    int32_t lv = (int32_t)(app->_level_left * 100.0f);
    int32_t rv = (int32_t)(app->_level_right * 100.0f);

    if (lv > 100) lv = 100;
    if (rv > 100) rv = 100;

    if (app->_bar_left)  lv_bar_set_value(app->_bar_left, lv, LV_ANIM_OFF);
    if (app->_bar_right) lv_bar_set_value(app->_bar_right, rv, LV_ANIM_OFF);

    /* Update labels with dB-like values */
    if (app->_label_left) {
        char txt[24];
        snprintf(txt, sizeof(txt), "Mic L %d%%", (int)lv);
        lv_label_set_text(app->_label_left, txt);
    }
    if (app->_label_right) {
        char txt[24];
        snprintf(txt, sizeof(txt), "Mic R %d%%", (int)rv);
        lv_label_set_text(app->_label_right, txt);
    }
}
