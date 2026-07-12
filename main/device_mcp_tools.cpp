/*
 * ESP32-P4 Monitor — Device MCP Tools
 *
 * Exposes hardware capabilities (camera, audio, system stats, volume)
 * as MCP tools that can be called by LLM agents via the MCP server.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_mcp_data.h"
#include "esp_mcp_property.h"
#include "cap_mcp_server.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

#include "peripherals.hpp"
#include "camera_driver.hpp"
#include "audio_driver.hpp"
#include "sdcard_driver.hpp"
#include "system_monitor.hpp"
#include "generated/system_stats.h"
#include "example_config.h"

static const char *TAG = "device_mcp";

/* ── Helper: build JSON string for MCP tool response ── */
static esp_mcp_value_t mcp_respond_str(const char *str)
{
    return esp_mcp_value_create_string(str ? str : "{\"error\":\"null\"}");
}

/* ── Tool: system.info ─────────────────────────────────────────── */
static esp_mcp_value_t tool_system_info(const esp_mcp_property_list_t *props)
{
    (void)props;
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "{\"has_lcd\":%s,\"sd_mounted\":%s,\"camera_available\":%s,\"audio_available\":%s}",
        g_has_lcd.load() ? "true" : "false",
        SDCardDriver::instance().available() ? "true" : "false",
        CameraDriver::instance().available() ? "true" : "false",
        AudioDriver::instance().available() ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return mcp_respond_str("{\"error\":\"buffer overflow\"}");
    }
    return esp_mcp_value_create_string(buf);
}

/* ── Tool: system.stats ────────────────────────────────────────── */
static esp_mcp_value_t tool_system_stats(const esp_mcp_property_list_t *props)
{
    (void)props;
    system_stats_s stats = SystemMonitor::instance().get_latest();
    char buf[768];
    int n = snprintf(buf, sizeof(buf),
        "{\"core0_cpu_pct\":%" PRIu32 ",\"core1_cpu_pct\":%" PRIu32 ","
        "\"free_internal_heap\":%" PRIu32 ",\"min_internal_heap\":%" PRIu32 ","
        "\"free_psram\":%" PRIu32 ",\"min_psram\":%" PRIu32 ","
        "\"task_count\":%" PRIu32 "}",
        stats.core0_cpu_pct, stats.core1_cpu_pct,
        stats.free_internal, stats.min_free_internal,
        stats.free_psram, stats.min_free_psram,
        stats.task_count);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return mcp_respond_str("{\"error\":\"buffer overflow\"}");
    }
    return esp_mcp_value_create_string(buf);
}

/* ── Tool: volume.get ──────────────────────────────────────────── */
static esp_mcp_value_t tool_volume_get(const esp_mcp_property_list_t *props)
{
    (void)props;
    if (!AudioDriver::instance().available()) {
        return mcp_respond_str("{\"error\":\"audio not available\"}");
    }
    int vol = AudioDriver::instance().volume();
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"volume\":%d}", vol);
    return esp_mcp_value_create_string(buf);
}

/* ── Tool: volume.set ──────────────────────────────────────────── */
static esp_mcp_value_t tool_volume_set(const esp_mcp_property_list_t *props)
{
    const char *vol_str = esp_mcp_property_list_get_property_string(props, "volume");
    if (!vol_str || !vol_str[0]) {
        return mcp_respond_str("{\"error\":\"volume parameter required\"}");
    }
    int vol = atoi(vol_str);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;

    if (!AudioDriver::instance().available()) {
        return mcp_respond_str("{\"error\":\"audio not available\"}");
    }
    AudioDriver::instance().set_volume(vol);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"volume\":%d}", vol);
    return esp_mcp_value_create_string(buf);
}

/* ── Tool: camera.status ───────────────────────────────────────── */
static esp_mcp_value_t tool_camera_status(const esp_mcp_property_list_t *props)
{
    (void)props;
    bool cam_avail = CameraDriver::instance().available();
    const char *owner = CameraDriver::instance().claimOwner();
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"camera_available\":%s,\"owner\":\"%s\"}",
        cam_avail ? "true" : "false",
        owner ? owner : "none");
    return esp_mcp_value_create_string(buf);
}

/* ── Tool: camera.stream_start ─────────────────────────────────── */
static esp_mcp_value_t tool_camera_stream_start(const esp_mcp_property_list_t *props)
{
    (void)props;
    if (!CameraDriver::instance().available()) {
        return mcp_respond_str("{\"error\":\"camera not available\"}");
    }
    // CameraStream start is triggered via web_config_server or Settings App
    // For MCP tool, we return the stream URL for the caller to connect
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"message\":\"Camera stream must be enabled via Settings or Web UI.\","
        "\"stream_url\":\"http://esp-web.local:81/stream\","
        "\"info_url\":\"http://esp-web.local:80\"}");
    return esp_mcp_value_create_string(buf);
}

