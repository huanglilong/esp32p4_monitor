#pragma once

#include "esp_brookesia.hpp"
#include "audio_player.h"

#include <string.h>

#define MAX_TRACKS 50

class PhoneAppMusic : public ESP_Brookesia_PhoneApp {
public:
    PhoneAppMusic(bool use_status_bar = false, bool use_navigation_bar = false);
    ~PhoneAppMusic();

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;

private:
    static void _player_event_cb_static(audio_player_cb_ctx_t *ctx);
    static void _playback_task(void *arg);

    void _scan_files(void);
    void _play(int index);
    void _pause_resume(void);
    void _stop(void);
    void _next(void);
    void _prev(void);

    /* Audio state */
    volatile int        _current_track;   // -1 = stopped, >=0 = playing
    volatile bool       _is_playing;
    int                 _track_count;

    /* File iterator */
    void               *_file_iter;        // Unused now, kept for compatibility
    char               *_file_names[MAX_TRACKS]; // Track file names

    /* LVGL objects */
    lv_obj_t           *_btn_back;
    lv_obj_t           *_label_title;
    lv_obj_t           *_label_status;
    lv_obj_t           *_btn_prev;
    lv_obj_t           *_btn_play;
    lv_obj_t           *_btn_next;
    lv_obj_t           *_list_tracks;
    lv_obj_t           *_label_no_files;
    lv_obj_t           *_slider_vol;
    lv_obj_t           *_label_vol;
    int                 _volume;           // Current volume (0-100)
};
