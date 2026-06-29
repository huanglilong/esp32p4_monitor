#pragma once

#include "esp_brookesia.hpp"
#include "esp_audio_simple_player.h"
#include "esp_gmf_err.h"

#define MAX_TRACKS 50

class PhoneAppMusic : public ESP_Brookesia_PhoneApp {
public:
    PhoneAppMusic(bool use_status_bar = false, bool use_navigation_bar = false);
    ~PhoneAppMusic();

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;

private:
    static int _asp_event_cb(esp_asp_event_pkt_t *pkt, void *ctx);
    static int _asp_output_cb(uint8_t *data, int data_size, void *ctx);
    static void _auto_next_timer_cb(lv_timer_t *timer);

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
    esp_asp_handle_t    _asp_handle;      // Audio simple player handle

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
    volatile int         _volume;           // Current volume (0-100), cross-core access
    bool                 _nvs_dirty;         // NVS debounce flag
    lv_timer_t          *_nvs_save_timer;    // NVS debounce timer (500ms)

    /* Deferred auto-next (avoids GMF re-entrancy from ASP event callback) */
    volatile bool       _auto_next;
    lv_timer_t         *_auto_next_timer;
};