/* ── Tool: sdcard.status ───────────────────────────────────────── */
static esp_mcp_value_t tool_sdcard_status(const esp_mcp_property_list_t *props)
{
    (void)props;
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"mounted\":%s}",
        SDCardDriver::instance().available() ? "true" : "false");
    return esp_mcp_value_create_string(buf);
}

/* ── Tool: device.restart ──────────────────────────────────────── */
static esp_mcp_value_t tool_device_restart(const esp_mcp_property_list_t *props)
{
    (void)props;
    ESP_LOGW(TAG, "Device restart requested via MCP tool");
    esp_restart();
    return esp_mcp_value_create_string("{\"restarting\":true}");
}

/* ── Tool: audio.record_start ─────────────────────────────────── */
static esp_mcp_value_t tool_audio_record_start(const esp_mcp_property_list_t *props)
{
    (void)props;
    /* Recording is initiated via web_config_server API.
     * This tool provides the API endpoint for programmatic access. */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "message",
        "Use POST /api/audio/record/start?filename=<filename> on port 8080 to start recording.");
    cJSON_AddStringToObject(root, "endpoint", "/api/audio/record/start");
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return mcp_respond_str("{\"error\":\"JSON allocation failed\"}");
    esp_mcp_value_t result = esp_mcp_value_create_string(json_str);
    cJSON_free(json_str);
    return result;
}

/* ── Tool: audio.play ─────────────────────────────────────────── */
static esp_mcp_value_t tool_audio_play(const esp_mcp_property_list_t *props)
{
    (void)props;
    /* Playback is initiated via web_config_server API. */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "message",
        "Use POST /api/audio/play?file=<filename> on port 8080 to play audio.");
    cJSON_AddStringToObject(root, "endpoint", "/api/audio/play");
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return mcp_respond_str("{\"error\":\"JSON allocation failed\"}");
    esp_mcp_value_t result = esp_mcp_value_create_string(json_str);
    cJSON_free(json_str);
    return result;
}

/* ── Tool: brightness.get ─────────────────────────────────────── */
static esp_mcp_value_t tool_brightness_get(const esp_mcp_property_list_t *props)
{
    (void)props;
    /* Brightness is stored in NVS, read via shared key */
    nvs_handle_t nvs_h;
    int32_t brightness = BRIGHTNESS_DEFAULT;
    if (nvs_open(NVS_NAMESPACE_SETTINGS, NVS_READONLY, &nvs_h) == ESP_OK) {
        nvs_get_i32(nvs_h, NVS_KEY_BRIGHTNESS, &brightness);
        nvs_close(nvs_h);
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"brightness\":%" PRId32 "}", brightness);
    return esp_mcp_value_create_string(buf);
}

/* ── Tool: brightness.set ─────────────────────────────────────── */
static esp_mcp_value_t tool_brightness_set(const esp_mcp_property_list_t *props)
{
    const char *val_str = esp_mcp_property_list_get_property_string(props, "brightness");
    if (!val_str || !val_str[0]) {
        return mcp_respond_str("{\"error\":\"brightness parameter required (20-100)\"}");
    }
    int32_t val = atoi(val_str);
    if (val < BRIGHTNESS_MIN) val = BRIGHTNESS_MIN;
    if (val > BRIGHTNESS_MAX) val = BRIGHTNESS_MAX;

    nvs_handle_t nvs_h;
    if (nvs_open(NVS_NAMESPACE_SETTINGS, NVS_READWRITE, &nvs_h) == ESP_OK) {
        esp_err_t set_err = nvs_set_i32(nvs_h, NVS_KEY_BRIGHTNESS, val);
        if (set_err == ESP_OK) {
            nvs_commit(nvs_h);
        } else {
            ESP_LOGW(TAG, "brightness.set: NVS write failed: %s", esp_err_to_name(set_err));
        }
        nvs_close(nvs_h);
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"brightness\":%" PRId32 "}", val);
    return esp_mcp_value_create_string(buf);
}

/* ── Tool: wifi.status ────────────────────────────────────────── */
static esp_mcp_value_t tool_wifi_status(const esp_mcp_property_list_t *props)
{
    (void)props;
    /* Check WiFi connection status via netif */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    bool connected = false;
    char ip_buf[16] = "0.0.0.0";
    if (netif) {
        connected = esp_netif_is_netif_up(netif);
        if (connected) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&ip_info.ip));
            }
        }
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"connected\":%s,\"ip\":\"%s\"}",
        connected ? "true" : "false", ip_buf);
    return esp_mcp_value_create_string(buf);
}

