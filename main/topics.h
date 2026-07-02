/*
 * uORB topic umbrella header for ESP32-P4 Monitor.
 *
 * Struct definitions and ORB_TOPIC_DECLARE() are auto-generated
 * from proto/*.msg by tools/msg_gen.py.
 *
 * To regenerate:  make uorb_topics_generate
 */

#pragma once

// Include all generated per-topic headers
#include "generated/fps_stats.h"
#include "generated/detection_result.h"
#include "generated/wifi_state.h"
#include "generated/audio_level.h"
#include "generated/camera_state.h"
#include "generated/recording_state.h"
#include "generated/volume_state.h"

#include "uorb.h"

#ifdef __cplusplus
extern "C" {
#endif

// Convenience: ORB_TOPIC_DECLARE for the primary topic names
// (generated .cpp files already contain ORB_TOPIC_DEFINE)
ORB_TOPIC_DECLARE(fps_stats);
ORB_TOPIC_DECLARE(detection_result);
ORB_TOPIC_DECLARE(wifi_state);
ORB_TOPIC_DECLARE(audio_level);
ORB_TOPIC_DECLARE(camera_state);
ORB_TOPIC_DECLARE(recording_state);
ORB_TOPIC_DECLARE(volume_state);

#ifdef __cplusplus
}
