#pragma once

#include "esp_brookesia.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

extern "C" {
#include "layer3.h"
}

#define MAX_RECORDINGS 50

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

    void _start_recording(void);
    void _stop_recording(void);
    void _scan_recordings(void);

    /* Audio state */
    TaskHandle_t       _task_handle;
    volatile bool      _task_running;

    /* Recording state */
    volatile bool      _is_recording;
    shine_t            _encoder;
    FILE              *_record_file;
    int16_t           *_pcm_buffer;      // Accumulation buffer for 1152*2 samples
    int                _pcm_buf_count;   // Samples accumulated (per channel)
    volatile uint32_t  _record_bytes_written;
    volatile uint32_t  _record_start_ms;

    /* Recordings list */
    char              *_recording_names[MAX_RECORDINGS];
    int                _recording_count;

    /* LVGL objects */
    lv_obj_t          *_btn_back;        // Back button
    lv_obj_t          *_btn_record;      // Record/Stop button
    lv_obj_t          *_label_rec_status;// Recording status label
    lv_obj_t          *_label_footer;    // Footer label
    lv_obj_t          *_list_recordings; // List of recordings
    lv_obj_t          *_label_no_recs;   // "No recordings" label
    lv_timer_t        *_update_timer;    // LVGL update timer (50ms)
};
