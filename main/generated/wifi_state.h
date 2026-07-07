/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#ifndef UORB_TOPIC_WIFI_STATE_H_
#define UORB_TOPIC_WIFI_STATE_H_

#include <cstdint>
#include <cstddef>

#define ORB_QUEUE_LENGTH_WIFI_STATE 1

#define WIFI_STATE_FORMAT_STR "wifi_state:uint64_t timestamp;bool connected;int8_t rssi;bool scanning;char[32] ssid;uint8_t[5] _padding0;"

// NOLINTNEXTLINE(modernize-use-using)
typedef struct wifi_state_s
{
    uint64_t                 timestamp;  ///< @brief
    bool                     connected;  ///< @brief
    bool                     scanning;  ///< @brief
    int8_t                   rssi;  ///< @brief
    char                     ssid[32];  ///< @brief
} wifi_state_s;

#define WIFI_STATE_SIZE sizeof(wifi_state_s)

// NOLINTNEXTLINE
static constexpr size_t wifi_state_SIZE_CONST { WIFI_STATE_SIZE };

#endif /* UORB_TOPIC_WIFI_STATE_H_ */
