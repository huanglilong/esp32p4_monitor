/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#ifndef UORB_TOPIC_RECORDING_STATE_H_
#define UORB_TOPIC_RECORDING_STATE_H_

#include <cstdint>
#include <cstddef>

#define ORB_QUEUE_LENGTH_RECORDING_STATE 1

#define RECORDING_STATE_FORMAT_STR "recording_state:uint64_t timestamp;uint32_t bytes_written;uint32_t elapsed_ms;bool active;uint8_t[7] _padding0;"

// NOLINTNEXTLINE(modernize-use-using)
typedef struct recording_state_s
{
    uint64_t                 timestamp;  ///< @brief
    bool                     active;  ///< @brief
    uint32_t                 bytes_written;  ///< @brief
    uint32_t                 elapsed_ms;  ///< @brief
} recording_state_s;

#define RECORDING_STATE_SIZE sizeof(recording_state_s)

// NOLINTNEXTLINE
static constexpr size_t recording_state_SIZE_CONST { RECORDING_STATE_SIZE };

#endif /* UORB_TOPIC_RECORDING_STATE_H_ */
