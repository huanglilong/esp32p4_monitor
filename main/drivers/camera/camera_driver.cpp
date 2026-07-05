/*
 * CameraDriver — manages camera hardware mutual exclusion via uORB.
 *
 * Provides claim/release for camera hardware, with uORB camera_state
 * topic for cross-module coordination.
 * Thread-safe: all public methods are protected by _mutex.
 */

#include "camera_driver.hpp"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "CameraDriver";

/*============================================================================
 * Singleton
 *============================================================================*/
CameraDriver& CameraDriver::instance(void)
{
    static CameraDriver s;
    return s;
}

CameraDriver::CameraDriver() :
    _pub(ORB_ADVERT_INVALID),
    _sub(ORB_ADVERT_INVALID),
    _claimed(false),
    _mutex(nullptr)
{
    _mutex = xSemaphoreCreateMutex();
}

CameraDriver::~CameraDriver()
{
    if (_sub >= 0) {
        orb_unsubscribe(_sub);
        _sub = ORB_ADVERT_INVALID;
    }
    if (_mutex) {
        vSemaphoreDelete(_mutex);
        _mutex = nullptr;
    }
}

/*============================================================================
 * Camera availability and claim/release
 *============================================================================*/
bool CameraDriver::available(void) const
{
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    bool avail = _available_locked();

    xSemaphoreGive(_mutex);
    return avail;
}

bool CameraDriver::_available_locked(void) const
{
    /* If we've claimed it locally, it's not available */
    if (_claimed) return false;

    /* Lazy-subscribe on first call */
    if (_sub < 0) {
        _sub = orb_subscribe(ORB_ID(camera_state));
    }
    if (_sub >= 0) {
        bool updated = false;
        if (orb_check(_sub, &updated) == 0 && updated) {
            struct camera_state_s cs = {};
            orb_copy(ORB_ID(camera_state), _sub, &cs);
            if (cs.running) return false;
        }
    }
    return true;
}

bool CameraDriver::claim(void)
{
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_claimed) {
        xSemaphoreGive(_mutex);
        ESP_LOGW(TAG, "Camera already claimed by this module");
        return true;
    }

    /* Check availability and claim atomically (no TOCTOU gap) */
    if (!_available_locked()) {
        xSemaphoreGive(_mutex);
        ESP_LOGW(TAG, "Camera hardware in use by another module");
        return false;
    }

    /* Publish camera_state.running = true */
    if (_pub < 0) {
        _pub = orb_advertise(ORB_ID(camera_state));
    }
    if (_pub >= 0) {
        struct camera_state_s cs = {};
        cs.timestamp = esp_timer_get_time();
        cs.running = true;
        orb_publish(ORB_ID(camera_state), _pub, &cs);
    }

    _claimed = true;
    xSemaphoreGive(_mutex);
    ESP_LOGI(TAG, "Camera hardware claimed");
    return true;
}

void CameraDriver::release(void)
{
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (!_claimed) {
        xSemaphoreGive(_mutex);
        return;
    }

    /* Publish camera_state.running = false */
    if (_pub < 0) {
        _pub = orb_advertise(ORB_ID(camera_state));
    }
    if (_pub >= 0) {
        struct camera_state_s cs = {};
        cs.timestamp = esp_timer_get_time();
        cs.running = false;
        orb_publish(ORB_ID(camera_state), _pub, &cs);
    }

    _claimed = false;
    xSemaphoreGive(_mutex);
    ESP_LOGI(TAG, "Camera hardware released");
}
