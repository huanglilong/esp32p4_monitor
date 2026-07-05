#pragma once

/*
 * CameraDriver — manages camera hardware mutual exclusion via uORB.
 *
 * Provides camera_available() check and camera claim/release API.
 * CameraStream and PhoneAppCamera use CameraDriver to coordinate
 * exclusive access to the MIPI CSI hardware.
 *
 * Uses uORB camera_state topic for cross-module notification.
 * All public methods are thread-safe (protected by _mutex).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "uorb.h"
#include "topics.h"

class CameraDriver {
public:
    static CameraDriver& instance(void);

    /** Check if camera hardware is available (not claimed by another module).
     *  Uses uORB camera_state topic for cross-module check.
     *  Thread-safe. */
    bool available(void) const;

    /** Claim camera hardware. Publishes camera_state.running=true.
     *  @return true if successfully claimed, false if already in use
     *  Thread-safe: available() check and claim are atomic (no TOCTOU gap). */
    bool claim(void);

    /** Release camera hardware. Publishes camera_state.running=false.
     *  Thread-safe. */
    void release(void);

    /* Delete copy/move */
    CameraDriver(const CameraDriver&) = delete;
    CameraDriver& operator=(const CameraDriver&) = delete;

private:
    CameraDriver();
    ~CameraDriver();

    /** Internal availability check (caller must hold _mutex). */
    bool _available_locked(void) const;

    orb_advert_t    _pub;       /* uORB publisher handle */
    mutable orb_sub_t _sub;     /* uORB subscriber handle (lazy-init in const available check) */
    bool            _claimed;   /* Local claim state for fast-path check */
    SemaphoreHandle_t _mutex;   /* Protects _claimed, _sub, _pub lazy-init */
};
