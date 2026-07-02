/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#ifndef UORB_TOPIC_FPS_STATS_H_
#define UORB_TOPIC_FPS_STATS_H_

#include <cstdint>
#include <cstddef>

#define ORB_QUEUE_LENGTH_FPS_STATS 3

// NOLINTNEXTLINE(modernize-use-using)
typedef struct fps_stats_s
{
    uint64_t                 timestamp;  ///< @brief
    uint32_t                 frame_count;  ///< @brief
    uint32_t                 fps_total_bytes;  ///< @brief
    float                    fps;  ///< @brief
} fps_stats_s;

#define FPS_STATS_SIZE sizeof(fps_stats_s)

// NOLINTNEXTLINE
static constexpr size_t fps_stats_SIZE_CONST { FPS_STATS_SIZE };

#endif /* UORB_TOPIC_FPS_STATS_H_ */
