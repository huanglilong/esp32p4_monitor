/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#pragma once

#include <cstdint>
#include "uorb.h"

static constexpr size_t ORB_TOPICS_COUNT { 8 };

enum class ORB_ID : uint8_t {
    audio_level = 0,
    camera_state = 1,
    detection_result = 2,
    fps_stats = 3,
    recording_state = 4,
    ulog_state = 5,
    volume_state = 6,
    wifi_state = 7,
    INVALID
};

extern const struct orb_metadata *const *orb_get_topics();
extern const struct orb_metadata *get_orb_meta(ORB_ID id);
