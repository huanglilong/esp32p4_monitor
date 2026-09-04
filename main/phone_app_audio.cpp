#include "phone_app_audio.hpp"
#include "peripherals.hpp"
#include "private/esp_brookesia_utils.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "bsp/display.h"
#include "example_config.h"
#include <string.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/time.h>

static const char *TAG = "PhoneAppAudio";

/* External launcher icon from brookesia */
extern const lv_image_dsc_t esp_brookesia_image_large_app_launcher_default_112_112;

/* Audio buffer: 480 interleaved samples * 2 bytes = 1920 bytes ~15ms @16kHz stereo (100ms timeout) */
#define AUDIO_BUF_SAMPLES   480
#define AUDIO_BUF_BYTES     (AUDIO_BUF_SAMPLES * 2 * sizeof(int16_t))

/* AAC encoder expects 1024 samples per channel per frame */
#define AAC_SAMPLES_PER_CH  1024
#define PCM_BUF_SAMPLES     (AAC_SAMPLES_PER_CH * 2)  /* interleaved stereo */

PhoneAppAudio::PhoneAppAudio(bool use_status_bar, bool use_navigation_bar) :
    ESP_Brookesia_PhoneApp("Audio", &esp_brookesia_image_large_app_launcher_default_112_112,
                           true /* use_default_screen */,
                           use_status_bar, use_navigation_bar),
    _task_handle{nullptr}, _task_running{false},
    _audio_stack(nullptr), _audio_tcb(nullptr),
    _is_recording{false}, _recording_ops_in_flight{0},
    _encoder(nullptr), _record_file(nullptr),
    _pcm_buffer(nullptr), _pcm_buf_count(0),
    _enc_in_buf(nullptr), _enc_out_buf(nullptr),
    _enc_in_size(0), _enc_out_size(0),
    _record_bytes_written{0}, _record_start_ms{0},
    _rec_pub{ORB_ADVERT_INVALID},
    _recording_count(0),
    _btn_back(nullptr), _btn_record(nullptr),
    _label_rec_status(nullptr), _label_footer(nullptr),
    _list_recordings(nullptr), _label_no_recs(nullptr),
    _update_timer(nullptr)
{
    memset(_recording_names, 0, sizeof(_recording_names));
    /* Pre-allocate TCB at construction — reused across run/close cycles.
     * ~340B internal SRAM, avoids TCB use-after-free race with idle task. */
    _audio_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

PhoneAppAudio::~PhoneAppAudio()
{
    if (_is_recording) {
        _stop_recording();
    }
    if (_task_running) {
        _task_running = false;
        /* Wait up to 2s for task to exit (matches close()).
         * Task sets _task_handle = nullptr before vTaskDelete(NULL),
         * so we check the handle rather than eTaskGetState() which can
         * race with the idle task reclaiming the TCB. */
        int timeout = 0;
        while (timeout < 20) {
            if (!_task_handle.load(std::memory_order_acquire)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout++;
        }
        /* Atomically clear handle before vTaskDelete — prevents race where
         * task self-deletes between load() and exchange(), which would pass
         * nullptr to vTaskDelete and kill the calling task (S259). */
        TaskHandle_t h = _task_handle.exchange(nullptr, std::memory_order_acq_rel);
        if (h != nullptr) {
            ESP_LOGW(TAG, "Audio task did not exit within 2s, force-killing");
            vTaskDelete(h);
        }
    }
    /* Free PSRAM-allocated stack (xTaskCreateStatic does not free them).
     * Defensive: close() normally frees the stack, but guard against any
     * path that destroys the object before close() ran. */
    if (_audio_stack) { heap_caps_free(_audio_stack); _audio_stack = nullptr; }
    /* TCB is pre-allocated at construction and never freed — ~340B internal
     * SRAM, negligible cost for a permanent singleton in embedded firmware.
     * Avoids TCB use-after-free race with idle task entirely. */
    /* Delete update timer (defensive — close() normally does this) */
    if (_update_timer) {
        lv_timer_delete(_update_timer);
        _update_timer = nullptr;
    }
    /* Reset uORB recording_state publisher handle */
    _rec_pub.store(ORB_ADVERT_INVALID, std::memory_order_release);

    /* Free recording names (defensive — close() normally does this) */
    for (int i = 0; i < _recording_count; i++) {
        if (_recording_names[i]) { free(_recording_names[i]); _recording_names[i] = nullptr; }
    }
}

bool PhoneAppAudio::run(void)
{
    ESP_LOGI(TAG, "Audio app starting...");

    /* Init audio I2S + codecs (mic + speaker) */
    PeripheralManager::instance().init_audio();

    /* Init SD card for recording storage */
    PeripheralManager::instance().init_sdcard();

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

    /* Start audio echo task — allocate stack in PSRAM to save internal SRAM.
     * TCB was pre-allocated at construction and reused across run/close cycles.
     * Stack (12KB) is allocated from PSRAM per run and freed in close(). */
    _task_running = true;
    /* xTaskCreateStatic depth (12288) is in StackType_t words; buffer must be
     * depth * sizeof(StackType_t) bytes, else the kernel writes 4x past the alloc. */
    _audio_stack = (StackType_t *)heap_caps_malloc(12288 * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_audio_stack || !_audio_tcb) {
        ESP_LOGE(TAG, "Failed to allocate audio task buffers");
        if (_audio_stack) { heap_caps_free(_audio_stack); _audio_stack = nullptr; }
        _task_running = false;
        return false;
    }
    TaskHandle_t h = xTaskCreateStatic(_audio_task, "audio_echo", 12288, this, 5, _audio_stack, _audio_tcb);
    _task_handle.store(h, std::memory_order_release);
    if (h == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio task");
        heap_caps_free(_audio_stack); _audio_stack = nullptr;
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

    /* Wait for task to finish gracefully.
     * Task may be blocked in i2s_channel_read (100ms timeout). 2s timeout is safe.
     * Task self-deletes with vTaskDelete(nullptr) and clears _task_handle.
     * Check _task_handle instead of eTaskGetState() which can race with
     * the idle task reclaiming the TCB after vTaskDelete(NULL). */
    int timeout = 0;
    while (timeout < 20) {
        if (!_task_handle.load(std::memory_order_acquire)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout++;
    }
    /* Atomically clear handle before vTaskDelete — prevents race where
     * task self-deletes between load() and exchange(), which would pass
     * nullptr to vTaskDelete and kill the calling task (S259). */
    TaskHandle_t h = _task_handle.exchange(nullptr, std::memory_order_acq_rel);
    if (h != nullptr) {
        ESP_LOGW(TAG, "Audio task did not exit within 2s, force-killing");
        vTaskDelete(h);
    }

    /* Free PSRAM-allocated stack (xTaskCreateStatic does not free it).
     * TCB is pre-allocated and reused — not freed here.
     * Stack is safe to free immediately: FreeRTOS doesn't reference it
     * after the task exits (idle task only reclaims the TCB). */
    if (_audio_stack) { heap_caps_free(_audio_stack); _audio_stack = nullptr; }

    /* Free recording names */
    for (int i = 0; i < _recording_count; i++) {
        if (_recording_names[i]) { free(_recording_names[i]); _recording_names[i] = nullptr; }
    }

    /* Reset uORB recording_state publisher handle */
    _rec_pub.store(ORB_ADVERT_INVALID, std::memory_order_release);

    /* Deinit audio — release DMA/PSRAM resources. SD card stays mounted. */
    PeripheralManager::instance().deinit_audio();

    ESP_LOGI(TAG, "Audio app closed");
    return true;
}

/*============================================================================
 * Audio Echo Task (with optional AAC recording)
 *============================================================================*/

void PhoneAppAudio::_audio_task(void *arg)
{
    PhoneAppAudio *app = (PhoneAppAudio *)arg;
    /* Use PSRAM: ESP32-P4 PSRAM is DMA-capable (200MHz octal), I2S DMA can access it.
     * This frees ~1.9KB internal SRAM per instance. */
    int16_t *buf = (int16_t *)heap_caps_calloc(1, AUDIO_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        app->_task_running = false;
        app->_task_handle.store(nullptr, std::memory_order_release);  /* Clear before self-delete to prevent double-free in close() */
        vTaskDelete(nullptr);
        return;
    }

    while (app->_task_running) {
        /* Read from mic via direct I2S RX */
        size_t bytes_read = 0;
        esp_err_t ret = i2s_channel_read(PeripheralManager::instance().rx_handle(), buf, AUDIO_BUF_BYTES, &bytes_read, pdMS_TO_TICKS(100));
        if (ret != ESP_OK || bytes_read == 0) {
            continue;
        }

        int32_t samples_per_ch = (int32_t)(bytes_read / (2 * sizeof(int16_t)));

        /* If recording, accumulate PCM and encode to AAC */
        if (app->_is_recording && app->_pcm_buffer && app->_encoder) {
            app->_recording_ops_in_flight.fetch_add(1, std::memory_order_acquire);
            /* Re-check under counter — _stop_recording waits for counter to drain */
            if (app->_is_recording && app->_pcm_buffer && app->_encoder) {
                for (int32_t i = 0; i < samples_per_ch; i++) {
                    int idx = app->_pcm_buf_count * 2;
                    app->_pcm_buffer[idx]     = buf[i * 2];
                    app->_pcm_buffer[idx + 1] = buf[i * 2 + 1];
                    app->_pcm_buf_count++;

                    if (app->_pcm_buf_count >= AAC_SAMPLES_PER_CH) {
                        /* Copy interleaved PCM into encoder input buffer and encode one AAC frame */
                        memcpy(app->_enc_in_buf, app->_pcm_buffer, app->_enc_in_size);
                        esp_audio_enc_in_frame_t in_frame = {
                            .buffer = app->_enc_in_buf,
                            .len = (uint32_t)app->_enc_in_size,
                        };
                        esp_audio_enc_out_frame_t out_frame = {
                            .buffer = app->_enc_out_buf,
                            .len = (uint32_t)app->_enc_out_size,
                        };
                        esp_audio_err_t ret = esp_aac_enc_process(app->_encoder, &in_frame, &out_frame);
                        if (ret == ESP_AUDIO_ERR_OK && out_frame.encoded_bytes > 0 && app->_record_file) {
                            size_t wr = fwrite(app->_enc_out_buf, 1, out_frame.encoded_bytes, app->_record_file);
                            app->_record_bytes_written += wr;
                        } else if (ret != ESP_AUDIO_ERR_OK) {
                            ESP_LOGW(TAG, "AAC encode error: %d", ret);
                        }
                        app->_pcm_buf_count = 0;
                    }
                }
            }
            app->_recording_ops_in_flight.fetch_sub(1, std::memory_order_release);
        }
    }

    heap_caps_free(buf);
    app->_task_handle.store(nullptr, std::memory_order_release);  /* Clear before self-delete to prevent double-free in close() */
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
        int64_t now_us = esp_timer_get_time();
        uint32_t elapsed_s = (uint32_t)((now_us / 1000 - app->_record_start_ms) / 1000);
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

    ESP_LOGI(TAG, "Starting AAC recording...");

    /* Allocate PCM accumulation buffer (1024 samples * 2 channels interleaved = 4096 bytes).
     * Use PSRAM: ESP32-P4 PSRAM is DMA-capable, freeing internal SRAM for critical allocations. */
    _pcm_buffer = (int16_t *)heap_caps_calloc(1, PCM_BUF_SAMPLES * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_pcm_buffer) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer");
        return;
    }
    _pcm_buf_count = 0;

    /* Configure AAC encoder: 16kHz stereo, 64kbps, ADTS mode */
    esp_aac_enc_config_t aac_cfg = {
        .sample_rate     = 16000,
        .channel         = ESP_AUDIO_DUAL,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate         = 64000,
        .adts_used       = true,
    };

    esp_audio_err_t ret = esp_aac_enc_open(&aac_cfg, sizeof(aac_cfg), &_encoder);
    if (ret != ESP_AUDIO_ERR_OK || !_encoder) {
        ESP_LOGE(TAG, "Failed to initialize AAC encoder: %d", ret);
        heap_caps_free(_pcm_buffer);
        _pcm_buffer = nullptr;
        return;
    }

    /* Get encoder frame sizes for buffer allocation */
    ret = esp_aac_enc_get_frame_size(_encoder, &_enc_in_size, &_enc_out_size);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to get AAC frame size: %d", ret);
        esp_aac_enc_close(_encoder);
        _encoder = nullptr;
        heap_caps_free(_pcm_buffer);
        _pcm_buffer = nullptr;
        return;
    }

    ESP_LOGI(TAG, "AAC frame sizes: in=%d bytes, out=%d bytes", _enc_in_size, _enc_out_size);

    /* Allocate encoder I/O buffers from PSRAM */
    _enc_in_buf = (uint8_t *)heap_caps_calloc(1, _enc_in_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    _enc_out_buf = (uint8_t *)heap_caps_calloc(1, _enc_out_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_enc_in_buf || !_enc_out_buf) {
        ESP_LOGE(TAG, "Failed to allocate AAC encoder buffers (in=%p out=%p)", _enc_in_buf, _enc_out_buf);
        if (_enc_in_buf) { heap_caps_free(_enc_in_buf); _enc_in_buf = nullptr; }
        if (_enc_out_buf) { heap_caps_free(_enc_out_buf); _enc_out_buf = nullptr; }
        esp_aac_enc_close(_encoder);
        _encoder = nullptr;
        heap_caps_free(_pcm_buffer);
        _pcm_buffer = nullptr;
        return;
    }

    /* Open file on SD card */
    if (!PeripheralManager::instance().init_sdcard()) {
        ESP_LOGE(TAG, "SD card not available, cannot start recording");
        esp_aac_enc_close(_encoder);
        _encoder = nullptr;
        heap_caps_free(_enc_in_buf); _enc_in_buf = nullptr;
        heap_caps_free(_enc_out_buf); _enc_out_buf = nullptr;
        heap_caps_free(_pcm_buffer);
        _pcm_buffer = nullptr;
        return;
    }
    char path[128];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    /* Check if wall-clock time is reasonable (year > 2020).
     * Before NTP sync, gettimeofday returns epoch (1970), causing
     * all recordings to get the same filename and overwrite each other. */
    if (tm_info.tm_year + 1900 > 2020) {
        snprintf(path, sizeof(path), SDMMC_MOUNT_POINT "/%lld.aac",
                 (long long)tv.tv_sec);
    } else {
        /* Fallback: use monotonic timer as unique identifier */
        uint32_t mono_ms = (uint32_t)(esp_timer_get_time() / 1000);
        snprintf(path, sizeof(path), SDMMC_MOUNT_POINT "/mono_%lu.aac", (unsigned long)mono_ms);
    }

    _record_file = fopen(path, "wb");
    if (!_record_file) {
        ESP_LOGE(TAG, "Failed to create file: %s", path);
        esp_aac_enc_close(_encoder);
        _encoder = nullptr;
        heap_caps_free(_enc_in_buf); _enc_in_buf = nullptr;
        heap_caps_free(_enc_out_buf); _enc_out_buf = nullptr;
        heap_caps_free(_pcm_buffer);
        _pcm_buffer = nullptr;
        return;
    }

    _record_bytes_written = 0;
    /* Use monotonic esp_timer for elapsed time (avoids gettimeofday overflow on 32-bit) */
    _record_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    _is_recording = true;

    /* Publish recording_state.active=true for cross-module notification (e.g. Music app) */
    orb_advert_t pub = _rec_pub.load(std::memory_order_acquire);
    if (pub < 0) {
        orb_advert_t new_pub = orb_advertise(ORB_ID(recording_state));
        if (!_rec_pub.compare_exchange_strong(pub, new_pub, std::memory_order_release, std::memory_order_relaxed)) {
            /* Another context set it first — discard our new handle */
        } else {
            pub = new_pub;
        }
    }
    if (pub >= 0) {
        struct recording_state_s rs = {};
        rs.timestamp = esp_timer_get_time();
        rs.active = true;
        rs.bytes_written = 0;
        rs.elapsed_ms = 0;
        orb_publish(ORB_ID(recording_state), pub, &rs);
    }

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

    ESP_LOGI(TAG, "Stopping AAC recording...");
    _is_recording = false;

    /* Wait for audio task to exit the recording block before freeing
     * _pcm_buffer/_encoder. The task increments _recording_ops_in_flight
     * while in the block; we poll until it drains (max 1s).
     * This replaces the previous vTaskDelay(200ms) heuristic which could
     * fail under heavy system load (S240). */
    for (int i = 0; i < 100 && _recording_ops_in_flight.load(std::memory_order_acquire) > 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Save file handle locally before nulling — task won't access it after _is_recording=false */
    FILE *file = _record_file;
    _record_file = nullptr;

    /* Encode remaining partial buffer (if any) and close encoder */
    if (_encoder) {
        if (_pcm_buf_count > 0 && _enc_in_buf && _enc_out_buf && file) {
            /* Partial frame: zero-pad remaining bytes and encode */
            int partial_bytes = _pcm_buf_count * 2 * sizeof(int16_t);
            memset(_enc_in_buf, 0, _enc_in_size);
            memcpy(_enc_in_buf, _pcm_buffer, partial_bytes < _enc_in_size ? partial_bytes : _enc_in_size);
            esp_audio_enc_in_frame_t in_frame = {
                .buffer = _enc_in_buf,
                .len = (uint32_t)_enc_in_size,
            };
            esp_audio_enc_out_frame_t out_frame = {
                .buffer = _enc_out_buf,
                .len = (uint32_t)_enc_out_size,
            };
            esp_audio_err_t ret = esp_aac_enc_process(_encoder, &in_frame, &out_frame);
            if (ret == ESP_AUDIO_ERR_OK && out_frame.encoded_bytes > 0) {
                fwrite(_enc_out_buf, 1, out_frame.encoded_bytes, file);
            }
        }
        esp_aac_enc_close(_encoder);
        _encoder = nullptr;
    }

    /* Close file */
    if (file) {
        fclose(file);
    }

    /* Free PCM buffer */
    if (_pcm_buffer) {
        heap_caps_free(_pcm_buffer);
        _pcm_buffer = nullptr;
    }

    /* Free encoder I/O buffers */
    if (_enc_in_buf) {
        heap_caps_free(_enc_in_buf);
        _enc_in_buf = nullptr;
    }
    if (_enc_out_buf) {
        heap_caps_free(_enc_out_buf);
        _enc_out_buf = nullptr;
    }
    _enc_in_size = 0;
    _enc_out_size = 0;

    /* Publish recording_state.active=false for cross-module notification (e.g. Music app) */
    orb_advert_t pub = _rec_pub.load(std::memory_order_acquire);
    if (pub >= 0) {
        struct recording_state_s rs = {};
        rs.timestamp = esp_timer_get_time();
        rs.active = false;
        rs.bytes_written = _record_bytes_written;
        rs.elapsed_ms = (uint32_t)((esp_timer_get_time() / 1000 - _record_start_ms));
        orb_publish(ORB_ID(recording_state), pub, &rs);
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
 * Scan SD card for AAC recordings
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

    /* Ensure SD card is mounted */
    if (!PeripheralManager::instance().init_sdcard()) {
        ESP_LOGW(TAG, "SD card not available, cannot scan recordings");
        if (_label_no_recs) lv_obj_remove_flag(_label_no_recs, LV_OBJ_FLAG_HIDDEN);
        if (_list_recordings) lv_obj_add_flag(_list_recordings, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    struct dirent *entry;
    DIR *dir = opendir(SDMMC_MOUNT_POINT);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s (SD card not mounted?)", SDMMC_MOUNT_POINT);
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
        if (strcasecmp(ext, ".aac") != 0) continue;
        _recording_names[count] = strdup(name);
        if (!_recording_names[count]) continue;  // OOM, skip this file
        count++;
    }
    closedir(dir);
    _recording_count = count;

    ESP_LOGI(TAG, "Found %d recording(s) in %s", _recording_count, SDMMC_MOUNT_POINT);

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
