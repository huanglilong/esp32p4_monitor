/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#ifndef UORB_TOPIC_CAMERA_FRAME_CHUNK_H_
#define UORB_TOPIC_CAMERA_FRAME_CHUNK_H_

#include <cstdint>
#include <cstddef>

#define ORB_QUEUE_LENGTH_CAMERA_FRAME_CHUNK 32

#define CAMERA_FRAME_CHUNK_FORMAT_STR "camera_frame_chunk:uint64_t timestamp;uint32_t frame_index;uint16_t chunk_index;uint16_t chunks_total;uint16_t chunk_size;uint16_t width;uint16_t height;uint8_t format;uint8_t[1024] chunk_data;uint8_t[1] _padding0;"

// NOLINTNEXTLINE(modernize-use-using)
typedef struct camera_frame_chunk_s
{
    uint64_t                 timestamp;  ///< @brief
    uint32_t                 frame_index;  ///< @brief
    uint16_t                 chunk_index;  ///< @brief
    uint16_t                 chunks_total;  ///< @brief
    uint16_t                 chunk_size;  ///< @brief
    uint16_t                 width;  ///< @brief
    uint16_t                 height;  ///< @brief
    uint8_t                  format;  ///< @brief
    uint8_t                  chunk_data[1024];  ///< @brief
} camera_frame_chunk_s;

#define CAMERA_FRAME_CHUNK_SIZE sizeof(camera_frame_chunk_s)

// NOLINTNEXTLINE
static constexpr size_t camera_frame_chunk_SIZE_CONST { CAMERA_FRAME_CHUNK_SIZE };

/** Size without trailing _padding (for ULog writer). Matches PX4 o_size_no_padding. */
#define CAMERA_FRAME_CHUNK_SIZE_NO_PADDING (sizeof(camera_frame_chunk_s) - 1)

// NOLINTNEXTLINE
static constexpr size_t camera_frame_chunk_SIZE_NO_PADDING_CONST { CAMERA_FRAME_CHUNK_SIZE_NO_PADDING };

#endif /* UORB_TOPIC_CAMERA_FRAME_CHUNK_H_ */
