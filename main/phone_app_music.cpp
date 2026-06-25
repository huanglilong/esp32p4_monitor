#include "phone_app_music.hpp"
#include "private/esp_brookesia_utils.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "audio_player.h"
#include "bsp/display.h"
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "MusicApp";

/* External handles from main.cpp */
extern esp_codec_dev_handle_t s_codec_handle;

/* External icon from brookesia */
extern const lv_image_dsc_t esp_brookesia_image_large_app_launcher_default_112_112;

#define MUSIC_DIR  "/sdcard"
#define MAX_TRACKS 50

/* Forward decls for audio player callbacks */
static esp_err_t _player_mute_cb(AUDIO_PLAYER_MUTE_SETTING setting);
static esp_err_t _player_write_cb(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms);
static esp_err_t _player_clk_set_cb(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch);

PhoneAppMusic::PhoneAppMusic(bool use_status_bar, bool use_navigation_bar) :
    ESP_Brookesia_PhoneApp("Music", &esp_brookesia_image_large_app_launcher_default_112_112,
                           true, use_status_bar, use_navigation_bar),
    _current_track(-1), _is_playing(false), _track_count(0),
    _file_iter(nullptr),
    _btn_back(nullptr), _label_title(nullptr), _label_status(nullptr),
    _btn_prev(nullptr), _btn_play(nullptr), _btn_next(nullptr),
    _list_tracks(nullptr), _label_no_files(nullptr),
    _slider_vol(nullptr), _label_vol(nullptr),
    _volume(60)
{
    memset(_file_names, 0, sizeof(_file_names));
}

PhoneAppMusic::~PhoneAppMusic()
{
    _stop();
}

bool PhoneAppMusic::run(void)
{
    ESP_LOGI(TAG, "Music app starting...");
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
        if (s_codec_handle) esp_codec_dev_set_out_vol(s_codec_handle, app->_volume);
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

    /* Init audio player */
    audio_player_config_t cfg = {
        .mute_fn = _player_mute_cb,
        .clk_set_fn = _player_clk_set_cb,
        .write_fn = _player_write_cb,
        .priority = 5,
        .coreID = 0,
    };
    audio_player_new(cfg);
    audio_player_callback_register(PhoneAppMusic::_player_event_cb_static, this);

    /* Set initial volume on codec */
    if (s_codec_handle) esp_codec_dev_set_out_vol(s_codec_handle, _volume);

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
    // Free file names
    for (int i = 0; i < _track_count; i++) {
        if (_file_names[i]) { free(_file_names[i]); _file_names[i] = nullptr; }
    }
    audio_player_delete();
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

    _stop();
    _current_track = index;

    const char *name = _file_names[index];
    char path[256];
    snprintf(path, sizeof(path), MUSIC_DIR "/%s", name);

    ESP_LOGI(TAG, "Playing: %s", path);
    lv_label_set_text(_label_title, name);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open: %s", path);
        lv_label_set_text(_label_status, "Open failed");
        return;
    }

    audio_player_play(fp);
    _is_playing = true;
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
        audio_player_pause();
        _is_playing = false;
        lv_obj_t *btn = lv_obj_get_child(_btn_play, 0);
        lv_label_set_text(btn, LV_SYMBOL_PLAY);
    } else {
        audio_player_resume();
        _is_playing = true;
        lv_obj_t *btn = lv_obj_get_child(_btn_play, 0);
        lv_label_set_text(btn, LV_SYMBOL_PAUSE);
    }
}

void PhoneAppMusic::_stop(void)
{
    if (_current_track >= 0) {
        audio_player_stop();
        _is_playing = false;
        _current_track = -1;
        lv_obj_t *btn = lv_obj_get_child(_btn_play, 0);
        lv_label_set_text(btn, LV_SYMBOL_PLAY);
        lv_label_set_text(_label_title, "Music Player");
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
 * Audio Player Callbacks
 *============================================================================*/

static esp_err_t _player_mute_cb(AUDIO_PLAYER_MUTE_SETTING setting)
{
    if (s_codec_handle) {
        esp_codec_dev_set_out_mute(s_codec_handle, (setting == AUDIO_PLAYER_MUTE));
    }
    return ESP_OK;
}

static esp_err_t _player_write_cb(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    if (!s_codec_handle) {
        *bytes_written = 0;
        return ESP_FAIL;
    }
    int ret = esp_codec_dev_write(s_codec_handle, audio_buffer, (int)len);
    /* esp_codec_dev_write returns ESP_CODEC_DEV_OK (0) on success, not byte count */
    if (ret == ESP_CODEC_DEV_OK) {
        if (bytes_written) *bytes_written = len;
        return ESP_OK;
    }
    ESP_LOGW(TAG, "codec write err: %d", ret);
    *bytes_written = 0;
    return ESP_FAIL;
}

static esp_err_t _player_clk_set_cb(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    /* Keep I2S at fixed 48kHz. Helix MP3 decoder handles rate mismatch internally.
     * Per-file reconfig causes DMA disruption → decoder error -9. */
    ESP_LOGI(TAG, "Clock set request: %luHz (keeping 48000Hz)", rate);
    return ESP_OK;
}

void PhoneAppMusic::_player_event_cb_static(audio_player_cb_ctx_t *ctx)
{
    PhoneAppMusic *app = (PhoneAppMusic *)ctx->user_ctx;
    switch (ctx->audio_event) {
    case AUDIO_PLAYER_CALLBACK_EVENT_IDLE:
    case AUDIO_PLAYER_CALLBACK_EVENT_PLAYING:
    case AUDIO_PLAYER_CALLBACK_EVENT_PAUSE:
    case AUDIO_PLAYER_CALLBACK_EVENT_SHUTDOWN:
        break;  // States handled by UI timer
    case AUDIO_PLAYER_CALLBACK_EVENT_COMPLETED_PLAYING_NEXT:
        break;  // Manual track selection only
    case AUDIO_PLAYER_CALLBACK_EVENT_UNKNOWN_FILE_TYPE:
        ESP_LOGW(TAG, "Unknown file type - skipping");
        break;
    default:
        break;
    }
}
