/*
 * uORB topic umbrella header for ESP32-P4 Monitor.
 *
 * Struct definitions and ORB_TOPIC_DECLARE() are auto-generated
 * from proto/.msg files by tools/msg_gen.py.
 *
 * To regenerate: run idf.py uorb_topics
 */

#pragma once

// Include all generated per-topic headers
#include "generated/fps_stats.h"
#include "generated/wifi_state.h"
#include "generated/audio_level.h"
#include "generated/camera_state.h"
#include "generated/recording_state.h"
#include "generated/volume_state.h"
#include "generated/ulog_state.h"
#include "generated/system_stats.h"
#include "generated/system_alert.h"
#include "generated/camera_frame.h"

#include "uorb.h"

/* NOTE: No extern "C" wrapper here! The ORB_TOPIC_DECLARE/ORB_TOPIC_DEFINE
 * macros expand to C++ variable declarations (const orb_metadata_t).
 * The generated .cpp files (main/generated/) define these variables without
 * extern "C" linkage, so the DECLARE must match — no extern "C" linkage. */

// Convenience: ORB_TOPIC_DECLARE for the primary topic names
// (generated .cpp files already contain ORB_TOPIC_DEFINE)
ORB_TOPIC_DECLARE(fps_stats);
ORB_TOPIC_DECLARE(wifi_state);
ORB_TOPIC_DECLARE(audio_level);
ORB_TOPIC_DECLARE(camera_state);
ORB_TOPIC_DECLARE(recording_state);
ORB_TOPIC_DECLARE(volume_state);
ORB_TOPIC_DECLARE(ulog_state);
ORB_TOPIC_DECLARE(system_stats);
ORB_TOPIC_DECLARE(system_alert);
ORB_TOPIC_DECLARE(camera_frame);
