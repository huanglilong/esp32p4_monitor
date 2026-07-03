/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#ifndef UORB_TOPIC_CAMERA_STATE_H_
#define UORB_TOPIC_CAMERA_STATE_H_

#include <cstdint>
#include <cstddef>

#define ORB_QUEUE_LENGTH_CAMERA_STATE 1

#define CAMERA_STATE_FORMAT_STR "camera_state:uint64_t timestamp;bool running;"

// NOLINTNEXTLINE(modernize-use-using)
typedef struct camera_state_s
{
    uint64_t                 timestamp;  ///< @brief
    bool                     running;  ///< @brief
} camera_state_s;

#define CAMERA_STATE_SIZE sizeof(camera_state_s)

// NOLINTNEXTLINE
static constexpr size_t camera_state_SIZE_CONST { CAMERA_STATE_SIZE };

#endif /* UORB_TOPIC_CAMERA_STATE_H_ */
