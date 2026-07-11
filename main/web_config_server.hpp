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

#ifdef __cplusplus
}
#endif

#ifdef CONFIG_APP_CLAW_CAP_IM_LOCAL
/**
 * @brief Bind cap_im_local outbound callback to the agent response buffer.
 *
 * Call after cap_im_local_start() to connect the local IM channel's
 * outbound path to the web agent chat response buffer. Agent responses
 * routed through the "web_chat" channel will be stored and made
 * available via the /api/agent/messages polling endpoint.
 */
void bind_agent_outbound(void);
#endif
