#include "phone_app_audio.hpp"
#include "private/esp_brookesia_utils.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "bsp/display.h"
#include "example_config.h"
#include <string.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/time.h>

static const char *TAG = "PhoneAppAudio";

/* External audio handles from main.cpp */
extern i2s_chan_handle_t s_rx_handle;              // I2S RX for mic

/* External launcher icon from brookesia */
extern const lv_image_dsc_t esp_brookesia_image_large_app_launcher_default_112_112;

/* Audio buffer: 480 samples * 2 channels * 2 bytes = 1920 bytes ~10ms @48kHz */
#define AUDIO_BUF_SAMPLES   480
#define AUDIO_BUF_BYTES     (AUDIO_BUF_SAMPLES * 2 * sizeof(int16_t))

/* Recording directory on SD card */
#define RECORD_DIR          "/sdcard"

/* Shine encoder expects SHINE_MAX_SAMPLES (1152) samples per channel per frame */
#define ENC_SAMPLES_PER_CH  SHINE_MAX_SAMPLES
#define PCM_BUF_SAMPLES     (ENC_SAMPLES_PER_CH * 2)  /* interleaved stereo */

PhoneAppAudio::PhoneAppAudio(bool use_status_bar, bool use_navigation_bar) :
    ESP_Brookesia_PhoneApp("Audio", &esp_brookesia_image_large_app_launcher_default_112_112,
                           true /* use_default_screen */,
                           use_status_bar, use_navigation_bar),
    _task_handle(nullptr), _task_running(false),
    _is_recording(false), _encoder(nullptr), _record_file(nullptr),
    _pcm_buffer(nullptr), _pcm_buf_count(0),
    _record_bytes_written(0), _record_start_ms(0),
    _recording_count(0),
    _btn_back(nullptr), _btn_record(nullptr),
    _label_rec_status(nullptr), _label_footer(nullptr),
    _list_recordings(nullptr), _label_no_recs(nullptr),
    _update_timer(nullptr)
{
    memset(_recording_names, 0, sizeof(_recording_names));
}

