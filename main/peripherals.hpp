#pragma once

/*
 * PeripheralManager — unified facade for shared peripheral lifecycle.
 *
 * Replaces scattered extern globals and init/deinit functions that were
 * previously defined in main.cpp. All peripheral access (audio codec,
 * I2S handles, SD card) goes through this singleton.
 *
 * Design goals:
 *   - Single source of truth for peripheral handles
 *   - Reference-counted init/deinit (safe shared usage)
 *   - Thread-safe codec access via mutex
 *   - Eliminates all extern declarations from app modules
 *
 * Usage:
 *   PeripheralManager &pm = PeripheralManager::instance();
 *   pm.init_audio();          // refcounted
 *   pm.set_volume(80);        // thread-safe
 *   pm.deinit_audio();        // refcounted, only tears down at 0
 */

#include "esp_codec_dev.h"
#include "driver/i2s_std.h"
#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

class PeripheralManager {
public:
    /** Singleton access */
    static PeripheralManager& instance(void);

    /* ---- Board detection ---- */
    bool has_lcd(void) const { return _has_lcd; }
    void set_has_lcd(bool v) { _has_lcd = v; }

    /* ---- SD Card ---- */
    /** Initialize SD card (refcounted). Safe to call multiple times.
     *  @return true on success (or already initialized) */
    bool init_sdcard(void);

    /** Deinitialize SD card (refcounted). Only unmounts at refcount=0. */
    void deinit_sdcard(void);

    /** @return true if SD card is currently mounted */
    bool sdcard_available(void) const { return _sdcard_refcount > 0; }

    /* ---- Audio ---- */
    /** Initialize audio I2S + codec (refcounted). Safe to call multiple times.
     *  Configures ES8311+ES7210 (LCD-4B) or ES8311 single-chip (WIFI6). */
    void init_audio(void);

    /** Deinitialize audio (refcounted). Only tears down at refcount=0. */
    void deinit_audio(void);

    /** @return true if audio is currently initialized */
    bool audio_available(void) const { return _audio_refcount > 0; }

    /* ---- Audio handles (read-only access) ---- */
    i2s_chan_handle_t rx_handle(void) const { return _rx_handle; }
    i2s_chan_handle_t tx_handle(void) const { return _tx_handle; }
    esp_codec_dev_handle_t codec_handle(void) const { return _codec_handle; }

    /* ---- Thread-safe codec operations ---- */
    /** Set speaker output volume (0-100). Thread-safe via internal mutex. */
    void set_volume(int volume);

    /** Get current volume setting (cached, no I2C access). */
    int volume(void) const { return _volume; }

    /** Set microphone input gain. Thread-safe via internal mutex. */
    void set_mic_gain(int gain_db);

    /** Write PCM data to codec DAC. Thread-safe via internal mutex.
     *  @return data_size on success, -1 on failure */
    int codec_write(const uint8_t *data, int size);

    /* ---- Camera mutual exclusion ---- */
    /** Check if camera hardware is available (not used by another module).
     *  Uses uORB camera_state topic for cross-module check. */
    bool camera_available(void) const;

    /* Delete copy/move */
    PeripheralManager(const PeripheralManager&) = delete;
    PeripheralManager& operator=(const PeripheralManager&) = delete;

private:
    PeripheralManager();
    ~PeripheralManager() = default;

    /* Board info */
    bool _has_lcd;

    /* SD card state */
    sdmmc_card_t          *_card;
    int                    _sdcard_refcount;
    esp_ldo_channel_handle_t _sdcard_ldo_chan;

    /* Audio state */
    esp_codec_dev_handle_t _codec_handle;       // Speaker DAC
    esp_codec_dev_handle_t _codec_mic_handle;   // Microphone ADC
    i2s_chan_handle_t       _rx_handle;          // I2S RX (mic)
    i2s_chan_handle_t       _tx_handle;          // I2S TX (speaker)
    SemaphoreHandle_t       _codec_mutex;        // Protect concurrent codec access
    int                     _audio_refcount;
    int                     _volume;
};
