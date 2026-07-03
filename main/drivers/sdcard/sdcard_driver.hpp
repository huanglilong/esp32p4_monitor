#pragma once

/*
 * SDCardDriver — manages SD card lifecycle via SDSPI.
 *
 * Reference-counted init/deinit, LDO power-cycle for clean state.
 * Publishes sdcard_state via uORB when availability changes.
 *
 * Extracted from PeripheralManager for independent module ownership.
 */

#include "esp_ldo_regulator.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"

class SDCardDriver {
public:
    static SDCardDriver& instance(void);

    /** Initialize SD card (refcounted). Thread-safe.
     *  @return true on success (or already initialized) */
    bool init(void);

    /** Deinitialize SD card (refcounted). Thread-safe.
     *  Only unmounts at refcount=0. */
    void deinit(void);

    /** @return true if SD card is currently mounted */
    bool available(void) const { return _refcount > 0; }

    /* Delete copy/move */
    SDCardDriver(const SDCardDriver&) = delete;
    SDCardDriver& operator=(const SDCardDriver&) = delete;

private:
    SDCardDriver();
    ~SDCardDriver();

    SemaphoreHandle_t           _lifecycle_mutex;
    sdmmc_card_t                *_card;
    int                         _refcount;
    esp_ldo_channel_handle_t    _ldo_chan;
};
