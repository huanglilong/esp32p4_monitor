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

PeripheralManager::PeripheralManager() :
    _has_lcd(false)
{
}

/*============================================================================
 * Board detection — propagates to drivers that need it
 *============================================================================*/
void PeripheralManager::set_has_lcd(bool v)
{
    _has_lcd = v;
    /* Propagate board type to drivers */
    AudioDriver::instance().set_has_lcd(v);
    SDCardDriver::instance().set_has_lcd(v);
}

/*============================================================================
 * SD Card — delegates to SDCardDriver (init-once, never unmount)
 *============================================================================*/
bool PeripheralManager::init_sdcard(void)
{
    return SDCardDriver::instance().init();
}

void PeripheralManager::deinit_sdcard(void)
{
    /* No-op: SD card stays mounted permanently after boot */
    SDCardDriver::instance().deinit();
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
