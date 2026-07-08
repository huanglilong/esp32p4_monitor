#pragma once

#include <atomic>

/*
 * AudioDriver — manages audio I2S + codec lifecycle.
 *
 * Reference-counted init/deinit, thread-safe codec operations.
 * Publishes volume_state via uORB when volume changes.
 *
 * Extracted from PeripheralManager for independent module ownership.
 */

#include "esp_codec_dev.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "uorb.h"

class AudioDriver {
public:
    static AudioDriver& instance(void);

    /** Initialize audio I2S + codec (refcounted). Thread-safe.
     *  Configures ES8311+ES7210 (LCD-4B) or ES8311 single-chip (WIFI6). */
    void init(void);

    /** Deinitialize audio (refcounted). Thread-safe.
     *  Only tears down at refcount=0. */
    void deinit(void);

    /** @return true if audio is currently initialized */
    bool available(void) const { return _refcount.load(std::memory_order_relaxed) > 0; }

    /* ---- Audio handles (read-only access) ---- */
    i2s_chan_handle_t rx_handle(void) const { return _rx_handle; }
    i2s_chan_handle_t tx_handle(void) const { return _tx_handle; }
    esp_codec_dev_handle_t codec_handle(void) const { return _codec_handle.load(std::memory_order_relaxed); }

    /* ---- Thread-safe codec operations ---- */
    /** Set speaker output volume (0-100). Publishes volume_state via uORB. */
    void set_volume(int volume);

    /** Get current volume (cached). */
    int volume(void) const { return _volume; }

    /** Set microphone input gain. Thread-safe. */
    void set_mic_gain(int gain_db);

    /** Write PCM data to codec DAC. Thread-safe.
     *  @return data_size on success, -1 on failure */
    int codec_write(const uint8_t *data, int size);

    /** Set board type (LCD-4B vs WIFI6). Must be called before init(). */
    void set_has_lcd(bool v) { _has_lcd.store(v, std::memory_order_release); }

    /* Delete copy/move */
    AudioDriver(const AudioDriver&) = delete;
    AudioDriver& operator=(const AudioDriver&) = delete;

private:
    AudioDriver();
    ~AudioDriver();

    SemaphoreHandle_t       _lifecycle_mutex;
    std::atomic<SemaphoreHandle_t> _codec_mutex;  /* atomic: TOCTOU-safe reads in codec ops */

    std::atomic<bool>       _has_lcd;        /* atomic: set before init, read from init path */
    std::atomic<int>        _refcount;       /* atomic: available() reads without lock */
    std::atomic<int>        _volume;

    std::atomic<esp_codec_dev_handle_t> _codec_handle;      /* atomic: TOCTOU-safe reads */
    std::atomic<esp_codec_dev_handle_t> _codec_mic_handle;  /* atomic: TOCTOU-safe reads */
    i2s_chan_handle_t        _rx_handle;
    i2s_chan_handle_t        _tx_handle;

    std::atomic<orb_advert_t>   _vol_pub;    /* uORB volume_state publisher handle */
    std::atomic<int>            _codec_ops_in_flight;  /* Atomic: tracks in-flight codec ops for safe deinit */
};
