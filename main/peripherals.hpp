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
 */

#include "audio_driver.hpp"
#include "sdcard_driver.hpp"
#include "camera_driver.hpp"

class PeripheralManager {
public:
    /** Singleton access */
    static PeripheralManager& instance(void);

    /* ---- Board detection ---- */
    void set_has_lcd(bool v);

    /* ---- SD Card (delegates to SDCardDriver, init-once) ---- */
    bool init_sdcard(void);
    bool sdcard_available(void) const { return SDCardDriver::instance().available(); }

    /* ---- Audio (delegates to AudioDriver) ---- */
    void init_audio(void);
    void deinit_audio(void);

    /* ---- Audio handles (read-only, from AudioDriver) ---- */
    i2s_chan_handle_t rx_handle(void) const { return AudioDriver::instance().rx_handle(); }

    /* ---- Thread-safe codec operations (delegates to AudioDriver) ---- */
    void set_volume(int volume) { AudioDriver::instance().set_volume(volume); }
    int codec_write(const uint8_t *data, int size) { return AudioDriver::instance().codec_write(data, size); }

    /* Delete copy/move */
    PeripheralManager(const PeripheralManager&) = delete;
    PeripheralManager& operator=(const PeripheralManager&) = delete;

private:
    PeripheralManager();
    ~PeripheralManager() = default;
};
