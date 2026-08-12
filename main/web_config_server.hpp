#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the web config server FreeRTOS task.
 *
 * Launches an HTTP server on port 80 with a web UI for configuring:
 *   - WiFi SSID / Password / Enable
 *   - Speaker Volume (0-100)
 *
 * Settings are persisted to NVS namespace "settings".
 * Designed for headless boards (ESP32-P4-WIFI6) without an LCD.
 */
void web_config_server_start(void);

/**
 * @brief Stop the web config server and cleanup.
 */
void web_config_server_stop(void);

/**
 * @brief Check if AAC file recording is active.
 * Used by AudioUlogRecorder for mutual exclusion.
 */
bool web_config_is_aac_recording(void);

/**
 * @brief Check if audio playback is active.
 * Used by AudioUlogRecorder for mutual exclusion.
 */
bool web_config_is_playing(void);

#ifdef __cplusplus
}
#endif

