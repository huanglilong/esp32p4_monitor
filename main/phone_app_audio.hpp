#pragma once

#include "esp_brookesia.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class PhoneAppAudio : public ESP_Brookesia_PhoneApp {
public:
    PhoneAppAudio(bool use_status_bar = false, bool use_navigation_bar = false);
    ~PhoneAppAudio();

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;

private:
    static void _audio_task(void *arg);
    static void _level_update_timer_cb(lv_timer_t *timer);

    /* Audio state */
    TaskHandle_t       _task_handle;
    volatile bool      _task_running;
    volatile float     _level_left;      // Left channel level (0.0 - 1.0)
    volatile float     _level_right;     // Right channel level (0.0 - 1.0)

    /* LVGL objects */
    lv_obj_t          *_bar_left;        // Left channel bar
    lv_obj_t          *_bar_right;       // Right channel bar
    lv_obj_t          *_label_left;      // Left channel label
    lv_obj_t          *_label_right;     // Right channel label
    lv_obj_t          *_btn_back;        // Back button
    lv_obj_t          *_label_footer;    // Footer label
    lv_timer_t        *_update_timer;    // LVGL update timer (50ms)
};
