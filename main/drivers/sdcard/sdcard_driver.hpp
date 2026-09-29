#pragma once

#include <atomic>

/*
 * SDCardDriver — manages SD card lifecycle via SDSPI.
 *
 * Init-once, never deinit. SD card is mounted at boot and stays mounted.
 * Uses ESP-IDF standard sd_pwr_ctrl API for LDO4 power management.
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

    /** Initialize SD card (idempotent). Thread-safe.
     *  SDSPI init with LDO4 power-on.
     *  @return true on success (or already initialized) */
    bool init(void);

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
    sd_pwr_ctrl_handle_t        _pwr_ctrl;
};
