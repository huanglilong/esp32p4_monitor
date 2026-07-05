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
 *
 * Claim ownership:
 *   Each claim() caller provides a caller_id (e.g., module name).
 *   Re-claiming with the same caller_id succeeds (reentrant).
 *   Claiming with a different caller_id fails (mutual exclusion).
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
     *  @param caller_id  Identifier for the claiming module (e.g., "stream", "camera_app").
     *                    Re-claiming with the same caller_id succeeds (reentrant).
     *                    Claiming with a different caller_id fails (mutual exclusion).
     *  @return true if successfully claimed, false if already in use by another module
     *  Thread-safe: available() check and claim are atomic (no TOCTOU gap). */
    bool claim(const char *caller_id = "unknown");

    /** Release camera hardware. Publishes camera_state.running=false.
     *  @param caller_id  Must match the caller_id used in claim().
     *                    Mismatched caller_id is ignored (defensive).
     *  Thread-safe. */
    void release(const char *caller_id = "unknown");

    /** @return true if camera is currently claimed by any module */
    bool isClaimed(void) const;

    /** @return the caller_id of the current claimer, or nullptr if unclaimed */
    const char* claimOwner(void) const;

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
    const char     *_owner_id;  /* Who claimed the camera (for reentrant detection) */
    SemaphoreHandle_t _mutex;   /* Protects _claimed, _owner_id, _sub, _pub lazy-init */
};
