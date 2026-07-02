#include "phone_app_music.hpp"
#include "private/esp_brookesia_utils.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_lvgl_port.h"
#include "nvs.h"
#include "bsp/display.h"
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "MusicApp";

/* External peripheral init/deinit from main.cpp */
extern bool monitor_init_sdcard(void);
extern void monitor_deinit_sdcard(void);
extern void monitor_init_audio(void);
extern void monitor_deinit_audio(void);

extern esp_codec_dev_handle_t s_codec_handle;
extern SemaphoreHandle_t s_codec_mutex;
static inline void safe_set_volume(int vol) {
    if (s_codec_handle && s_codec_mutex &&
        xSemaphoreTake(s_codec_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        esp_codec_dev_set_out_vol(s_codec_handle, vol);
        xSemaphoreGive(s_codec_mutex);
    }
}

/* External handles from main.cpp */
extern esp_codec_dev_handle_t s_codec_handle;

/* External icon from brookesia */
extern const lv_image_dsc_t esp_brookesia_image_large_app_launcher_default_112_112;

#define MUSIC_DIR  "/sdcard"
#define MAX_TRACKS 50

PhoneAppMusic::PhoneAppMusic(bool use_status_bar, bool use_navigation_bar) :
    ESP_Brookesia_PhoneApp("Music", &esp_brookesia_image_large_app_launcher_default_112_112,
                           true, use_status_bar, use_navigation_bar),
    _current_track(-1), _is_playing(false), _track_count(0),
    _asp_handle(nullptr),
    _btn_back(nullptr), _label_title(nullptr), _label_status(nullptr),
    _btn_prev(nullptr), _btn_play(nullptr), _btn_next(nullptr),
    _list_tracks(nullptr), _label_no_files(nullptr),
    _slider_vol(nullptr), _label_vol(nullptr),
    _volume(60),
    _nvs_dirty(false), _nvs_save_timer(nullptr),
    _vol_sync_timer(nullptr),
    _auto_next(false), _auto_next_timer(nullptr)
{
    memset(_file_names, 0, sizeof(_file_names));
}

PhoneAppMusic::~PhoneAppMusic()
{
    _stop();
    /* Defensive cleanup: free file names if close() wasn't called */
    for (int i = 0; i < _track_count; i++) {
        if (_file_names[i]) { free(_file_names[i]); _file_names[i] = nullptr; }
    }
}

bool PhoneAppMusic::run(void)
{
    ESP_LOGI(TAG, "Music app starting...");

    /* Init audio I2S + DAC (speaker output) */
    monitor_init_audio();

    /* Init SD card for music files */
    if (!monitor_init_sdcard()) {
        ESP_LOGW(TAG, "SD card not available, music app may have no files");
    }

    lv_obj_t *screen = lv_scr_act();

    /* Back button */
    _btn_back = lv_btn_create(screen);
    lv_obj_set_size(_btn_back, 50, 50);
    lv_obj_align(_btn_back, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_set_style_bg_color(_btn_back, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(_btn_back, LV_OPA_70, 0);
    lv_obj_set_style_radius(_btn_back, 25, 0);
    lv_obj_add_event_cb(_btn_back, [](lv_event_t *e) {
        ((PhoneAppMusic *)e->user_data)->back();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t *back_label = lv_label_create(_btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_CLOSE);
    lv_obj_center(back_label);

    /* Title */
    _label_title = lv_label_create(screen);
    lv_label_set_text(_label_title, "Music Player");
    lv_obj_set_style_text_font(_label_title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(_label_title, lv_color_white(), 0);
    lv_obj_align(_label_title, LV_ALIGN_TOP_MID, 0, 15);

    /* Status label */
    _label_status = lv_label_create(screen);
    lv_label_set_text(_label_status, "Scanning...");
    lv_obj_set_style_text_color(_label_status, lv_color_hex(0x888888), 0);
    lv_obj_align(_label_status, LV_ALIGN_TOP_MID, 0, 40);

    /* Volume slider (bottom, between track list and controls) */
    _slider_vol = lv_slider_create(screen);
    lv_obj_set_size(_slider_vol, 260, 15);
    lv_obj_align(_slider_vol, LV_ALIGN_BOTTOM_MID, 0, -95);
    lv_slider_set_range(_slider_vol, 0, 100);
    lv_slider_set_value(_slider_vol, 60, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(_slider_vol, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(_slider_vol, lv_color_hex(0x228833), LV_PART_INDICATOR);

    _label_vol = lv_label_create(screen);
    lv_label_set_text(_label_vol, "Vol: 60");
    lv_obj_set_style_text_color(_label_vol, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(_label_vol, &lv_font_montserrat_12, 0);
    lv_obj_align(_label_vol, LV_ALIGN_BOTTOM_MID, 0, -113);

    lv_obj_add_event_cb(_slider_vol, [](lv_event_t *e) {
        PhoneAppMusic *app = (PhoneAppMusic *)lv_event_get_user_data(e);
        app->_volume = (int)lv_slider_get_value(app->_slider_vol);
        char txt[16];
        snprintf(txt, sizeof(txt), "Vol: %d", app->_volume);
        lv_label_set_text(app->_label_vol, txt);
        safe_set_volume(app->_volume);
        /* Defer NVS write (debounce to avoid flash wear) */
        app->_nvs_dirty = true;
    }, LV_EVENT_VALUE_CHANGED, this);

    /* Control buttons (bottom) */
    int btn_w = 60, btn_h = 60;

    _btn_prev = lv_btn_create(screen);
    lv_obj_set_size(_btn_prev, btn_w, btn_h);
    lv_obj_align(_btn_prev, LV_ALIGN_BOTTOM_MID, -btn_w - 30, -20);
    lv_obj_add_event_cb(_btn_prev, [](lv_event_t *e) {
        ((PhoneAppMusic *)e->user_data)->_prev();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t *prev_lbl = lv_label_create(_btn_prev);
    lv_label_set_text(prev_lbl, LV_SYMBOL_PREV);
    lv_obj_center(prev_lbl);

    _btn_play = lv_btn_create(screen);
    lv_obj_set_size(_btn_play, btn_w + 10, btn_h + 10);
    lv_obj_align(_btn_play, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(_btn_play, lv_color_hex(0x228833), 0);
    lv_obj_add_event_cb(_btn_play, [](lv_event_t *e) {
        ((PhoneAppMusic *)e->user_data)->_pause_resume();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t *play_lbl = lv_label_create(_btn_play);
    lv_label_set_text(play_lbl, LV_SYMBOL_PLAY);
    lv_obj_center(play_lbl);

    _btn_next = lv_btn_create(screen);
    lv_obj_set_size(_btn_next, btn_w, btn_h);
    lv_obj_align(_btn_next, LV_ALIGN_BOTTOM_MID, btn_w + 30, -20);
    lv_obj_add_event_cb(_btn_next, [](lv_event_t *e) {
        ((PhoneAppMusic *)e->user_data)->_next();
    }, LV_EVENT_CLICKED, this);
    lv_obj_t *next_lbl = lv_label_create(_btn_next);
    lv_label_set_text(next_lbl, LV_SYMBOL_NEXT);
    lv_obj_center(next_lbl);

    /* No files label */
    _label_no_files = lv_label_create(screen);
    lv_label_set_text(_label_no_files, "No music files on SD card\nAdd .mp3 or .wav files");
    lv_obj_set_style_text_color(_label_no_files, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(_label_no_files, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(_label_no_files, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_label_no_files, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(_label_no_files, LV_OBJ_FLAG_HIDDEN);

    /* Track list */
    _list_tracks = lv_list_create(screen);
    lv_obj_set_size(_list_tracks, BSP_LCD_H_RES - 20, BSP_LCD_V_RES - 180);
    lv_obj_align(_list_tracks, LV_ALIGN_TOP_MID, 0, 60);

    /* Restore NVS volume */
    {
        nvs_handle_t nvs_h;
        if (nvs_open("settings", NVS_READONLY, &nvs_h) == ESP_OK) {
            int32_t saved_vol = (int32_t)_volume;
            if (nvs_get_i32(nvs_h, "volume", &saved_vol) == ESP_OK) {
                if (saved_vol >= 0 && saved_vol <= 100) _volume = (int)saved_vol;
            }
            nvs_close(nvs_h);
        }
    }

    /* Set initial volume on codec */
    safe_set_volume(_volume);

    /* Deferred auto-next timer: checks _auto_next flag to avoid GMF re-entrancy.
     * _asp_event_cb sets _auto_next=true instead of calling _next() directly. */
    _auto_next_timer = lv_timer_create(_auto_next_timer_cb, 200, this);

    /* NVS debounce timer (500ms): avoid flash wear from rapid slider events. */
    _nvs_save_timer = lv_timer_create([](lv_timer_t *t) {
        PhoneAppMusic *app = (PhoneAppMusic *)t->user_data;
        if (!app || !app->_nvs_dirty) return;
        app->_nvs_dirty = false;
        nvs_handle_t nvs_h;
        if (nvs_open("settings", NVS_READWRITE, &nvs_h) == ESP_OK) {
            nvs_set_i32(nvs_h, "volume", (int32_t)app->_volume);
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
        }
    }, 500, this);

    /* Volume sync timer: pull volume from NVS every 5s (Settings may change it). */
    _vol_sync_timer = lv_timer_create([](lv_timer_t *t) {
        PhoneAppMusic *app = (PhoneAppMusic *)t->user_data;
        if (!app || !app->_label_vol || !app->_slider_vol) return;
        nvs_handle_t nvs_h;
        if (nvs_open("settings", NVS_READONLY, &nvs_h) == ESP_OK) {
            int32_t saved_vol = (int32_t)app->_volume;
            if (nvs_get_i32(nvs_h, "volume", &saved_vol) == ESP_OK) {
                if (saved_vol >= 0 && saved_vol <= 100 && saved_vol != (int32_t)app->_volume) {
                    app->_volume = (int)saved_vol;
                    lv_slider_set_value(app->_slider_vol, saved_vol, LV_ANIM_OFF);
                    char txt[16];
                    snprintf(txt, sizeof(txt), "Vol: %ld", saved_vol);
                    lv_label_set_text(app->_label_vol, txt);
                    if (s_codec_handle) safe_set_volume((int)saved_vol);
                }
            }
            nvs_close(nvs_h);
        }
    }, 5000, this);

    /* Scan SD card */
    _scan_files();

    if (_track_count == 0) {
        lv_label_set_text(_label_status, "No files found");
        lv_obj_remove_flag(_label_no_files, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_list_tracks, LV_OBJ_FLAG_HIDDEN);
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d tracks found", _track_count);
        lv_label_set_text(_label_status, buf);
    }

    ESP_LOGI(TAG, "Music app running (%d tracks)", _track_count);
    return true;
}

bool PhoneAppMusic::back(void)
{
    ESP_BROOKESIA_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
    return true;
}

bool PhoneAppMusic::close(void)
{
    _stop();
    /* Clean up auto-next timer */
    if (_auto_next_timer) {
        lv_timer_delete(_auto_next_timer);
        _auto_next_timer = nullptr;
    }
    /* Clean up volume sync timer */
    if (_vol_sync_timer) {
        lv_timer_delete(_vol_sync_timer);
        _vol_sync_timer = nullptr;
    }
    /* Flush pending NVS write */
    if (_nvs_save_timer) {
        lv_timer_delete(_nvs_save_timer);
        _nvs_save_timer = nullptr;
    }
    if (_nvs_dirty) {
        _nvs_dirty = false;
        nvs_handle_t nvs_h;
        if (nvs_open("settings", NVS_READWRITE, &nvs_h) == ESP_OK) {
            nvs_set_i32(nvs_h, "volume", (int32_t)_volume);
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
        }
    }
    // Free file names
    for (int i = 0; i < _track_count; i++) {
        if (_file_names[i]) { free(_file_names[i]); _file_names[i] = nullptr; }
    }
    if (_asp_handle) {
        esp_audio_simple_player_destroy(_asp_handle);
        _asp_handle = nullptr;
    }

    /* Deinit audio and SD card — release DMA/PSRAM resources */
    monitor_deinit_audio();
    monitor_deinit_sdcard();

    ESP_LOGI(TAG, "Music app closed");
    return true;
}

/*============================================================================
 * File Scanning
 *============================================================================*/

void PhoneAppMusic::_scan_files(void)
{
    struct dirent *entry;
    DIR *dir = opendir(MUSIC_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s (SD card not mounted?)", MUSIC_DIR);
        _track_count = 0;
        return;
    }

    int count = 0;
    while ((entry = readdir(dir)) != NULL && count < MAX_TRACKS) {
        const char *name = entry->d_name;
        if (name[0] == '.') continue;  /* skip hidden files, ._*, .DS_Store etc */
        if (entry->d_type != DT_REG) continue;
        size_t len = strlen(name);
        if (len < 5) continue;
        const char *ext = name + len - 4;
        if (strcasecmp(ext, ".mp3") != 0 && strcasecmp(ext, ".wav") != 0) continue;
        _file_names[count] = strdup(name);
        if (!_file_names[count]) continue;  // OOM, skip this file
        count++;
    }
    closedir(dir);
    _track_count = count;

    ESP_LOGI(TAG, "Found %d music files in %s", _track_count, MUSIC_DIR);

    for (int i = 0; i < _track_count; i++) {
        lv_obj_t *btn = lv_list_add_btn(_list_tracks, NULL, _file_names[i]);
        lv_obj_add_event_cb(btn, [](lv_event_t *e) {
            lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
            int idx = (int)(intptr_t)lv_obj_get_user_data(obj);
            PhoneAppMusic *app = (PhoneAppMusic *)lv_event_get_user_data(e);
            app->_play(idx);
        }, LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
    }
}

/*============================================================================
 * Playback Controls
 *============================================================================*/

void PhoneAppMusic::_play(int index)
{
    if (index < 0 || index >= _track_count) return;

    /* Stop + destroy previous player for clean pipeline state (S31 fix).
     * Recreate fresh ASP handle each play to prevent GMF state residue crashes. */
    if (_asp_handle) {
        esp_audio_simple_player_stop(_asp_handle);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_audio_simple_player_destroy(_asp_handle);
        _asp_handle = nullptr;
    }
    _is_playing = false;
    _current_track = -1;

    const char *name = _file_names[index];
    char uri[320];
    snprintf(uri, sizeof(uri), "file://sdcard/%s", name);

    ESP_LOGI(TAG, "Playing: %s", uri);
    lv_label_set_text(_label_title, name);

    /* Create fresh ASP handle */
    esp_asp_cfg_t asp_cfg = {
        .out = { .cb = _asp_output_cb, .user_ctx = this },
        .task_prio = 5,
        .task_stack = 4096,
        .task_core = 0,
    };
    if (esp_audio_simple_player_new(&asp_cfg, &_asp_handle) != ESP_GMF_ERR_OK || !_asp_handle) {
        ESP_LOGE(TAG, "Failed to create player");
        lv_label_set_text(_label_status, "Init failed");
        return;
    }
    esp_audio_simple_player_set_event(_asp_handle, _asp_event_cb, this);

    esp_gmf_err_t ret = esp_audio_simple_player_run(_asp_handle, uri, NULL);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to play %s: %d", uri, ret);
        lv_label_set_text(_label_status, "Play failed");
        return;
    }
    _is_playing = true;
    _current_track = index;
    lv_obj_t *btn = lv_obj_get_child(_btn_play, 0);
    lv_label_set_text(btn, LV_SYMBOL_PAUSE);

    char buf[64];
    snprintf(buf, sizeof(buf), "%d/%d %s", index + 1, _track_count, name);
    lv_label_set_text(_label_status, buf);
}

void PhoneAppMusic::_pause_resume(void)
{
    if (_current_track < 0) {
        if (_track_count > 0) _play(0);
        return;
    }
    if (_is_playing) {
        esp_audio_simple_player_pause(_asp_handle);
        _is_playing = false;
        lv_obj_t *btn = lv_obj_get_child(_btn_play, 0);
        lv_label_set_text(btn, LV_SYMBOL_PLAY);
    } else {
        esp_audio_simple_player_resume(_asp_handle);
        _is_playing = true;
        lv_obj_t *btn = lv_obj_get_child(_btn_play, 0);
        lv_label_set_text(btn, LV_SYMBOL_PAUSE);
    }
}

void PhoneAppMusic::_stop(void)
{
    if (_asp_handle) {
        esp_audio_simple_player_stop(_asp_handle);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_audio_simple_player_destroy(_asp_handle);
        _asp_handle = nullptr;
        _is_playing = false;
        _current_track = -1;
        lv_obj_t *btn = lv_obj_get_child(_btn_play, 0);
        lv_label_set_text(btn, LV_SYMBOL_PLAY);
        lv_label_set_text(_label_title, "Music Player");
        lv_label_set_text(_label_status, "Stopped");
    }
}

void PhoneAppMusic::_next(void)
{
    if (_track_count == 0) return;
    int next = (_current_track + 1) % _track_count;
    _play(next);
}

void PhoneAppMusic::_prev(void)
{
    if (_track_count == 0) return;
    int prev = (_current_track <= 0) ? _track_count - 1 : _current_track - 1;
    _play(prev);
}

/*============================================================================
 * ESP Audio Simple Player Callbacks (static members)
 *============================================================================*/

/* Output callback: receives decoded PCM data, writes to ES8311 codec */
int PhoneAppMusic::_asp_output_cb(uint8_t *data, int data_size, void *ctx)
{
    (void)ctx;
    if (!s_codec_handle || !s_codec_mutex || data_size <= 0) return -1;
    if (xSemaphoreTake(s_codec_mutex, pdMS_TO_TICKS(50)) != pdPASS) return -1;
    int ret = esp_codec_dev_write(s_codec_handle, data, data_size);
    xSemaphoreGive(s_codec_mutex);
    return (ret == ESP_CODEC_DEV_OK) ? data_size : -1;
}

/* Event callback: handles state transitions */
int PhoneAppMusic::_asp_event_cb(esp_asp_event_pkt_t *pkt, void *ctx)
{
    PhoneAppMusic *app = (PhoneAppMusic *)ctx;
    if (pkt->type == ESP_ASP_EVENT_TYPE_STATE) {
        esp_asp_state_t state = *(esp_asp_state_t *)pkt->payload;
        switch (state) {
        case ESP_ASP_STATE_FINISHED:
            ESP_LOGI(TAG, "Playback finished, advancing to next track");
            app->_is_playing = false;
            app->_auto_next = true;  // Defer _next() to LVGL timer (avoid GMF re-entrancy)
            break;
        case ESP_ASP_STATE_ERROR:
            ESP_LOGW(TAG, "Playback error, skipping to next");
            app->_is_playing = false;
            app->_auto_next = true;  // Defer _next() to LVGL timer (avoid GMF re-entrancy)
            break;
        case ESP_ASP_STATE_STOPPED:
            // User-initiated stop, already handled in _stop()
            break;
        default:
            break;
        }
    }
    return 0;
}

/*============================================================================
 * Auto-Next Timer: polls _auto_next to safely advance track from LVGL context
 *============================================================================*/
void PhoneAppMusic::_auto_next_timer_cb(lv_timer_t *timer)
{
    PhoneAppMusic *app = (PhoneAppMusic *)timer->user_data;
    if (!app || !app->_auto_next) return;
    app->_auto_next = false;
    app->_next();
}
