/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#ifndef UORB_TOPIC_DETECTION_RESULT_H_
#define UORB_TOPIC_DETECTION_RESULT_H_

#include <cstdint>
#include <cstddef>

#define ORB_QUEUE_LENGTH_DETECTION_RESULT 1

#define DETECTION_RESULT_FORMAT_STR "detection_result:uint64_t timestamp;int32_t person_count;uint8_t[4] _padding0;"

// NOLINTNEXTLINE(modernize-use-using)
typedef struct detection_result_s
{
    uint64_t                 timestamp;  ///< @brief
    int32_t                  person_count;  ///< @brief
} detection_result_s;

#define DETECTION_RESULT_SIZE sizeof(detection_result_s)

// NOLINTNEXTLINE
static constexpr size_t detection_result_SIZE_CONST { DETECTION_RESULT_SIZE };

/** Size without trailing _padding (for ULog writer). Matches PX4 o_size_no_padding. */
#define DETECTION_RESULT_SIZE_NO_PADDING (sizeof(detection_result_s) - 4)

// NOLINTNEXTLINE
static constexpr size_t detection_result_SIZE_NO_PADDING_CONST { DETECTION_RESULT_SIZE_NO_PADDING };

#endif /* UORB_TOPIC_DETECTION_RESULT_H_ */
