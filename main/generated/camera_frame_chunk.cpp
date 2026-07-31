/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#include "camera_frame_chunk.h"
#include <uorb.h>

ORB_TOPIC_DEFINE(camera_frame_chunk, camera_frame_chunk_s, 32, "camera_frame_chunk:uint64_t timestamp;uint32_t frame_index;uint16_t chunk_index;uint16_t chunks_total;uint16_t chunk_size;uint16_t width;uint16_t height;uint8_t format;uint8_t[1024] chunk_data;uint8_t[1] _padding0;", 1);