/* ── Tool: camera.capture_vision ──────────────────────────────── */
static esp_mcp_value_t tool_camera_capture_vision(const esp_mcp_property_list_t *props)
{
    (void)props;
    /* This tool signals the agent loop to capture a JPEG frame and
     * send it to the LLM Vision API. The actual capture is handled
     * by claw_core's media pipeline (supports_vision=true).
     * When the LLM requests a vision tool call, claw_core will:
     * 1. Capture a JPEG frame via CameraStream shared buffer
     * 2. Base64-encode and include as image content in the next LLM request
     * 3. The LLM Vision model describes what it sees
     *
     * For now, this returns instructions for the caller to use the
     * existing MJPEG stream or request vision via the agent. */
    if (!CameraDriver::instance().available()) {
        return mcp_respond_str("{\"error\":\"camera not available\"}");
    }
    return esp_mcp_value_create_string(
        "{\"message\":\"Vision is enabled on this device. Send an image to the agent via IM or use the MJPEG stream.\","
        "\"stream_url\":\"http://esp-web.local:81/stream\","
        "\"vision_enabled\":true}"
    );
}

/* ── Tool table ────────────────────────────────────────────────── */
/* 'extern' linkage required — C++ 'const' at namespace scope has
   internal linkage by default and gets stripped by the linker. */
cap_mcp_server_tool_def_t s_device_mcp_tools[] = {
    {
        .name = "system.info",
        .description = "Get device hardware info: has_lcd, sd_mounted, camera_available, audio_available",
        .callback = tool_system_info,
        .property_names = {NULL},
        .property_count = 0,
    },
    {
        .name = "system.stats",
        .description = "Get real-time CPU and memory statistics: per-core CPU%, free/min heap, free/min PSRAM, task count",
        .callback = tool_system_stats,
        .property_names = {NULL},
        .property_count = 0,
    },
    {
        .name = "volume.get",
        .description = "Get current audio volume level (0-100)",
        .callback = tool_volume_get,
        .property_names = {NULL},
        .property_count = 0,
    },
    {
        .name = "volume.set",
        .description = "Set audio volume level (0-100)",
        .callback = tool_volume_set,
        .property_names = {"volume"},
        .property_count = 1,
    },
    {
        .name = "camera.status",
        .description = "Get camera availability and current owner",
        .callback = tool_camera_status,
        .property_names = {NULL},
        .property_count = 0,
    },
    {
        .name = "camera.stream_start",
        .description = "Get camera stream access URLs (stream must be enabled via Settings first)",
        .callback = tool_camera_stream_start,
        .property_names = {NULL},
        .property_count = 0,
    },
    {
        .name = "sdcard.status",
        .description = "Get SD card mount status",
        .callback = tool_sdcard_status,
        .property_names = {NULL},
        .property_count = 0,
    },
    {
        .name = "device.restart",
        .description = "Restart the ESP32 device (use with caution)",
        .callback = tool_device_restart,
        .property_names = {NULL},
        .property_count = 0,
    },
    {
        .name = "audio.record_start",
        .description = "Start audio recording to SD card (provides API endpoint; use filename parameter for custom name)",
        .callback = tool_audio_record_start,
        .property_names = {"filename"},
        .property_count = 1,
    },
    {
        .name = "audio.play",
        .description = "Play an audio file from SD card (provides API endpoint; filename parameter required)",
        .callback = tool_audio_play,
        .property_names = {"filename"},
        .property_count = 1,
    },
    {
        .name = "brightness.get",
        .description = "Get current display brightness level (20-100)",
        .callback = tool_brightness_get,
        .property_names = {NULL},
        .property_count = 0,
    },
    {
        .name = "brightness.set",
        .description = "Set display brightness level (20-100)",
        .callback = tool_brightness_set,
        .property_names = {"brightness"},
        .property_count = 1,
    },
    {
        .name = "wifi.status",
        .description = "Get WiFi connection status and IP address",
        .callback = tool_wifi_status,
        .property_names = {NULL},
        .property_count = 0,
    },
    {
        .name = "camera.capture_vision",
        .description = "Capture a photo and describe it using LLM Vision (returns stream URL; vision is enabled on this device)",
        .callback = tool_camera_capture_vision,
        .property_names = {NULL},
        .property_count = 0,
    },
};

uint16_t s_device_mcp_tool_count =
    sizeof(s_device_mcp_tools) / sizeof(s_device_mcp_tools[0]);
