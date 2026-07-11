/*
 * CameraDriver — manages camera hardware mutual exclusion via uORB.
 *
 * Provides claim/release for camera hardware, with uORB camera_state
 * topic for cross-module coordination.
 * Thread-safe: all public methods are protected by _mutex.
 *
 * Claim ownership:
 *   Each claim() caller provides a caller_id (e.g., "stream", "camera_app").
 *   Re-claiming with the same caller_id succeeds (reentrant).
 *   Claiming with a different caller_id fails (mutual exclusion).
 */

#include "camera_driver.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstring>

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
    _owner_id(nullptr),
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

bool CameraDriver::claim(const char *caller_id)
{
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (_claimed) {
        if (_owner_id && caller_id && strcmp(_owner_id, caller_id) == 0) {
            /* Re-entrant claim by same owner — success */
            xSemaphoreGive(_mutex);
            ESP_LOGW(TAG, "Camera already claimed by %s (re-entrant)", caller_id);
            return true;
        }
        /* Claimed by a different module — fail */
        xSemaphoreGive(_mutex);
        ESP_LOGW(TAG, "Camera hardware in use by %s, cannot claim for %s",
                 _owner_id ? _owner_id : "unknown", caller_id ? caller_id : "unknown");
        return false;
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
    _owner_id = caller_id;
    xSemaphoreGive(_mutex);
    ESP_LOGI(TAG, "Camera hardware claimed by %s", caller_id ? caller_id : "unknown");
    return true;
}

void CameraDriver::release(const char *caller_id)
{
    if (!_mutex) return;
    xSemaphoreTake(_mutex, portMAX_DELAY);

    if (!_claimed) {
        xSemaphoreGive(_mutex);
        return;
    }

    /* Defensive: ignore release from non-owner (null caller_id is also a non-match) */
    if (!caller_id || !_owner_id || strcmp(_owner_id, caller_id) != 0) {
        ESP_LOGW(TAG, "Camera release ignored: caller %s is not owner %s",
                 caller_id, _owner_id);
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

    ESP_LOGI(TAG, "Camera hardware released by %s", _owner_id ? _owner_id : "unknown");
    _claimed = false;
    _owner_id = nullptr;
    xSemaphoreGive(_mutex);
}

bool CameraDriver::isClaimed(void) const
{
    if (!_mutex) return false;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool claimed = _claimed;
    xSemaphoreGive(_mutex);
    return claimed;
}

const char* CameraDriver::claimOwner(void) const
{
    if (!_mutex) return nullptr;
    xSemaphoreTake(_mutex, portMAX_DELAY);
    const char *owner = _owner_id;
    xSemaphoreGive(_mutex);
    return owner;
}
