/*
 * Automatically generated — DO NOT EDIT.
 * Generated from .msg files by tools/msg_gen.py
 */
#include "wifi_state.h"
#include <uorb.h>

ORB_TOPIC_DEFINE(wifi_state, wifi_state_s, 1, "wifi_state:uint64_t timestamp;bool connected;bool scanning;int8_t rssi;char[32] ssid;");
