/*
 * PeripheralManager — thin facade delegating to independent driver modules.
 *
 * All peripheral logic is now in AudioDriver, SDCardDriver, CameraDriver.
 * This facade preserves the existing API so app modules need no changes.
 */

#include "peripherals.hpp"
#include "esp_log.h"

/*============================================================================
 * Singleton
 *============================================================================*/
PeripheralManager& PeripheralManager::instance(void)
{
    static PeripheralManager s;
    return s;
}

PeripheralManager::PeripheralManager()
{
}

/*============================================================================
 * SD Card — delegates to SDCardDriver (init-once, never unmount)
 *============================================================================*/
bool PeripheralManager::init_sdcard(void)
{
    return SDCardDriver::instance().init();
}

/*============================================================================
 * Audio — delegates to AudioDriver
 *============================================================================*/
void PeripheralManager::init_audio(void)
{
    AudioDriver::instance().init();
}

void PeripheralManager::deinit_audio(void)
{
    AudioDriver::instance().deinit();
}
