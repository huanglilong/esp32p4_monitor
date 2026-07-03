/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#ifndef UORB_TOPIC_ULOG_STATE_H_
#define UORB_TOPIC_ULOG_STATE_H_

#include <cstdint>
#include <cstddef>

#define ORB_QUEUE_LENGTH_ULOG_STATE 1

#define ULOG_STATE_FORMAT_STR "ulog_state:uint64_t timestamp;bool logging;char[128] filepath;"

// NOLINTNEXTLINE(modernize-use-using)
typedef struct ulog_state_s
{
    uint64_t                 timestamp;  ///< @brief
    bool                     logging;  ///< @brief
    char                     filepath[128];  ///< @brief
} ulog_state_s;

#define ULOG_STATE_SIZE sizeof(ulog_state_s)

// NOLINTNEXTLINE
static constexpr size_t ulog_state_SIZE_CONST { ULOG_STATE_SIZE };

#endif /* UORB_TOPIC_ULOG_STATE_H_ */
