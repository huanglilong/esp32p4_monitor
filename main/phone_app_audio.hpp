#pragma once

#include "esp_brookesia.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <stdio.h>
#include "uorb.h"
#include "topics.h"

#include "esp_aac_enc.h"
#include "esp_audio_enc.h"
#include "esp_audio_enc_default.h"

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
    std::atomic<TaskHandle_t> _task_handle;
    std::atomic<bool>  _task_running;
    /* Static task buffers — stack in PSRAM to save internal SRAM.
     * TCB is pre-allocated at construction and reused across run/close
     * cycles (~340B internal SRAM, avoids TCB use-after-free race).
     * Stack (12KB) is allocated/freed per run/close from PSRAM. */
    StackType_t       *_audio_stack;      /* 12KB, PSRAM, allocated in run() */
    StaticTask_t      *_audio_tcb;        /* ~340B, internal SRAM, pre-allocated */

    /* Recording state */
    std::atomic<bool>  _is_recording;
    std::atomic<int>   _recording_ops_in_flight;  /* Non-zero while audio task is in recording block — prevents _stop_recording from freeing _pcm_buffer/_encoder prematurely */
    esp_audio_enc_handle_t _encoder;
    FILE              *_record_file;
    int16_t           *_pcm_buffer;      // Accumulation buffer for 1024*2 samples
    int                _pcm_buf_count;   // Samples accumulated (per channel)
    uint8_t           *_enc_in_buf;      // AAC encoder input buffer (enc_in_size bytes)
    uint8_t           *_enc_out_buf;     // AAC encoder output buffer (enc_out_size bytes)
    int                _enc_in_size;     // AAC encoder input frame size (bytes)
    int                _enc_out_size;    // AAC encoder output frame size (bytes)
    std::atomic<uint32_t> _record_bytes_written;
    std::atomic<uint32_t> _record_start_ms;

    /* uORB publisher: notifies other modules (e.g. Music) when recording is active */
    std::atomic<orb_advert_t> _rec_pub{ORB_ADVERT_INVALID};

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