PhoneAppAudio::~PhoneAppAudio()
{
    if (_is_recording) {
        _stop_recording();
    }
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

    /* Record button (top-right) */
    _btn_record = lv_btn_create(screen);
    lv_obj_set_size(_btn_record, 60, 50);
    lv_obj_align(_btn_record, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(_btn_record, lv_color_hex(0xCC2222), 0);
    lv_obj_set_style_bg_opa(_btn_record, LV_OPA_70, 0);
    lv_obj_set_style_radius(_btn_record, 10, 0);
    lv_obj_add_event_cb(_btn_record, [](lv_event_t *e) {
        PhoneAppAudio *app = (PhoneAppAudio *)e->user_data;
        if (!app->_is_recording) {
            app->_start_recording();
        } else {
            app->_stop_recording();
        }
    }, LV_EVENT_CLICKED, this);
    lv_obj_t *rec_label = lv_label_create(_btn_record);
    lv_label_set_text(rec_label, "REC");
    lv_obj_center(rec_label);

    /* Recording status label (below record button) */
    _label_rec_status = lv_label_create(screen);
    lv_label_set_text(_label_rec_status, "");
    lv_obj_set_style_text_color(_label_rec_status, lv_color_hex(0xCC4444), 0);
    lv_obj_set_style_text_font(_label_rec_status, &lv_font_montserrat_12, 0);
    lv_obj_align(_label_rec_status, LV_ALIGN_TOP_RIGHT, -10, 65);

    /* Recordings list (top-left, fills most of the screen) */
    _list_recordings = lv_list_create(screen);
    lv_obj_set_size(_list_recordings, screen_w - 20, screen_h - 90);
    lv_obj_align(_list_recordings, LV_ALIGN_TOP_LEFT, 10, 80);

    /* No recordings label */
    _label_no_recs = lv_label_create(screen);
    lv_label_set_text(_label_no_recs, "No recordings\nPress REC to start");
    lv_obj_set_style_text_color(_label_no_recs, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_align(_label_no_recs, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_label_no_recs, LV_ALIGN_CENTER, 0, 30);

    /* Footer */
    _label_footer = lv_label_create(screen);
    lv_label_set_text(_label_footer, "Audio Recorder");
    lv_obj_set_style_text_color(_label_footer, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(_label_footer, &lv_font_montserrat_10, 0);
    lv_obj_align(_label_footer, LV_ALIGN_BOTTOM_MID, 0, -5);

    /* Scan existing recordings */
    _scan_recordings();

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

    if (_is_recording) {
        _stop_recording();
    }

    _task_running = false;
    if (_update_timer) {
        lv_timer_delete(_update_timer);
        _update_timer = nullptr;
    }

    /* Wait for task to finish */
    vTaskDelay(pdMS_TO_TICKS(150));

    /* Free recording names */
    for (int i = 0; i < _recording_count; i++) {
        if (_recording_names[i]) { free(_recording_names[i]); _recording_names[i] = nullptr; }
    }

    ESP_LOGI(TAG, "Audio app closed");
    return true;
}

/*============================================================================
 * Audio Echo Task (with optional MP3 recording)
 *============================================================================*/

void PhoneAppAudio::_audio_task(void *arg)
{
    PhoneAppAudio *app = (PhoneAppAudio *)arg;
    int16_t *buf = (int16_t *)heap_caps_calloc(1, AUDIO_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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

        int32_t samples_per_ch = (int32_t)(bytes_read / (2 * sizeof(int16_t)));

        /* If recording, accumulate PCM and encode to MP3 */
        if (app->_is_recording && app->_pcm_buffer && app->_encoder) {
            for (int32_t i = 0; i < samples_per_ch; i++) {
                int idx = app->_pcm_buf_count * 2;
                app->_pcm_buffer[idx]     = buf[i * 2];
                app->_pcm_buffer[idx + 1] = buf[i * 2 + 1];
                app->_pcm_buf_count++;

                if (app->_pcm_buf_count >= ENC_SAMPLES_PER_CH) {
                    /* Encode one frame of MP3 */
                    int written = 0;
                    unsigned char *mp3_data = shine_encode_buffer_interleaved(
                        app->_encoder, app->_pcm_buffer, &written);
                    if (mp3_data && written > 0 && app->_record_file) {
                        size_t wr = fwrite(mp3_data, 1, written, app->_record_file);
                        app->_record_bytes_written += wr;
                    }
                    app->_pcm_buf_count = 0;
                }
            }
        }
    }

    free(buf);
    vTaskDelete(nullptr);
}

/*============================================================================
 * LVGL Update Timer (20Hz)
 *============================================================================*/

void PhoneAppAudio::_level_update_timer_cb(lv_timer_t *timer)
{
    PhoneAppAudio *app = (PhoneAppAudio *)timer->user_data;
    if (!app || !app->_task_running) return;

    /* Update recording status */
    if (app->_label_rec_status && app->_is_recording) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        uint32_t now_ms = (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
        uint32_t elapsed_s = (now_ms - app->_record_start_ms) / 1000;
        uint32_t kb = app->_record_bytes_written / 1024;
        char txt[48];
        snprintf(txt, sizeof(txt), "REC %lu:%02lu  %luKB",
                 elapsed_s / 60, elapsed_s % 60, (unsigned long)kb);
        lv_label_set_text(app->_label_rec_status, txt);
    }
}

/*============================================================================
 * Recording: start
 *============================================================================*/

void PhoneAppAudio::_start_recording(void)
{
    if (_is_recording) return;

    ESP_LOGI(TAG, "Starting MP3 recording...");

    /* Allocate PCM accumulation buffer (1152 samples * 2 channels interleaved) */
    _pcm_buffer = (int16_t *)heap_caps_calloc(1, PCM_BUF_SAMPLES * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_pcm_buffer) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer");
        return;
    }
    _pcm_buf_count = 0;

    /* Configure Shine encoder: 48kHz stereo, 128kbps */
    shine_config_t cfg;
    shine_set_config_mpeg_defaults(&cfg.mpeg);
    cfg.wave.channels = PCM_STEREO;
    cfg.wave.samplerate = 48000;
    cfg.mpeg.mode = STEREO;
    cfg.mpeg.bitr = 128;

    if (shine_check_config(cfg.wave.samplerate, cfg.mpeg.bitr) < 0) {
        ESP_LOGE(TAG, "Invalid encoder config: %dHz %dkbps",
                 cfg.wave.samplerate, cfg.mpeg.bitr);
        free(_pcm_buffer);
        _pcm_buffer = nullptr;
        return;
    }

    _encoder = shine_initialise(&cfg);
    if (!_encoder) {
        ESP_LOGE(TAG, "Failed to initialize MP3 encoder");
        free(_pcm_buffer);
        _pcm_buffer = nullptr;
        return;
    }

    /* Open file on SD card */
    char path[128];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    snprintf(path, sizeof(path), RECORD_DIR "/rec_%04d%02d%02d_%02d%02d%02d.mp3",
             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);

    _record_file = fopen(path, "wb");
    if (!_record_file) {
        ESP_LOGE(TAG, "Failed to create file: %s", path);
        shine_close(_encoder);
        _encoder = nullptr;
        free(_pcm_buffer);
        _pcm_buffer = nullptr;
        return;
    }

    _record_bytes_written = 0;
    _record_start_ms = (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
    _is_recording = true;

    /* Update button appearance */
    lv_obj_set_style_bg_color(_btn_record, lv_color_hex(0x666666), 0);
    lv_obj_t *btn_label = lv_obj_get_child(_btn_record, 0);
    if (btn_label) lv_label_set_text(btn_label, LV_SYMBOL_STOP);

    ESP_LOGI(TAG, "Recording to: %s", path);
}

/*============================================================================
 * Recording: stop
 *============================================================================*/

void PhoneAppAudio::_stop_recording(void)
{
    if (!_is_recording) return;

    ESP_LOGI(TAG, "Stopping MP3 recording...");
    _is_recording = false;

    /* Give audio task time to exit recording block */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Flush remaining encoded data */
    if (_encoder) {
        int written = 0;
        unsigned char *data = shine_flush(_encoder, &written);
        if (data && written > 0 && _record_file) {
            fwrite(data, 1, written, _record_file);
        }
        shine_close(_encoder);
        _encoder = nullptr;
    }

    /* Close file */
    if (_record_file) {
        fclose(_record_file);
        _record_file = nullptr;
    }

    /* Free PCM buffer */
    if (_pcm_buffer) {
        free(_pcm_buffer);
        _pcm_buffer = nullptr;
    }

    /* Update button appearance */
    lv_obj_set_style_bg_color(_btn_record, lv_color_hex(0xCC2222), 0);
    lv_obj_t *btn_label = lv_obj_get_child(_btn_record, 0);
    if (btn_label) lv_label_set_text(btn_label, "REC");

    if (_label_rec_status) {
        lv_label_set_text(_label_rec_status, "Saved!");
    }

    /* Refresh recordings list */
    _scan_recordings();

    ESP_LOGI(TAG, "Recording saved (%lu bytes)", (unsigned long)_record_bytes_written);
}

/*============================================================================
 * Scan SD card for MP3 recordings
 *============================================================================*/

void PhoneAppAudio::_scan_recordings(void)
{
    /* Free old list */
    for (int i = 0; i < _recording_count; i++) {
        if (_recording_names[i]) { free(_recording_names[i]); _recording_names[i] = nullptr; }
    }
    _recording_count = 0;

    /* Clear list UI */
    if (_list_recordings) {
        lv_obj_clean(_list_recordings);
    }

    struct dirent *entry;
    DIR *dir = opendir(RECORD_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s (SD card not mounted?)", RECORD_DIR);
        if (_label_no_recs) lv_obj_remove_flag(_label_no_recs, LV_OBJ_FLAG_HIDDEN);
        if (_list_recordings) lv_obj_add_flag(_list_recordings, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    int count = 0;
    while ((entry = readdir(dir)) != NULL && count < MAX_RECORDINGS) {
        const char *name = entry->d_name;
        if (name[0] == '.') continue;
        if (entry->d_type != DT_REG) continue;
        size_t len = strlen(name);
        if (len < 5) continue;
        const char *ext = name + len - 4;
        if (strcasecmp(ext, ".mp3") != 0) continue;
        _recording_names[count] = strdup(name);
        count++;
    }
    closedir(dir);
    _recording_count = count;

    ESP_LOGI(TAG, "Found %d recording(s) in %s", _recording_count, RECORD_DIR);

    if (_recording_count == 0) {
        if (_label_no_recs) lv_obj_remove_flag(_label_no_recs, LV_OBJ_FLAG_HIDDEN);
        if (_list_recordings) lv_obj_add_flag(_list_recordings, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (_label_no_recs) lv_obj_add_flag(_label_no_recs, LV_OBJ_FLAG_HIDDEN);
        if (_list_recordings) {
            lv_obj_remove_flag(_list_recordings, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < _recording_count; i++) {
                lv_list_add_btn(_list_recordings, LV_SYMBOL_AUDIO, _recording_names[i]);
            }
        }
    }
}
