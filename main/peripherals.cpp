/*
 * PeripheralManager — thin facade delegating to independent driver modules.
 *
 * All peripheral logic is now in AudioDriver, SDCardDriver, CameraDriver.
 * This facade preserves the existing API so app modules need no changes.
 */

#include "peripherals.hpp"
#include "esp_log.h"

static const char *TAG = "PeriphMgr";

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
    /* Propagate board type to AudioDriver for codec configuration */
    AudioDriver::instance().set_has_lcd(v);
}

/*============================================================================
 * SD Card — delegates to SDCardDriver
 *============================================================================*/
bool PeripheralManager::init_sdcard(void)
{
    return SDCardDriver::instance().init();
}

void PeripheralManager::deinit_sdcard(void)
{
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
