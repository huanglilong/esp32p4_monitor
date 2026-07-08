#pragma once

/*
 * PeripheralManager — thin facade delegating to independent driver modules.
 *
 * Architecture:
 *   PeripheralManager (facade)
 *     ├── AudioDriver      — I2S + codec lifecycle, volume, uORB
 *     ├── SDCardDriver     — SD init-once (mounts at boot, never unmounts)
 *     └── CameraDriver     — camera_state pub/sub, claim/release
 *
 * This facade preserves the existing API so app modules need no changes.
 * Each driver is an independent singleton that can be used directly
 * if finer-grained access is needed.
 *
 * Usage:
 *   PeripheralManager &pm = PeripheralManager::instance();
 *   pm.init_audio();          // delegates to AudioDriver
 *   pm.set_volume(80);        // delegates to AudioDriver
 *   pm.camera_available();    // delegates to CameraDriver
 */

#include "audio_driver.hpp"
#include "sdcard_driver.hpp"
#include "camera_driver.hpp"

class PeripheralManager {
public:
    /** Singleton access */
    static PeripheralManager& instance(void);

    /* ---- Board detection ---- */
    bool has_lcd(void) const { return _has_lcd.load(std::memory_order_acquire); }
    void set_has_lcd(bool v);

    /* ---- SD Card (delegates to SDCardDriver, init-once) ---- */
    bool init_sdcard(void);
    void deinit_sdcard(void);  /* no-op, SD stays mounted */
    bool sdcard_available(void) const { return SDCardDriver::instance().available(); }

    /* ---- Audio (delegates to AudioDriver) ---- */
    void init_audio(void);
    void deinit_audio(void);
    bool audio_available(void) const { return AudioDriver::instance().available(); }

    /* ---- Audio handles (read-only, from AudioDriver) ---- */
    i2s_chan_handle_t rx_handle(void) const { return AudioDriver::instance().rx_handle(); }
    i2s_chan_handle_t tx_handle(void) const { return AudioDriver::instance().tx_handle(); }
    esp_codec_dev_handle_t codec_handle(void) const { return AudioDriver::instance().codec_handle(); }

    /* ---- Thread-safe codec operations (delegates to AudioDriver) ---- */
    void set_volume(int volume) { AudioDriver::instance().set_volume(volume); }
    int volume(void) const { return AudioDriver::instance().volume(); }
    void set_mic_gain(int gain_db) { AudioDriver::instance().set_mic_gain(gain_db); }
    int codec_write(const uint8_t *data, int size) { return AudioDriver::instance().codec_write(data, size); }

    /* ---- Camera (delegates to CameraDriver) ---- */
    bool camera_available(void) const { return CameraDriver::instance().available(); }

    /* Delete copy/move */
    PeripheralManager(const PeripheralManager&) = delete;
    PeripheralManager& operator=(const PeripheralManager&) = delete;

private:
    PeripheralManager();
    ~PeripheralManager() = default;

    std::atomic<bool> _has_lcd{false};
};
