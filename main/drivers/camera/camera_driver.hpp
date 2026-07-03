#pragma once

/*
 * CameraDriver — manages camera hardware mutual exclusion via uORB.
 *
 * Provides camera_available() check and camera claim/release API.
 * CameraStream and PhoneAppCamera use CameraDriver to coordinate
 * exclusive access to the MIPI CSI hardware.
 *
 * Uses uORB camera_state topic for cross-module notification.
 */

#include "freertos/FreeRTOS.h"
#include "uorb.h"
#include "topics.h"

class CameraDriver {
public:
    static CameraDriver& instance(void);

    /** Check if camera hardware is available (not claimed by another module).
     *  Uses uORB camera_state topic for cross-module check. */
    bool available(void) const;

    /** Claim camera hardware. Publishes camera_state.running=true.
     *  @return true if successfully claimed, false if already in use */
    bool claim(void);

    /** Release camera hardware. Publishes camera_state.running=false. */
    void release(void);

    /* Delete copy/move */
    CameraDriver(const CameraDriver&) = delete;
    CameraDriver& operator=(const CameraDriver&) = delete;

private:
    CameraDriver();
    ~CameraDriver();

    orb_advert_t    _pub;       /* uORB publisher handle */
    orb_sub_t       _sub;       /* uORB subscriber handle (for available check) */
    bool            _claimed;   /* Local claim state for fast-path check */
};
