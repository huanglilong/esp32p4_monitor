#pragma once

#include <atomic>

/*
 * SDCardDriver — manages SD card lifecycle via SDSPI.
 *
 * Init-once, never deinit. SD card is mounted at boot and stays mounted.
 * Uses ESP-IDF standard sd_pwr_ctrl API for LDO4 power management.
 * On LCD-4B boards, BSP handles SDMMC independently.
 *
 * Extracted from PeripheralManager for independent module ownership.
 */

#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

class SDCardDriver {
public:
    static SDCardDriver& instance(void);

    /** Set board type — must be called before init().
     *  On LCD-4B: does not attempt SDSPI (BSP manages SDMMC + LDO4). */
    void set_has_lcd(bool v) { _has_lcd.store(v, std::memory_order_relaxed); }

    /** Initialize SD card (idempotent). Thread-safe.
     *  On WIFI6: SDSPI init with LDO4 power-on.
     *  On LCD-4B: returns true only if BSP SDMMC already mounted.
     *  @return true on success (or already initialized) */
    bool init(void);

    /** No-op — SD card is never unmounted after init.
     *  Kept for API compatibility. */
    void deinit(void);

    /** @return true if SD card was successfully initialized */
    bool available(void) const { return _initialized; }

    /** Format the SD card FAT filesystem (erases ALL data).
     *  Card must be mounted (init() called) first.
     *  Stops ULog/text logger writers is the caller's responsibility —
     *  this method only unmounts, formats, and remounts.
     *  @return true on success */
    bool format(void);

    /* Delete copy/move */
    SDCardDriver(const SDCardDriver&) = delete;
    SDCardDriver& operator=(const SDCardDriver&) = delete;

private:
    SDCardDriver();
    ~SDCardDriver();

    SemaphoreHandle_t           _init_mutex;
    sdmmc_card_t                *_card;
    std::atomic<bool>           _initialized;
    std::atomic<bool>           _has_lcd;
    sd_pwr_ctrl_handle_t        _pwr_ctrl;
};
