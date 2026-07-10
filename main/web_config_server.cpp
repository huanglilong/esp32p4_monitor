/*
 * Web Config Server — HTTP-based settings UI with mDNS discovery.
 *
 * Provides a web page to configure WiFi credentials, speaker volume,
 * and other settings normally set through the LCD UI on LCD-4B boards.
 * All settings are persisted to NVS namespace "settings".
 *
 * Serves on port 8080 on both boards (coexists with CameraStream port 80/81).
 * Advertised via mDNS as <hostname>.local (unique per device, shared with CameraStream).
 *
 * Assumption: NVS already contains WiFi SSID/password.
 * The device must already be connected to a network before the web UI
 * is reachable.  (TODO: AP-mode provisioning for first-boot — see PROJECT.md)
 */

#include "web_config_server.hpp"
#include "peripherals.hpp"
#include "sdkconfig.h"
#include "example_config.h"

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <atomic>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "mdns.h"
#include "lwip/apps/netbiosns.h"
#include "cJSON.h"
#include "camera_stream.hpp"

/* uORB */
#include "uorb.h"
#include "topics.h"

/* Audio recording / playback */
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include <sys/time.h>

/* ESP-Claw IM — WeChat QR login API */
#ifdef CONFIG_APP_CLAW_CAP_IM_WECHAT
#include "cap_im_wechat.h"
#endif
#ifdef CONFIG_APP_CLAW_CAP_IM_FEISHU
#include "cap_im_feishu.h"
#endif
#ifdef CONFIG_APP_CLAW_CAP_IM_QQ
#include "cap_im_qq.h"
#endif
#ifdef CONFIG_APP_CLAW_CAP_IM_TG
#include "cap_im_tg.h"
#endif
#if defined(CONFIG_APP_CLAW_CAP_IM_WECHAT) || defined(CONFIG_APP_CLAW_CAP_IM_FEISHU) || \
    defined(CONFIG_APP_CLAW_CAP_IM_QQ) || defined(CONFIG_APP_CLAW_CAP_IM_TG)
#include "cap_im_platform.h"
#include "claw_event_router.h"
#include "claw_event.h"
#include "claw_agent_mgr.h"
#include "claw_cap.h"
#include "claw_memory.h"
#include "claw_skill.h"
#include "claw_paths.h"
#include "cap_system.h"
#include "cap_files.h"
#include "cap_http_request.h"
#include "cap_lua.h"
#include "cap_web_search.h"
#include "cap_llm_config.h"
#include "cap_session_mgr.h"
#include "cap_scheduler.h"
#include "cap_agent_mgr.h"
#include "cap_router_mgr.h"
#include "cap_skill_mgr.h"
#include "cap_mcp_server.h"
#include "cap_mcp_client.h"
#include "mcp_mdns.h"
#endif
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include "driver/i2s_std.h"
#include "esp_audio_simple_player.h"

/* ULog writer */
#include "ulog_writer.h"

/* SNTP */
#include "esp_sntp.h"

/* System Monitor */
#include "system_monitor.hpp"

extern "C" {
#include "layer3.h"
}

static const char *TAG = "WebConfig";

/* Board detection — still extern until fully migrated */

#define WEB_CONFIG_PORT         8080

/* NVS keys now defined in example_config.h (NVS_NAMESPACE_SETTINGS, NVS_KEY_*) */
/* Volume/Brightness constants now defined in example_config.h (VOLUME_MIN/MAX/DEFAULT, BRIGHTNESS_MIN/MAX/DEFAULT) */
/* Backward-compatible local alias for brevity in this file */
#define NVS_NAMESPACE           NVS_NAMESPACE_SETTINGS

#define TASK_STACK_SIZE         (4 * 1024)
#define TASK_PRIORITY           1
#define WIFI_CONNECT_TIMEOUT_MS 15000  /* Max wait for STA connection before giving up */
/* WIFI_CONNECTED_BIT now defined in example_config.h */

static httpd_handle_t s_httpd = NULL;
static std::atomic<TaskHandle_t> s_task_handle{nullptr};
static std::atomic<bool>  s_running{false};
static std::atomic<bool>  s_mdns_running{false};

/*============================================================================
 * Audio state — lazy-init when camera stream is OFF
 *============================================================================*/
#define REC_BUF_SAMPLES      480
#define REC_BUF_BYTES        (REC_BUF_SAMPLES * 2 * sizeof(int16_t))
#define ENC_SAMPLES_PER_CH   1152
#define PCM_BUF_SAMPLES      (ENC_SAMPLES_PER_CH * 2)

static std::atomic<bool> s_audio_inited{false};
static std::atomic<TaskHandle_t> s_audio_task{nullptr};
static StackType_t   *s_audio_stack = NULL;   /* 12KB, PSRAM (saves internal SRAM) */
static StaticTask_t  *s_audio_tcb = NULL;     /* small, internal SRAM */
static std::atomic<bool>  s_audio_running{false};
static std::atomic<bool>  s_is_recording{false};
static shine_t        s_shine = NULL;
static int16_t       *s_pcm_buf = NULL;
static std::atomic<int>  s_pcm_count{0};
static FILE          *s_rec_file = NULL;
static std::atomic<uint32_t> s_rec_bytes{0};
static std::atomic<uint32_t> s_rec_start_ms{0};
static char           s_rec_path[128];

/* Playback */
static esp_asp_handle_t s_asp = NULL;
static std::atomic<bool>    s_playing{false};

/* Mutual exclusion flag — file manager sets this to block audio ops during download/delete */
static std::atomic<bool>    s_fm_busy{false};

/* uORB recording_state publisher — notifies PhoneAppMusic when web recording is active */
static std::atomic<orb_advert_t> s_rec_pub{ORB_ADVERT_INVALID};

/* Mutex to serialize audio operations across concurrent HTTP handlers.
 * Without this, two clients hitting /api/record and /api/play simultaneously
 * could corrupt s_is_recording, s_asp, s_shine, etc.
 * Created at task startup (not lazily) to avoid a race between two handlers
 * both seeing s_audio_mutex == NULL and creating separate mutexes. */
static SemaphoreHandle_t s_audio_mutex = NULL;

static void audio_lock(void)
{
    if (s_audio_mutex) {
        xSemaphoreTake(s_audio_mutex, portMAX_DELAY);
    }
}

static void audio_unlock(void)
{
    if (s_audio_mutex) {
        xSemaphoreGive(s_audio_mutex);
    }
}

static void _stop_audio_task_if_running(void)
{
    if (!s_audio_task.load(std::memory_order_acquire) && !s_audio_running) {
        return;
    }

    s_audio_running = false;
    for (int i = 0; i < 10 && s_audio_task.load(std::memory_order_acquire); ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (s_audio_task.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Audio task did not exit in time, force deleting");
        vTaskDelete(s_audio_task.exchange(nullptr, std::memory_order_acq_rel));
    }

    /* Free PSRAM-allocated stack (xTaskCreateStaticPinnedToCore does not free it).
     * TCB is pre-allocated and reused — not freed here.
     * Stack is safe to free immediately: FreeRTOS doesn't reference it
     * after the task exits (idle task only reclaims the TCB). */
    if (s_audio_stack) { heap_caps_free(s_audio_stack); s_audio_stack = NULL; }
}

/* Audio recording task: I2S RX → shine MP3 → SD card */
static void audio_task(void *arg)
{
    (void)arg;
    int16_t *buf = (int16_t *)heap_caps_calloc(1, REC_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { s_audio_running = false; s_audio_task.store(nullptr, std::memory_order_release); vTaskDelete(NULL); return; }
    while (s_audio_running) {
        /* Guard: if audio driver was deinitialized (e.g., by PhoneAppAudio::close()),
         * rx_handle() returns nullptr and i2s_channel_read would crash. */
        i2s_chan_handle_t rx = PeripheralManager::instance().rx_handle();
        if (!rx) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        size_t n = 0;
        if (i2s_channel_read(rx, buf, REC_BUF_BYTES, &n, pdMS_TO_TICKS(100)) != ESP_OK || n == 0) continue;
        int32_t spc = n / (2 * sizeof(int16_t));
        if (s_is_recording && s_pcm_buf && s_shine) {
            for (int32_t i = 0; i < spc; i++) {
                int idx = s_pcm_count.load(std::memory_order_relaxed);
                s_pcm_buf[idx * 2]     = buf[i * 2];
                s_pcm_buf[idx * 2 + 1] = buf[i * 2 + 1];
                if (s_pcm_count.fetch_add(1, std::memory_order_relaxed) + 1 >= ENC_SAMPLES_PER_CH) {
                    int wr = 0;
                    unsigned char *mp3 = shine_encode_buffer_interleaved(s_shine, s_pcm_buf, &wr);
                    if (mp3 && wr > 0 && s_rec_file) s_rec_bytes.fetch_add(fwrite(mp3, 1, wr, s_rec_file), std::memory_order_relaxed);
                    s_pcm_count.store(0, std::memory_order_relaxed);
                }
            }
        }
    }
    heap_caps_free(buf);
    s_audio_task.store(nullptr, std::memory_order_release);
    vTaskDelete(NULL);
}

/*============================================================================
 * NVS Helpers — with RAM cache for frequently-read integer keys.
 *
 * Inspired by PX4's layered parameter system: read from RAM cache (O(1)),
 * fall through to NVS on cache miss, and invalidate on write.
 * This avoids ~1ms flash access per nvs_get_i32_def() call.
 *============================================================================*/

/** Cached NVS integer entry — key is copied into the fixed buffer. */
typedef struct {
    char        key[16];  /* All known NVS keys are < 16 bytes */
    int32_t     value;
    bool        valid;    /* true after first read from NVS */
} nvs_cache_entry_t;

#define NVS_CACHE_MAX 8
static nvs_cache_entry_t s_nvs_cache[NVS_CACHE_MAX];
static int s_nvs_cache_count;
static SemaphoreHandle_t s_nvs_cache_mutex;

/** Find or create a cache slot for the given key. Returns slot index, or -1.
 *  Must be called with s_nvs_cache_mutex held. */
static int nvs_cache_find_locked(const char *key)
{
    for (int i = 0; i < s_nvs_cache_count; i++) {
        if (strcmp(s_nvs_cache[i].key, key) == 0) return i;
    }
    if (s_nvs_cache_count < NVS_CACHE_MAX) {
        int i = s_nvs_cache_count++;
        strlcpy(s_nvs_cache[i].key, key, sizeof(s_nvs_cache[i].key));
        s_nvs_cache[i].valid = false;
        return i;
    }
    return -1;
}

static int32_t nvs_get_i32_def(const char *key, int32_t def)
{
    /* Check RAM cache first (O(1), no flash access) */
    xSemaphoreTake(s_nvs_cache_mutex, portMAX_DELAY);
    int ci = nvs_cache_find_locked(key);
    if (ci >= 0 && s_nvs_cache[ci].valid) {
        int32_t val = s_nvs_cache[ci].value;
        xSemaphoreGive(s_nvs_cache_mutex);
        return val;
    }
    xSemaphoreGive(s_nvs_cache_mutex);

    /* Cache miss: read from NVS */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return def;
    int32_t val = def;
    nvs_get_i32(h, key, &val);
    nvs_close(h);

    /* Populate cache */
    xSemaphoreTake(s_nvs_cache_mutex, portMAX_DELAY);
    ci = nvs_cache_find_locked(key);
    if (ci >= 0) {
        s_nvs_cache[ci].value = val;
        s_nvs_cache[ci].valid = true;
    }
    xSemaphoreGive(s_nvs_cache_mutex);
    return val;
}

static void nvs_write_i32(const char *key, int32_t value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, key, value);
    nvs_commit(h);
    nvs_close(h);

    /* Update cache (write-through) */
    xSemaphoreTake(s_nvs_cache_mutex, portMAX_DELAY);
    int ci = nvs_cache_find_locked(key);
    if (ci >= 0) {
        s_nvs_cache[ci].value = value;
        s_nvs_cache[ci].valid = true;
    }
    xSemaphoreGive(s_nvs_cache_mutex);
}

static void nvs_get_str(const char *key, char *out, size_t max_len)
{
    nvs_handle_t h;
    out[0] = '\0';
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = max_len;
    nvs_get_str(h, key, out, &len);
    nvs_close(h);
}

static void nvs_set_str_def(const char *key, const char *value)
{
    if (!value || strlen(value) == 0) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

/*============================================================================
 * HTML Web UI (single-page application)
 *============================================================================*/
static const char *WEB_UI_HTML =
"<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>ESP32-P4 Settings</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
"background:#1a1a2e;color:#e0e0e0;min-height:100vh;display:flex;"
"justify-content:center;padding:16px;padding-top:52px}"
".topnav{position:fixed;top:0;left:0;right:0;background:#0f3460;color:#00d4ff;"
"padding:10px 16px;text-align:center;font-size:14px;z-index:100;"
"border-bottom:1px solid #1a4a6e}"
".topnav a{color:#00d4ff;font-weight:bold;text-decoration:none}"
".topnav a:hover{text-decoration:underline}"
".container{max-width:420px;width:100%}"
"h1{text-align:center;font-size:22px;margin:16px 0 24px;color:#00d4ff}"
".card{background:#16213e;border-radius:12px;padding:20px;margin-bottom:16px;"
"box-shadow:0 2px 8px rgba(0,0,0,.3)}"
".card h2{font-size:16px;margin-bottom:16px;color:#00d4ff;"
"border-bottom:1px solid #0f3460;padding-bottom:8px}"
"label{display:block;font-size:13px;color:#a0a0b0;margin-bottom:4px}"
"input[type=text],input[type=password]{width:100%;padding:10px 12px;"
"border-radius:8px;border:1px solid #0f3460;background:#0f3460;color:#e0e0e0;"
"font-size:14px;margin-bottom:12px;outline:none;transition:border .2s}"
"input:focus{border-color:#00d4ff}"
".toggle-row{display:flex;align-items:center;justify-content:space-between;"
"margin-bottom:12px}"
".toggle{position:relative;width:48px;height:26px;cursor:pointer}"
".toggle input{display:none}"
".toggle .slider{position:absolute;top:0;left:0;right:0;bottom:0;"
"background:#333;border-radius:26px;transition:.3s}"
".toggle .slider::before{content:'';position:absolute;width:20px;height:20px;"
"left:3px;bottom:3px;background:#666;border-radius:50%;transition:.3s}"
".toggle input:checked+.slider{background:#00d4ff}"
".toggle input:checked+.slider::before{transform:translateX(22px);background:#fff}"
".slider-container{display:flex;align-items:center;gap:12px}"
"input[type=range]{flex:1;appearance:none;height:6px;background:#0f3460;"
"border-radius:3px;outline:none}"
"input[type=range]::-webkit-slider-thumb{appearance:none;width:20px;height:20px;"
"border-radius:50%;background:#00d4ff;cursor:pointer}"
".slider-val{font-size:14px;font-weight:bold;color:#00d4ff;min-width:36px}"
"button{width:100%;padding:12px;background:linear-gradient(135deg,#00d4ff,#0099cc);"
"color:#fff;border:none;border-radius:8px;font-size:16px;font-weight:bold;"
"cursor:pointer;transition:opacity .2s}"
"button:hover{opacity:.9} button:active{opacity:.7}"
"#status{text-align:center;font-size:13px;margin-top:12px;min-height:18px;"
"transition:color .3s}"
"#status.success{color:#4caf50} #status.error{color:#f44336} #status.info{color:#00d4ff}"
"</style></head><body>"
"<div class=\"topnav\">"
"📷 <a id=\"cam-link\">Jump to Camera Stream</a>"
"</div>"
"<script>"
"document.getElementById('cam-link').href='http://'+window.location.hostname+':80/';"
"</script>"
"<div class=\"container\">"
"<h1>ESP32-P4 Configs</h1>"
"<div class=\"card\">"
"<h2>WiFi Settings</h2>"
"<div class=\"toggle-row\">"
"<span>WiFi (Always On)</span></div>"
"<label>SSID</label>"
"<input type=\"text\" id=\"ssid\" maxlength=\"32\" placeholder=\"WiFi name\">"
"<label>Password</label>"
"<input type=\"password\" id=\"pass\" maxlength=\"64\" placeholder=\"WiFi password\">"
"</div>"
"<div class=\"card\">"
"<h2>Camera Stream</h2>"
"<div class=\"toggle-row\">"
"<span id=\"cam_label\">Enable (WiFi required)</span>"
"<label class=\"toggle\">"
"<input type=\"checkbox\" id=\"cam_stream\" onchange=\"onCamToggle()\">"
"<span class=\"slider\"></span></label></div>"
"<div id=\"cam_status\" style=\"font-size:12px;color:#a0a0b0;margin-top:4px\"></div>"
"</div>"
"<div class=\"card\" id=\"audio_card\" style=\"display:none\">"
"<div style=\"display:flex;align-items:center;justify-content:space-between;margin-bottom:8px\">"
"<h2 style=\"margin:0;border:none;padding:0\">Audio Recorder</h2>"
"<button onclick=\"switchMode()\" id=\"btn_switch_mode\" style=\"width:auto;padding:4px 12px;font-size:12px;background:#333\">📁 Files</button>"
"</div>"
"<div style=\"display:flex;gap:8px;margin-bottom:8px\">"
"<button id=\"btn_rec\" onclick=\"onRecord()\" style=\"flex:1\">Start Record</button>"
"<button id=\"btn_stop\" onclick=\"onStopAll()\" style=\"flex:0 0 auto;width:auto;padding:12px 16px;background:#e65100;display:none\">Stop</button></div>"
"<div id=\"rec_stat\" style=\"font-size:12px;color:#4caf50;margin-bottom:8px;min-height:16px\"></div>"
"<div id=\"file_list\" style=\"font-size:13px;max-height:160px;overflow-y:auto\"></div>"
"</div>"
"<div class=\"card\" id=\"files_card\" style=\"display:none\">"
"<div style=\"display:flex;align-items:center;justify-content:space-between;margin-bottom:8px\">"
"<h2 style=\"margin:0;border:none;padding:0\">File Manager</h2>"
"<button onclick=\"switchMode()\" id=\"btn_switch_fm\" style=\"width:auto;padding:4px 12px;font-size:12px;background:#333\">🎤 Audio</button>"
"</div>"
"<div id=\"fm_breadcrumb\" style=\"font-size:12px;color:#00d4ff;margin-bottom:4px;word-break:break-all\">/</div>"
"<div id=\"fm_capacity\" style=\"font-size:11px;color:#666;margin-bottom:8px\"></div>"
"<div id=\"fm_list\" style=\"font-size:13px;max-height:200px;overflow-y:auto\"></div>"
"</div>"
"<div class=\"card\">"
"<h2>Volume</h2>"
"<div class=\"slider-container\">"
"<input type=\"range\" id=\"volume\" min=\"0\" max=\"100\" value=\"60\" oninput=\"updateVol()\">"
"<span class=\"slider-val\" id=\"vol_val\">60</span></div>"
"</div>"
"<div class=\"card\">"
"<h2>ULog Recording</h2>"
"<div style=\"display:flex;gap:8px\">"
"<button id=\"btn_ulog_start\" onclick=\"onUlogStart()\" style=\"flex:1\">Start</button>"
"<button id=\"btn_ulog_stop\" onclick=\"onUlogStop()\" style=\"flex:0 0 auto;width:auto;padding:12px 16px;background:#e65100;display:none\">Stop</button></div>"
"<div id=\"ulog_stat\" style=\"font-size:12px;color:#a0a0b0;margin-top:8px;min-height:16px\"></div>"
"</div>"
#ifdef CONFIG_APP_CLAW_CAP_IM_WECHAT
"<div class=\"card\">"
"<h2>WeChat Bot</h2>"
"<div id=\"wx_status\" style=\"font-size:13px;color:#a0a0b0;margin-bottom:12px\">Not configured</div>"
"<div id=\"wx_qr_area\" style=\"text-align:center;margin-bottom:12px;display:none\">"
"<canvas id=\"wx_qr_cv\" style=\"width:400px;max-width:100%;image-rendering:pixelated;border-radius:8px;border:2px solid #0f3460\"></canvas>"
"<div style=\"font-size:12px;color:#a0a0b0;margin-top:8px\">Scan with WeChat to login</div>"
"</div>"
"<div style=\"display:flex;gap:8px\">"
"<button id=\"btn_wx_login\" onclick=\"onWxLogin()\" style=\"flex:1\">QR Login</button>"
"<button id=\"btn_wx_cancel\" onclick=\"onWxCancel()\" style=\"flex:0 0 auto;width:auto;padding:12px 16px;background:#e65100;display:none\">Cancel</button>"
"</div>"
"</div>"
#endif
"<div class=\"card\">"
"<h2>AI Agent (LLM)</h2>"
"<label>Provider</label>"
"<select id=\"llm_provider\" style=\"width:100%;padding:10px 12px;border-radius:8px;"
"border:1px solid #0f3460;background:#0f3460;color:#e0e0e0;font-size:14px;margin-bottom:12px\">"
"<option value=\"deepseek\">DeepSeek</option>"
"<option value=\"openai\">OpenAI</option>"
"<option value=\"anthropic\">Anthropic</option>"
"<option value=\"qwen\">Qwen (Bailian)</option>"
"<option value=\"custom\">Custom</option>"
"</select>"
"<label>API Key</label>"
"<input type=\"password\" id=\"llm_key\" placeholder=\"sk-...\" style=\"width:100%;padding:10px 12px;"
"border-radius:8px;border:1px solid #0f3460;background:#0f3460;color:#e0e0e0;font-size:14px;margin-bottom:12px\">"
"<label>Model</label>"
"<input type=\"text\" id=\"llm_model\" placeholder=\"deepseek-chat\" style=\"width:100%;padding:10px 12px;"
"border-radius:8px;border:1px solid #0f3460;background:#0f3460;color:#e0e0e0;font-size:14px;margin-bottom:12px\">"
"<label>Base URL (optional)</label>"
"<input type=\"text\" id=\"llm_url\" placeholder=\"https://api.deepseek.com\" style=\"width:100%;padding:10px 12px;"
"border-radius:8px;border:1px solid #0f3460;background:#0f3460;color:#e0e0e0;font-size:14px;margin-bottom:12px\">"
"<button onclick=\"saveLlmConfig()\">Save LLM Config</button>"
"<div id=\"llm_status\" style=\"font-size:12px;color:#a0a0b0;margin-top:8px;min-height:16px\"></div>"
"</div>"

/* ── Agent Chat Card ── */
"<div class=\"card\" id=\"chat_card\">"
"<h2>Agent Chat</h2>"
"<div id=\"chat_log\" style=\"height:260px;overflow-y:auto;background:#0a0a1a;border-radius:8px;"
"padding:10px;font-size:13px;line-height:1.6;margin-bottom:12px\"></div>"
"<div style=\"display:flex;gap:8px;align-items:center\">"
"<input type=\"text\" id=\"chat_input\" placeholder=\"Ask the AI agent...\" "
"style=\"flex:1;width:auto;padding:10px 12px;border-radius:8px;border:1px solid #0f3460;"
"background:#0f3460;color:#e0e0e0;font-size:14px;outline:none;margin-bottom:0\" "
"onkeydown=\"if(event.key==='Enter')sendChat()\">"
"<button onclick=\"sendChat()\" style=\"width:auto;padding:10px 16px;border-radius:8px;"
"background:linear-gradient(135deg,#00d4ff,#0078d4);color:#fff;border:none;"
"font-size:14px;cursor:pointer;white-space:nowrap\">Send</button>"
"</div>"
"<div id=\"chat_status\" style=\"font-size:11px;color:#a0a0b0;margin-top:6px;min-height:14px\"></div>"
"</div>"

"<button onclick=\"saveSettings()\">Save Settings</button>"
"<div id=\"status\"></div>"
"<div style=\"margin-top:24px;padding-top:16px;border-top:1px solid #0f3460\">"
"<button onclick=\"factoryReset()\" style=\"background:linear-gradient(135deg,#ff4444,#cc0000)\">"
"Factory Reset (Erase NVS)</button></div>"
"</div>"
"<script>"
"function updateVol(){"
"document.getElementById('vol_val').textContent=document.getElementById('volume').value}"
"function updateUI(){"
"document.getElementById('ssid').disabled=false;"
"document.getElementById('pass').disabled=false}"
"async function loadStatus(){"
"try{let r=await fetch('/api/status');let j=await r.json();"
"document.getElementById('ssid').value=j.ssid||'';"
"document.getElementById('pass').placeholder=j.has_pass?'(saved)':'WiFi password';"
"document.getElementById('pass').value='';"
"document.getElementById('volume').value=j.volume||60;"
"document.getElementById('vol_val').textContent=j.volume||60;"
"document.getElementById('cam_stream').checked=j.cam_stream!=0;"
"let cs=document.getElementById('cam_status');"
"cs.textContent=j.cam_running?(j.cam_recording?'● Streaming + Recording':'● Streaming active'):'○ Stopped';"
"cs.style.color=j.cam_running?'#4caf50':'#a0a0b0';"
"document.getElementById('audio_card').style.display='block';"
"document.getElementById('files_card').style.display='none';"
"fmMode=false;"
"loadFiles();"
"loadUlogStatus();"
"refreshRec();"
"if(!uiTimer)uiTimer=setInterval(pollLive,2000);"
"updateUI()}catch(e){showStatus('Failed to load settings','error')}}"
"function showStatus(msg,cls){let s=document.getElementById('status');"
"s.textContent=msg;s.className=cls}"
"async function onCamToggle(){"
"let en=document.getElementById('cam_stream').checked;"
"showStatus(en?'Starting camera stream...':'Stopping camera stream...','info');"
"try{let r=await fetch('/api/camera_stream',{method:'POST',"
"headers:{'Content-Type':'application/json'},body:JSON.stringify({enable:en?1:0})});"
"let j=await r.json();"
"if(j.ok){document.getElementById('cam_stream').checked=j.enabled;"
"let cs=document.getElementById('cam_status');"
"cs.textContent=j.running?(j.recording?'● Streaming + Recording':'● Streaming active'):'○ Stopped';"
"cs.style.color=j.running?'#4caf50':'#a0a0b0';"
"document.getElementById('files_card').style.display='none';"
"fmMode=false;"
"loadFiles();"
"showStatus(j.running?'Stream started':'Stream stopped','success')}"
"else{showStatus(j.error||'Failed','error');loadStatus()}}"
"catch(e){showStatus('Connection error','error');loadStatus()}}"
"var uiTimer=null;"
"async function loadFiles(){"
"try{var r=await fetch('/api/audio/list');var j=await r.json();"
"var h='';"
"if(j.files&&j.files.length)"
"for(var i=0;i<j.files.length;i++)"
"h+=`<div style=\"display:flex;justify-content:space-between;align-items:center;padding:4px 0;border-bottom:1px solid #0f3460\">"
"<span style=\"flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap\">${j.files[i]}</span>"
"<button onclick='playFile(${JSON.stringify(j.files[i])})' style=\"width:auto;padding:4px 12px;font-size:12px;margin-left:8px\">Play</button></div>`;"
"else h='<span style=\"color:#666;font-size:12px\">No recordings</span>';"
"document.getElementById('file_list').innerHTML=h}"
"catch(e){}}"
"async function onRecord(){"
"var b=document.getElementById('btn_rec');"
"if(b.textContent=='Start Record'){"
"showStatus('Starting...','info');"
"try{var r=await fetch('/api/audio/record_start');var j=await r.json();"
"if(j.ok){b.textContent='End Record';b.style.background='#c00';"
"document.getElementById('btn_stop').style.display='block';"
"showStatus('Recording...','success');refreshRec()}"
"else showStatus('Error: '+j.error,'error')}"
"catch(e){showStatus('Error','error')}}else endRecord()}"
"async function endRecord(){"
"try{var r=await fetch('/api/audio/record_stop');var j=await r.json();"
"var b=document.getElementById('btn_rec');"
"b.textContent='Start Record';b.style.background='';"
"document.getElementById('btn_stop').style.display='none';"
"document.getElementById('rec_stat').textContent='';"
"showStatus('Saved: '+j.file,'success');loadFiles()}"
"catch(e){showStatus('Error','error')}}"
"async function refreshRec(){"
"try{var r=await fetch('/api/audio/record_status');var j=await r.json();"
"if(j.recording){var m=Math.floor(j.seconds/60);var s=j.seconds%60;"
"document.getElementById('rec_stat').textContent='Recording '+('0'+m).slice(-2)+':'+('0'+s).slice(-2)+' | '+Math.round(j.bytes/1024)+'KB'}"
"else document.getElementById('rec_stat').textContent=''}"
"catch(e){}}"
"async function pollLive(){loadUlogStatus();refreshRec();checkWxSession();}"
"async function playFile(name){"
"showStatus('Playing: '+name,'info');"
"try{var r=await fetch('/api/audio/play?file='+encodeURIComponent(name));"
"var j=await r.json();"
"if(j.ok){document.getElementById('btn_stop').style.display='block';showStatus('Playing: '+name,'success')}"
"else showStatus('Error: '+j.error,'error')}"
"catch(e){showStatus('Error','error')}}"
"async function onStopAll(){"
"try{await Promise.all([fetch('/api/audio/stop')]);"
"if(document.getElementById('btn_rec').textContent!='Start Record')await endRecord();"
"document.getElementById('btn_stop').style.display='none';"
"loadFiles();showStatus('Stopped','success')}"
"catch(e){showStatus('Error','error')}}"
"/* File Manager */"
"var fmCurrentDir='/';"
"var fmMode=false; /* false=audio, true=file manager */"
"function switchMode(){"
"if(!fmMode){"
"/* Switching to file manager: stop any audio */"
"if(document.getElementById('btn_rec').textContent!='Start Record'){endRecord();}"
"fetch('/api/audio/stop');"
"document.getElementById('btn_stop').style.display='none';"
"showStatus('','');"
"fmMode=true;"
"document.getElementById('audio_card').style.display='none';"
"document.getElementById('files_card').style.display='block';"
"loadFileManager('/')"
"}else{"
"fmMode=false;"
"document.getElementById('files_card').style.display='none';"
"document.getElementById('audio_card').style.display='block';"
"loadFiles()"
"}}"
"async function loadFileManager(d){"
"if(typeof d!=='undefined')fmCurrentDir=d;"
"try{var r=await fetch('/api/files/list?dir='+encodeURIComponent(fmCurrentDir));"
"var j=await r.json();"
"if(!j.ok){document.getElementById('fm_breadcrumb').textContent='Error';return}"
"document.getElementById('fm_breadcrumb').textContent=j.current||'/';"
"var cap='';"
"if(j.total_kb&&j.total_kb>0){var free=j.free_kb, total=j.total_kb, used=total-free;"
"var u=used>1048576?(used/1048576).toFixed(1)+'GB':used>1024?(used/1024).toFixed(1)+'MB':used+'KB';"
"var t=total>1048576?(total/1048576).toFixed(1)+'GB':total>1024?(total/1024).toFixed(1)+'MB':total+'KB';"
"cap=u+' used / '+t}"
"else if(j.total_kb!==undefined)cap='Capacity unknown';"
"document.getElementById('fm_capacity').textContent=cap;"
"var h='';"
"if(fmCurrentDir!=='/')h+='<div style=\"display:flex;align-items:center;padding:3px 0;border-bottom:1px solid #0f3460;cursor:pointer;color:#00d4ff\" onclick=\"navigateUp()\">📁 ..</div>';"
"j.files.sort(function(a,b){if(a.is_dir!=b.is_dir)return a.is_dir?-1:1;return a.name.localeCompare(b.name)});"
"if(j.files&&j.files.length)"
"for(var i=0;i<j.files.length;i++){"
"var f=j.files[i];var icon=f.is_dir?'📁':'📄';"
"var sz=f.size>1048576?(f.size/1048576).toFixed(1)+'MB':f.size>1024?Math.round(f.size/1024)+'KB':f.size+'B';"
"var ej=JSON.stringify(f.name);var dn=f.name.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;');"
"if(f.is_dir){h+='<div style=\"display:flex;justify-content:space-between;align-items:center;padding:3px 0;border-bottom:1px solid #0f3460\"><span style=\"flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;cursor:pointer\" onclick=\\'navigateTo('+ej+')\\'>'+icon+' '+dn+' <span style=\"color:#666;font-size:10px\">'+sz+'</span></span><span style=\"display:flex;gap:4px;margin-left:8px\"><button onclick=\\'navigateTo('+ej+')\\' style=\"width:auto;padding:3px 8px;font-size:11px\">Open</button><button onclick=\\'deleteFile('+ej+')\\' style=\"width:auto;padding:3px 8px;font-size:11px;background:#c00\">🗑</button></span></div>'}"
"else{h+='<div style=\"display:flex;justify-content:space-between;align-items:center;padding:3px 0;border-bottom:1px solid #0f3460\"><span style=\"flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap\">'+icon+' '+dn+' <span style=\"color:#666;font-size:10px\">'+sz+'</span></span><span style=\"display:flex;gap:4px;margin-left:8px\"><button onclick=\\'downloadFile('+ej+')\\' style=\"width:auto;padding:3px 8px;font-size:11px\">⬇</button><button onclick=\\'deleteFile('+ej+')\\' style=\"width:auto;padding:3px 8px;font-size:11px;background:#c00\">🗑</button></span></div>'}"
"}"
"document.getElementById('fm_list').innerHTML=h}"
"catch(e){document.getElementById('fm_breadcrumb').textContent='Load failed'}}"
"function navigateTo(name){"
"var p=fmCurrentDir;if(p[p.length-1]!=='/')p+='/';"
"loadFileManager(p+name)}"
"function navigateUp(){"
"if(fmCurrentDir=='/'||fmCurrentDir=='')return;"
"var p=fmCurrentDir;if(p[p.length-1]=='/')p=p.slice(0,-1);"
"var i=p.lastIndexOf('/');"
"loadFileManager(i<0?'/':p.substring(0,i)||'/')}"
"function downloadFile(name){"
"var p=fmCurrentDir;if(p[p.length-1]!=='/')p+='/';"
"window.open('/api/files/download?path='+encodeURIComponent(p+name),'_blank')}"
"async function deleteFile(name){"
"var p=fmCurrentDir;if(p[p.length-1]!=='/')p+='/';"
"if(!confirm('Delete '+name+'?'))return;"
"showStatus('Deleting...','info');"
"try{var r=await fetch('/api/files/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:p+name})});"
"var j=await r.json();"
"if(j.ok){showStatus('Deleted: '+name,'success');loadFileManager()}"
"else showStatus('Error: '+j.error,'error')}"
"catch(e){showStatus('Error','error')}}"
"async function saveSettings(){"
"let data={ssid:document.getElementById('ssid').value,"
"pass:document.getElementById('pass').value,"
"volume:parseInt(document.getElementById('volume').value)};"
"showStatus('Saving...','info');"
"try{let r=await fetch('/api/settings',{method:'POST',"
"headers:{'Content-Type':'application/json'},body:JSON.stringify(data)});"
"let j=await r.json();"
"if(j.ok)showStatus('Saved! WiFi will reconnect.','success');"
"else showStatus('Save failed: '+j.error,'error')}"
"catch(e){showStatus('Connection error','error')}}"
"async function factoryReset(){"
"if(!confirm('Are you sure? This will erase ALL NVS settings and reboot.'))return;"
"showStatus('Factory resetting...','error');"
"try{let r=await fetch('/api/factory_reset',{method:'POST'});"
"let j=await r.json();showStatus(j.message||'Rebooting...','success')}"
"catch(e){showStatus('Device is rebooting...','success')}}"
"async function loadUlogStatus(){"
"try{let r=await fetch('/api/ulog/status');let j=await r.json();"
"let s=document.getElementById('ulog_stat');"
"let bs=document.getElementById('btn_ulog_start');"
"let bp=document.getElementById('btn_ulog_stop');"
"if(j.running){"
"s.textContent='● Logging '+Math.round(j.bytes_written/1024)+'KB → '+j.filepath;"
"s.style.color='#4caf50';bs.style.display='none';bp.style.display='block'"
"}else{"
"s.textContent='○ Stopped';"
"s.style.color='#a0a0b0';bs.style.display='block';bp.style.display='none'}"
"}catch(e){}}"
"async function onUlogStart(){"
"showStatus('Starting ULog...','info');"
"try{let r=await fetch('/api/ulog/start',{method:'POST'});"
"let j=await r.json();"
"if(j.ok){showStatus('ULog started','success');loadUlogStatus()}"
"else showStatus('ULog error: '+(j.error||'failed'),'error')}"
"catch(e){showStatus('ULog connection error','error')}}"
"async function onUlogStop(){"
"try{let r=await fetch('/api/ulog/stop',{method:'POST'});"
"let j=await r.json();"
"showStatus('ULog stopped','success');loadUlogStatus()}"
"catch(e){showStatus('ULog error','error')}}"
"loadStatus();loadLlmConfig();"
#ifdef CONFIG_APP_CLAW_CAP_IM_WECHAT
/* lean-qr v2.7.2 (MIT, https://github.com/nicolo-ribaudo/lean-qr) —
 * nano build, browser-compatible (no ES module export).
 * WeChat qr_data_url is an HTML page, not an image; we render the URL
 * as a QR code on canvas so WeChat can scan it directly. */
"const t=[.2,3/8,5/9,2/3],o=(o,r)=>e=>{const n=4*o+e-4,f=\"*-04-39?2$%%$%%'$%''%'''%')(%'))%(++'(++'(+.'+-.',/3',33)-/5)-43).36)058*18<+37<+4:<,4:E,5<A-7>C/8@F/:EH/<EK0=FM1?IP2@KS3BNV4DPY5FS\\\\6HV_6IXb7K[e8N^i9Pam;Rdp<Tgt\".charCodeAt(n)-35,s=n>8?f:1,l=r/s|0,c=r%s,a=s-c,i=n>8?l*t[e]+(o>5)&-2:f,d=l-i;return{t:a*d+c*d+c,o:[[a,d],[c,d+1]],l:i}},r={L:0,M:1,Q:2,H:3},e=t=>new Uint8Array(t),n=t=>{const o=new Error(`lean-qr error ${t}`);throw o.code=t,o},f=(t,o=t*t,r=e(o))=>({size:t,i:r,get:(o,e)=>o>=0&&o<t&&!!(1&r[e*t+o]),u(o,{on:r=[0,0,0],off:e=[0,0,0,0],pad:n=4,padX:f=n,padY:s=n}={}){const l=t+2*f,c=t+2*s,a=o.createImageData(l,c),i=new Uint32Array(a.data.buffer);a.data.set([...r,255]);const d=i[0];a.data.set([...e,255]);const u=i[0];for(let t=0;t<c;++t)for(let o=0;o<l;++o)i[t*l+o]=this.get(o-f,t-s)?d:u;return a},toCanvas(t,o){const r=t.getContext(\"2d\"),e=this.u(r,o);t.width=e.width,t.height=e.height,r.putImageData(e,0,0)}}),s=[(t,o)=>1&(t^o),(t,o)=>1&o,t=>t%3,(t,o)=>(t+o)%3,(t,o)=>1&(t/3^o>>1),(t,o)=>(t&o&1)+t*o%3,(t,o)=>(t&o)+t*o%3&1,(t,o)=>(t^o)+t*o%3&1],l=e(511);for(let t=0,o=1;t<255;o=2*o^285*(o>127))l[l[o+255]=t++]=o;const c=t=>l[t%255],a=t=>l[t+255],i=(t,o)=>{const r=e(t.length+o.length-1);for(let e=0;e<t.length;++e)for(let n=0;n<o.length;++n)r[e+n]^=c(t[e]+o[n]);return r.map(a)},d=(t,o)=>{const r=e(t.length+o.length-1);r.set(t,0);for(let e=0;e<t.length;++e)if(r[e]){const t=a(r[e]);for(let n=0;n<o.length;++n)r[e+n]^=c(o[n]+t)}return r.slice(t.length)},u=[[0],[0,0]];for(let t=1;t<30;++t)u.push(i(u[t],[0,t]));const _=(t,o)=>{const r=[[],[]];let n=0,f=0;for(const[e,s]of o.o)for(let l=0;l<e;++l,n+=s){const e=t.slice(n,n+s);r[0].push(e),r[1].push(d(e,u[o.l])),f+=s+o.l}const s=e(f);f=0;for(const t of r)for(let o=0,r=-1;r<f;++o){r=f;for(const r of t)o<r.length&&(s[f++]=r[o])}return s},p=(t,o,r)=>{let e=t<<=r;for(let t=134217728;t>>=1;)e&t&&(e^=o*(t>>r));return e|t},z=({size:t,i:o},r)=>{const e=(r,e,n,f)=>{for(;n-- >0;r+=t)o.fill(f,r,r+e)},n=(o,r,n)=>{for(let f=0;f++<3;n-=2)e(r*t+o-(n>>1)*(t+1),n,n,2|f)},f=2*((t-13)/(1+(r/7|0))/2+.75|0);if(r>1)for(let o=t-7;o>8;o-=f){for(let t=o;t>8;t-=f)n(o,t,5);o<t-7&&n(o,6,5)}if(r>6)for(let e=p(r,7973,12),n=1;n<7;++n)for(let r=12;r-- >9;e>>=1)o[n*t-r]=2|1&e;e(7,2,9,2),e(t-8,8,9,2);for(let r=0;r<t;++r)o[6*t+r]=3^1&r;n(3,3,7),n(t-4,3,7);for(let r=0;r<t;++r)for(let e=r;e<t;++e)o[e*t+r]=o[r*t+e];o[(t-8)*t+8]=3},g=({size:t,i:o})=>{const r=[];for(let e=t-2,n=t,f=-1;e>=0;e-=2){for(5===e&&(e=4);n+=f,-1!==n&&n!==t;){const f=n*t+e;o[f+1]||r.push(f+1),o[f]||r.push(f)}f*=-1}return r},w=({i:t},o,r)=>o.forEach((o,e)=>t[o]=r[e>>3]>>(7&~e)&1),y=({size:t,i:o},r,e,n)=>{for(let e=0;e<t;++e)for(let n=0;n<t;++n){const f=e*t+n;o[f]^=!(r(n,e)||2&o[f])}let f=21522^p((1^n)<<3|e,1335,10);for(let r=0;r++<8;f>>=1)o[(r-(r<7))*t+8]=1&f,o[9*t-r]=1&f;for(let r=8;--r,f;f>>=1)o[8*t+r-(r<7)]=1&f,o[(t-r)*t+8]=1&f},E=({size:t,i:o},r=0,e=0)=>{for(let n=0;n<t;++n){for(let f=0;f<2;++f)for(let s,l=0,c=0,a=2;l<t;++l){const i=1&o[f?n*t+l:l*t+n];e+=i,c=(c>>1|2098176)&(3047517^i-1),2049&c&&(r+=40),i^a&&(s=0),a=i,r+=5===++s?3:s>5}if(n)for(let e=t+n,f=5*o[n-1]^o[n];e<t*t;e+=t){const t=5*o[e-1]^o[e];r+=3*!(1&(f|t)|4&(f^t)),f=t}}return r+10*(10*Math.abs(e/(t*t)-1)|0)},h=[],m=(t=n(1),{minCorrectionLevel:r=0,minVersion:l=1}={})=>{\"string\"!=typeof t&&n(5),t=(new TextEncoder).encode(t);const c=e(2957);c.set([113,164,t.length>>8]);for(let e=l;e<41;++e){let n=h[e];n||(h[e]=n=f(4*e+17),z(n,e),n.p=g(n));const l=o(e,n.p.length>>3)(r);if(l.t>=3+(e>9)+t.length){let o=e>9?3:2;for(c[o++]=t.length,c.set(t,o),o+=t.length-1;o<2954;c.set([236,17],o+=2));const a=f(n.size,n.i);return w(a,n.p,_(c,l)),s.map((t,o)=>{const e=f(a.size,a.i);return y(e,t,o,r),e.s=E(e),e}).sort((t,o)=>t.s-o.s)[0]}}n(4)};window._leanqr={correction:r,generate:m};"
"let wxPollId=null;"
"function qrGen(u){try{let q=window._leanqr.generate(u);q.toCanvas(document.getElementById('wx_qr_cv'),{on:[0,0,0],off:[255,255,255],pad:2})}catch(e){console.warn('QR gen failed:',e)}}"
"async function onWxLogin(){"
"document.getElementById('btn_wx_login').disabled=true;"
"document.getElementById('btn_wx_cancel').style.display='block';"
"document.getElementById('wx_status').textContent='Starting QR login...';"
"try{let r=await fetch('/api/wechat/login/start',{method:'POST'});"
"let j=await r.json();"
"if(j.qr_data_url){"
"qrGen(j.qr_data_url);"
"document.getElementById('wx_qr_area').style.display='block';"
"document.getElementById('wx_status').textContent='Scan QR with WeChat';"
"wxPollId=setInterval(pollWxStatus,2000)"
"}else{document.getElementById('wx_status').textContent='Failed to get QR';"
"document.getElementById('btn_wx_login').disabled=false;"
"document.getElementById('btn_wx_cancel').style.display='none'}}"
"catch(e){document.getElementById('wx_status').textContent='Network error';"
"document.getElementById('btn_wx_login').disabled=false;"
"document.getElementById('btn_wx_cancel').style.display='none'}}"
"async function pollWxStatus(){"
"try{let r=await fetch('/api/wechat/login/status');let j=await r.json();"
"if(j.status==='waiting_scan'){document.getElementById('wx_status').textContent='Waiting for scan...'}"
"else if(j.status==='scanned'){document.getElementById('wx_status').textContent='Scanned! Confirm on phone...'}"
"else if(j.status==='confirmed'&&j.completed){"
"clearInterval(wxPollId);wxPollId=null;"
"document.getElementById('wx_qr_area').style.display='none';"
"/* Auto-save token to NVS */"
"let s=await fetch('/api/wechat/login/persist',{method:'POST'});"
"let sj=await s.json();"
"document.getElementById('wx_status').textContent=sj.ok?'WeChat login saved! ✅':'Login done but save failed';"
"document.getElementById('btn_wx_login').disabled=false;"
"document.getElementById('btn_wx_cancel').style.display='none'}"
"else if(j.status==='expired'){clearInterval(wxPollId);wxPollId=null;"
"document.getElementById('wx_status').textContent='QR expired. Try again.';"
"document.getElementById('wx_qr_area').style.display='none';"
"document.getElementById('btn_wx_login').disabled=false;"
"document.getElementById('btn_wx_cancel').style.display='none'}}"
"catch(e){}}"
"async function onWxCancel(){"
"if(wxPollId){clearInterval(wxPollId);wxPollId=null}"
"await fetch('/api/wechat/login/cancel',{method:'POST'});"
"document.getElementById('wx_qr_area').style.display='none';"
"document.getElementById('wx_status').textContent='Cancelled';"
"document.getElementById('btn_wx_login').disabled=false;"
"document.getElementById('btn_wx_cancel').style.display='none'}"
"let wxLastConfigured=true;"
"async function checkWxSession(){"
"try{let r=await fetch('/api/wechat/login/status');let j=await r.json();"
"if(!j.configured&&wxLastConfigured){"
"document.getElementById('wx_status').textContent='⚠️ Session expired — re-scan QR to login';"
"document.getElementById('wx_status').style.color='#ff9800';"
"document.getElementById('btn_wx_login').disabled=false;"
"document.getElementById('btn_wx_cancel').style.display='none'}"
"else if(j.configured&&!wxLastConfigured){"
"document.getElementById('wx_status').textContent='WeChat connected ✅';"
"document.getElementById('wx_status').style.color='#4caf50'}"
"wxLastConfigured=j.configured}"
"catch(e){}}"
#endif
"async function loadLlmConfig(){"
"try{let r=await fetch('/api/llm/config');let j=await r.json();"
"if(j.provider)document.getElementById('llm_provider').value=j.provider;"
"if(j.has_api_key)document.getElementById('llm_key').placeholder='(saved)';"
"if(j.model)document.getElementById('llm_model').value=j.model;"
"if(j.base_url)document.getElementById('llm_url').value=j.base_url}"
"catch(e){}}"
"async function saveLlmConfig(){"
"let d={provider:document.getElementById('llm_provider').value,"
"api_key:document.getElementById('llm_key').value,"
"model:document.getElementById('llm_model').value,"
"base_url:document.getElementById('llm_url').value};"
"try{let r=await fetch('/api/llm/config',{method:'POST',"
"headers:{'Content-Type':'application/json'},body:JSON.stringify(d)});"
"let j=await r.json();"
"document.getElementById('llm_status').textContent=j.ok?'LLM config saved ✅':'Save failed';"
"document.getElementById('llm_key').placeholder='(saved)';"
"document.getElementById('llm_key').value=''}"
"catch(e){document.getElementById('llm_status').textContent='Network error'}}"

/* ── Agent Chat Functions ── */
"function appendChat(role,text){"
"let log=document.getElementById('chat_log');"
"let d=document.createElement('div');"
"d.style.marginBottom='8px';"
"if(role==='user'){d.innerHTML='<span style=\"color:#00d4ff;font-weight:bold\">You:</span> '+text}"
"else if(role==='tool'){d.innerHTML='<span style=\"color:#ffa500;font-weight:bold\">Tool:</span> '+text}"
"else{d.innerHTML='<span style=\"color:#4caf50;font-weight:bold\">Agent:</span> '+text}"
"log.appendChild(d);log.scrollTop=log.scrollHeight}"
"async function sendChat(){"
"let input=document.getElementById('chat_input');"
"let msg=input.value.trim();if(!msg)return;"
"input.value='';"
"appendChat('user',msg);"
"document.getElementById('chat_status').textContent='Thinking...';"
"try{"
"let r=await fetch('/api/agent/chat',{method:'POST',"
"headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({message:msg})});"
"let j=await r.json();"
"if(j.reply)appendChat('agent',j.reply.replace(/</g,'&lt;'));"
"if(j.tool_calls&&j.tool_calls.length>0){"
"j.tool_calls.forEach(t=>appendChat('tool',t.name+' → '+t.result.replace(/</g,'&lt;')))}"
"document.getElementById('chat_status').textContent=j.ok?'':'Error: '+(j.error||'unknown')}"
"catch(e){document.getElementById('chat_status').textContent='Network error'}}"

"</script></body></html>";

/*============================================================================
 * WiFi Event Handler (used by settings_handler for connection verification)
 *============================================================================*/
/* WiFi state from uORB — published by PhoneAppSettings */
static orb_sub_t s_wifi_state_sub = -1;

/* SNTP initialized flag — prevent double init */
static std::atomic<bool> s_sntp_initialized{false};

/* SNTP synced flag — set by callback (lwIP task), consumed by web_config_task.
 * ulog_writer_start() does heavy file I/O (statvfs, opendir, write, xTaskCreate)
 * that overflows the lwIP tcpip task's 3KB stack, so the actual start is
 * deferred to the web_config_task loop (8KB stack). */
static std::atomic<bool> s_sntp_synced{false};

/* Start SNTP and register lightweight callback to signal time sync.
 * Called once when WiFi first gets an IP address. */
static void sntp_start_and_ulog_autostart(void)
{
    if (s_sntp_initialized.load(std::memory_order_acquire)) return;

    /* If SNTP was already initialized elsewhere (shouldn't happen after we
     * removed it from PhoneAppSettings, but defensive), just mark synced
     * if time is already available and let the main loop handle ULog start. */
    if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        s_sntp_synced.store(true, std::memory_order_release);
        s_sntp_initialized.store(true, std::memory_order_release);
        ulog_writer_set_wall_clock(ulog_writer_get(), true);
        return;
    }
    if (esp_sntp_get_sync_status() != SNTP_SYNC_STATUS_RESET) {
        /* SNTP init already done but not yet synced — register our callback */
        s_sntp_initialized.store(true, std::memory_order_release);
        esp_sntp_set_time_sync_notification_cb([](struct timeval *tv) {
            struct tm tm;
            localtime_r(&tv->tv_sec, &tm);
            ESP_LOGI(TAG, "SNTP synchronized: %04d-%02d-%02d %02d:%02d:%02d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
            ulog_writer_set_wall_clock(ulog_writer_get(), true);
            s_sntp_synced.store(true, std::memory_order_release);
        });
        return;
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb([](struct timeval *tv) {
        struct tm tm;
        localtime_r(&tv->tv_sec, &tm);
        ESP_LOGI(TAG, "SNTP synchronized: %04d-%02d-%02d %02d:%02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        /* Only set wall-clock flag + signal main loop.
         * Do NOT call ulog_writer_start() here — it does heavy file I/O
         * (statvfs, opendir, write, xTaskCreate) that overflows the
         * lwIP tcpip task's 3KB stack. The web_config_task loop
         * (8KB stack) will handle the actual start. */
        ulog_writer_set_wall_clock(ulog_writer_get(), true);
        s_sntp_synced.store(true, std::memory_order_release);
    });
    esp_sntp_init();
    s_sntp_initialized.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "SNTP started — wall-clock time will sync shortly");
}

static bool wifi_sta_is_connected(void)
{
    /* Check uORB wifi_state first */
    if (s_wifi_state_sub >= 0) {
        bool updated = false;
        struct wifi_state_s ws = {};
        if (orb_check(s_wifi_state_sub, &updated) == 0 && updated) {
            orb_copy(ORB_ID(wifi_state), s_wifi_state_sub, &ws);
            return ws.connected;
        }
    }
    /* Fallback: direct netif IP check.
     * esp_wifi_sta_get_ap_info() is NOT used here because it can return
     * stale AP info even after disconnection — the IP address is the
     * definitive connectivity indicator. */
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(sta, &ip) != ESP_OK) return false;
    return ip.ip.addr != 0;
}

/*============================================================================
 * HTTP Handlers
 *============================================================================*/

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, WEB_UI_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char ssid[33] = {};
    char pass[65] = {};
    nvs_get_str(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid));
    nvs_get_str(NVS_KEY_WIFI_PASS, pass, sizeof(pass));
    int32_t volume  = nvs_get_i32_def(NVS_KEY_VOLUME, VOLUME_DEFAULT);
    int32_t cam_en = nvs_get_i32_def(NVS_KEY_CAM_STREAM, 0);
    bool cam_running = CameraStream::instance().isRunning();

    /* Build JSON safely with cJSON — avoids injection from SSID special chars */
    cJSON *root = cJSON_CreateObject();
    if (!root) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    /* WiFi is always enabled — no wifi_en field needed */
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddBoolToObject(root, "has_pass", strlen(pass) > 0);  /* Never expose plaintext password */
    cJSON_AddNumberToObject(root, "volume", volume);
    cJSON_AddNumberToObject(root, "cam_stream", cam_en);
    cJSON_AddBoolToObject(root, "cam_running", cam_running);
    cJSON_AddBoolToObject(root, "cam_recording", CameraStream::instance().is_recording());

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (json_str) {
        httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
        cJSON_free(json_str);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON build failed");
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t settings_handler(httpd_req_t *req)
{
    char buf[512];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *j_ssid    = cJSON_GetObjectItem(root, "ssid");
    cJSON *j_pass    = cJSON_GetObjectItem(root, "pass");
    cJSON *j_volume  = cJSON_GetObjectItem(root, "volume");

    /* Log old vs new values for debugging */
    {
        int32_t old_volume  = nvs_get_i32_def(NVS_KEY_VOLUME, -1);
        char old_ssid[33] = {}; nvs_get_str(NVS_KEY_WIFI_SSID, old_ssid, sizeof(old_ssid));
        char old_pass[65] = {}; nvs_get_str(NVS_KEY_WIFI_PASS, old_pass, sizeof(old_pass));

        const char *new_ssid = (j_ssid && cJSON_IsString(j_ssid) && j_ssid->valuestring)
                               ? j_ssid->valuestring : old_ssid;
        const char *new_pass = (j_pass && cJSON_IsString(j_pass) && j_pass->valuestring)
                               ? j_pass->valuestring : old_pass;
        int32_t new_volume = j_volume ? j_volume->valueint : old_volume;
        if (new_volume < VOLUME_MIN) new_volume = VOLUME_MIN;
        if (new_volume > VOLUME_MAX) new_volume = VOLUME_MAX;

        ESP_LOGI(TAG, "Settings requested: ssid=\"%s\"->\"%s\", "
                 "pass_len=%d->%d, volume=%ld->%ld",
                 old_ssid, new_ssid,
                 (int)strlen(old_pass), (int)strlen(new_pass),
                 old_volume, new_volume);
    }

    /* Volume: save to NVS AND apply to hardware immediately */
    if (j_volume) {
        int32_t vol = j_volume->valueint;
        if (vol < VOLUME_MIN) vol = VOLUME_MIN;
        if (vol > VOLUME_MAX) vol = VOLUME_MAX;
        nvs_write_i32(NVS_KEY_VOLUME, vol);
        /* Apply to codec immediately (thread-safe via PeripheralManager) */
        PeripheralManager::instance().set_volume((int)vol);
    }

    /* WiFi sub-setting field validation (rule #1): SSID AND password must
     * BOTH be present and non-empty. If the request carries any WiFi field
     * (ssid/pass) but either is empty/missing, skip the ENTIRE WiFi NVS
     * update. Open networks are intentionally NOT supported — both fields
     * are required. A volume-only request (no WiFi fields) leaves WiFi
     * untouched. */
    bool skip_wifi = false;
    bool wifi_intended = (j_ssid != nullptr) || (j_pass != nullptr);
    if (wifi_intended) {
        bool ssid_ok = j_ssid && cJSON_IsString(j_ssid) && j_ssid->valuestring &&
                       strlen(j_ssid->valuestring) > 0;
        bool pass_ok = j_pass && cJSON_IsString(j_pass) && j_pass->valuestring &&
                       strlen(j_pass->valuestring) > 0;
        if (!ssid_ok || !pass_ok) {
            ESP_LOGW(TAG, "WiFi SSID or password empty/missing — skipping whole WiFi settings NVS update");
            skip_wifi = true;
        }
    }

    /* WiFi: try connecting first, save to NVS only on successful connection
     * (rule #2). WiFi is always enabled — no wifi_en check needed. */
    bool wifi_ok = true;  /* default true if no WiFi change */
    if (skip_wifi && wifi_intended) wifi_ok = false;
    bool need_reconnect = !skip_wifi
                          && j_ssid && cJSON_IsString(j_ssid) && j_ssid->valuestring
                          && strlen(j_ssid->valuestring) > 0
                          && j_pass && cJSON_IsString(j_pass) && j_pass->valuestring
                          && strlen(j_pass->valuestring) > 0;
    if (need_reconnect) {
        const char *target_ssid = j_ssid->valuestring;
        const char *target_pass = (j_pass && cJSON_IsString(j_pass) && j_pass->valuestring)
                                   ? j_pass->valuestring : "";
        ESP_LOGI(TAG, "Trying WiFi connection to %s before saving...", target_ssid);

        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));

        wifi_config_t wifi_cfg = {};
        size_t slen = strlen(target_ssid);
        if (slen > sizeof(wifi_cfg.sta.ssid) - 1)
            slen = sizeof(wifi_cfg.sta.ssid) - 1;
        memcpy(wifi_cfg.sta.ssid, target_ssid, slen);
        if (target_pass && strlen(target_pass) > 0) {
            slen = strlen(target_pass);
            if (slen > sizeof(wifi_cfg.sta.password) - 1)
                slen = sizeof(wifi_cfg.sta.password) - 1;
            memcpy(wifi_cfg.sta.password, target_pass, slen);
        }
        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        esp_wifi_connect();

        /* Wait for connection result via uORB (published by PhoneAppSettings::wifiEventHandler) */
        bool wifi_connected = false;
        int timeout_ms = WIFI_CONNECT_TIMEOUT_MS;
        while (timeout_ms > 0) {
            if (s_wifi_state_sub >= 0) {
                bool updated = false;
                struct wifi_state_s ws = {};
                if (orb_check(s_wifi_state_sub, &updated) == 0 && updated) {
                    orb_copy(ORB_ID(wifi_state), s_wifi_state_sub, &ws);
                    if (ws.connected) {
                        wifi_connected = true;
                        break;
                    }
                }
            }
            /* Fallback direct check */
            if (wifi_sta_is_connected()) { wifi_connected = true; break; }
            vTaskDelay(pdMS_TO_TICKS(200));
            timeout_ms -= 200;
        }

        if (wifi_connected) {
            ESP_LOGI(TAG, "WiFi connected to %s — saving to NVS", target_ssid);
            nvs_set_str_def(NVS_KEY_WIFI_SSID, target_ssid);
            if (target_pass && strlen(target_pass) > 0)
                nvs_set_str_def(NVS_KEY_WIFI_PASS, target_pass);
            wifi_ok = true;
        } else {
            ESP_LOGW(TAG, "WiFi connect to %s timed out — NOT saving to NVS", target_ssid);
            wifi_ok = false;
            /* Reconnect to old credentials if any */
            char old_ssid[33] = {}; nvs_get_str(NVS_KEY_WIFI_SSID, old_ssid, sizeof(old_ssid));
            if (strlen(old_ssid) > 0) {
                ESP_LOGI(TAG, "Reconnecting to previous SSID: %s", old_ssid);
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(500));
                wifi_config_t old_cfg = {};
                strlcpy((char *)old_cfg.sta.ssid, old_ssid, sizeof(old_cfg.sta.ssid));
                char old_pass[65] = {}; nvs_get_str(NVS_KEY_WIFI_PASS, old_pass, sizeof(old_pass));
                if (strlen(old_pass) > 0)
                    strlcpy((char *)old_cfg.sta.password, old_pass, sizeof(old_cfg.sta.password));
                esp_wifi_set_config(WIFI_IF_STA, &old_cfg);
                esp_wifi_connect();
            }
        }
    }

    cJSON_Delete(root);

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":1,\"wifi_connected\":%s}",
             wifi_ok ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t camera_stream_handler(httpd_req_t *req)
{
    char buf[128];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *j_enable = cJSON_GetObjectItem(root, "enable");
    bool enable = (j_enable && j_enable->valueint != 0);

    if (enable) {
        /* Require WiFi connected to start camera stream */
        if (!wifi_sta_is_connected()) {
            const char *resp = "{\"ok\":0,\"error\":\"WiFi not connected\"}";
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
            cJSON_Delete(root);
            return ESP_OK;
        }
        if (!CameraStream::instance().isRunning()) {
            ESP_LOGI(TAG, "Starting camera stream...");
            CameraStream::instance().start();
            /* start() auto-enables recording on success; if it failed,
             * isRunning() is false — don't set recording on a dead stream */
        }
        /* For an already-running stream, enable recording explicitly */
        if (CameraStream::instance().isRunning()) {
            CameraStream::instance().set_recording(true);
        }
    } else {
        /* Auto-disable camera frame recording when stream stops */
        CameraStream::instance().set_recording(false);
        if (CameraStream::instance().isRunning()) {
            ESP_LOGI(TAG, "Stopping camera stream...");
            CameraStream::instance().stop();
        }
    }

    /* Persist intent to NVS */
    nvs_write_i32(NVS_KEY_CAM_STREAM, enable ? 1 : 0);

    cJSON_Delete(root);

    bool running = CameraStream::instance().isRunning();
    bool recording = CameraStream::instance().is_recording();
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"ok\":1,\"enabled\":%s,\"running\":%s,\"recording\":%s}",
             enable ? "true" : "false", running ? "true" : "false", recording ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Factory reset requested! Erasing NVS settings...");

    /* Invalidate RAM cache — all cached values are stale */
    if (s_nvs_cache_mutex) xSemaphoreTake(s_nvs_cache_mutex, portMAX_DELAY);
    for (int i = 0; i < s_nvs_cache_count; i++) {
        s_nvs_cache[i].valid = false;
    }
    s_nvs_cache_count = 0;
    if (s_nvs_cache_mutex) xSemaphoreGive(s_nvs_cache_mutex);

    /* Erase all keys in the "settings" namespace */
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_all(h);
        if (err == ESP_OK) {
            nvs_commit(h);
            ESP_LOGI(TAG, "All keys in namespace '%s' erased", NVS_NAMESPACE);
        } else {
            ESP_LOGE(TAG, "nvs_erase_all failed: %s", esp_err_to_name(err));
        }
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "Namespace '%s' not found — nothing to erase", NVS_NAMESPACE);
    }

    /* Respond to the client before reboot so they see confirmation */
    const char *resp = "{\"ok\":1,\"message\":\"Settings erased, rebooting...\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    /* Give the HTTP response time to flush */
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_restart();
    /* Note: esp_restart() does not return. The return below is kept
     * for compiler satisfaction but is never reached. */
    return ESP_OK;
}

/*============================================================================
 * Audio Handlers — record / play / list mp3 files on SD card
 *============================================================================*/

/* Lazy-init SD card + audio codec on headless boards */
static bool __audio_init(void) {
    if (s_audio_inited.load(std::memory_order_acquire)) return true;
    if (!PeripheralManager::instance().init_sdcard()) { ESP_LOGE(TAG,"SD init fail"); return false; }
    PeripheralManager::instance().init_audio();
    /* Pre-allocate TCB for audio task — reused across start/stop cycles.
     * ~340B internal SRAM, avoids TCB use-after-free race with idle task. */
    if (!s_audio_tcb) {
        s_audio_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_audio_tcb) { ESP_LOGE(TAG, "Failed to allocate audio TCB"); return false; }
    }
    s_audio_inited.store(true, std::memory_order_release);
    return true;
}

/* Lazy-init SD card only (no audio) — used by file manager */
static bool __sd_ensure(void) {
    if (SDCardDriver::instance().available()) return true;
    return SDCardDriver::instance().init();
}

/* Sanitize a user-supplied path to ensure it stays within /sdcard.
 * Resolves ".." and symlinks, strips trailing slashes.
 * Returns the normalized absolute path in `out` (max `out_len` bytes).
 * Returns true if the path is safe (under /sdcard/), false if rejected. */
static bool __path_sanitize(const char *user_path, char *out, size_t out_len) {
    if (!user_path || !out || out_len == 0) return false;
    out[0] = '\0';

    /* Reject empty path */
    if (user_path[0] == '\0') return false;

    /* Prepend /sdcard unless path already starts with /sdcard */
    char full[256];
    if (strncmp(user_path, "/sdcard", 7) == 0 && (user_path[7] == '\0' || user_path[7] == '/')) {
        snprintf(full, sizeof(full), "%s", user_path);
    } else if (user_path[0] == '/') {
        snprintf(full, sizeof(full), "/sdcard%s", user_path);
    } else {
        snprintf(full, sizeof(full), "/sdcard/%s", user_path);
    }

    /* Strip trailing slashes (but preserve root "/sdcard") */
    size_t len = strlen(full);
    while (len > 1 && full[len - 1] == '/') {
        full[--len] = '\0';
    }
    if (len == 0 || (len == 1 && full[0] == '/')) {
        snprintf(full, sizeof(full), "/sdcard");
        len = strlen(full);
    }

    /* Reject ".." as a path segment (not just substring).
     * Checks each segment: if segment equals ".." (not ".hidden" or "abc..def") */
    {
        const char *p = full;
        while (*p) {
            if (*p == '/') p++;
            if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
                ESP_LOGW(TAG, "[FM] sanitize: rejected \"..\" segment in \"%s\"", full);
                return false;
            }
            while (*p && *p != '/') p++;
        }
    }

    /* Ensure path starts with /sdcard/ or is exactly /sdcard */
    if (strncmp(full, "/sdcard", 7) != 0) {
        ESP_LOGW(TAG, "[FM] sanitize: \"%s\" does not start with /sdcard", full);
        return false;
    }
    if (full[7] != '\0' && full[7] != '/') {
        ESP_LOGW(TAG, "[FM] sanitize: \"%s\" has invalid prefix", full);
        return false;
    }

    strlcpy(out, full, out_len);
    ESP_LOGI(TAG, "[FM] sanitize: \"%s\" -> \"%s\"", user_path, out);
    return true;
}

/* GET /api/audio/record_start */
static esp_err_t h_rec_start(httpd_req_t *req) {
    audio_lock();
    if (s_is_recording)   { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":1}"); return ESP_OK; }
    if (s_fm_busy)        { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"File manager busy\"}"); return ESP_OK; }
    if (!__audio_init())  { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Init fail\"}"); return ESP_OK; }
    if (!s_audio_task.load(std::memory_order_acquire)) {
        s_audio_running = true;
        s_audio_stack = (StackType_t *)heap_caps_malloc(12 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_audio_stack) {
            ESP_LOGE(TAG, "Failed to allocate audio task stack");
            s_audio_running = false; audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK;
        }
        TaskHandle_t h = xTaskCreateStaticPinnedToCore(audio_task, "w_audio", 12 * 1024, NULL, 1, s_audio_stack, s_audio_tcb, 1);  /* Core 1 (mutually exclusive with Music) */
        s_audio_task.store(h, std::memory_order_release);
        if (h == NULL) {
            ESP_LOGE(TAG, "Failed to create audio task");
            heap_caps_free(s_audio_stack); s_audio_stack = NULL;
            s_audio_running = false; audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK;
        }
    }
    s_pcm_buf = (int16_t*)heap_caps_calloc(1, PCM_BUF_SAMPLES*sizeof(int16_t), MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s_pcm_buf) {
        _stop_audio_task_if_running();
        audio_unlock();
        httpd_resp_sendstr(req, "{\"ok\":0}");
        return ESP_OK;
    }
    s_pcm_count.store(0, std::memory_order_relaxed);
    shine_config_t c; shine_set_config_mpeg_defaults(&c.mpeg);
    c.wave.channels = PCM_STEREO; c.wave.samplerate = 48000; c.mpeg.mode = STEREO; c.mpeg.bitr = 128;
    s_shine = shine_initialise(&c);
    if (!s_shine) {
        heap_caps_free(s_pcm_buf);
        s_pcm_buf = NULL;
        _stop_audio_task_if_running();
        audio_unlock();
        httpd_resp_sendstr(req, "{\"ok\":0}");
        return ESP_OK;
    }
    struct timeval tv; gettimeofday(&tv, NULL);
    time_t t = tv.tv_sec; struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    /* Guard against unsynced wall-clock (epoch year < 2020 → overwrite) */
    if (tm_buf.tm_year + 1900 > 2020) {
        snprintf(s_rec_path, sizeof(s_rec_path), SDMMC_MOUNT_POINT "/rec_%04d%02d%02d_%02d%02d%02d.mp3",
                 tm_buf.tm_year+1900, tm_buf.tm_mon+1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    } else {
        uint32_t mono_ms = (uint32_t)(esp_timer_get_time() / 1000);
        snprintf(s_rec_path, sizeof(s_rec_path), SDMMC_MOUNT_POINT "/rec_%lu.mp3", (unsigned long)mono_ms);
    }
    s_rec_file = fopen(s_rec_path, "wb");
    if (!s_rec_file) { shine_close(s_shine); s_shine = NULL; heap_caps_free(s_pcm_buf); s_pcm_buf = NULL;
        _stop_audio_task_if_running(); audio_unlock();
        httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK; }
    s_rec_bytes = 0; s_rec_start_ms.store((uint32_t)(esp_timer_get_time() / 1000), std::memory_order_relaxed);
    s_is_recording = true;

    /* Stop any active web playback — recording and playback share I2S hardware.
     * destroy() internally waits for GMF task to reach STOPPED state. */
    if (s_asp && s_playing) {
        esp_asp_handle_t old_asp = s_asp;
        s_asp = NULL;
        s_playing = false;
        audio_unlock();
        esp_audio_simple_player_stop(old_asp);
        esp_audio_simple_player_destroy(old_asp);
        audio_lock();
        ESP_LOGI(TAG, "Stopped web playback for recording");
    }

    /* Publish recording_state.active=true so PhoneAppMusic can stop its playback */
    orb_advert_t pub = s_rec_pub.load(std::memory_order_acquire);
    if (pub < 0) {
        pub = orb_advertise(ORB_ID(recording_state));
        s_rec_pub.store(pub, std::memory_order_release);
    }
    if (pub >= 0) {
        struct recording_state_s rs = {};
        rs.timestamp = esp_timer_get_time();
        rs.active = true;
        rs.bytes_written = 0;
        rs.elapsed_ms = 0;
        orb_publish(ORB_ID(recording_state), pub, &rs);
    }

    audio_unlock();
    ESP_LOGI(TAG, "Recording: %s", s_rec_path);
    httpd_resp_sendstr(req, "{\"ok\":1}");
    return ESP_OK;
}

/* GET /api/audio/record_stop */
static esp_err_t h_rec_stop(httpd_req_t *req) {
    audio_lock();
    if (!s_is_recording) {
        /* Recover from stale state if a previous record_start failed mid-way. */
        _stop_audio_task_if_running();
        if (s_shine) { shine_close(s_shine); s_shine = NULL; }
        if (s_pcm_buf) { heap_caps_free(s_pcm_buf); s_pcm_buf = NULL; }
        if (s_rec_file) { fclose(s_rec_file); s_rec_file = NULL; }
        audio_unlock();
        httpd_resp_sendstr(req, "{\"ok\":1}");
        return ESP_OK;
    }
    s_is_recording = false;

    /* Publish recording_state.active=false so PhoneAppMusic can resume playback */
    {
        orb_advert_t pub = s_rec_pub.load(std::memory_order_acquire);
        if (pub >= 0) {
            struct recording_state_s rs = {};
            rs.timestamp = esp_timer_get_time();
            rs.active = false;
            rs.bytes_written = s_rec_bytes.load(std::memory_order_relaxed);
            rs.elapsed_ms = (uint32_t)((esp_timer_get_time() / 1000 - s_rec_start_ms.load(std::memory_order_relaxed)));
            orb_publish(ORB_ID(recording_state), pub, &rs);
        }
    }

    /* Stop audio task to release I2S RX before cleaning up resources.
     * _stop_audio_task_if_running() blocks up to 500ms waiting for the
     * audio task to exit — acceptable for an HTTP handler.
     * Keep the lock held throughout: preventing a concurrent h_rec_start
     * from creating a new recording between stopping and cleanup. */
    _stop_audio_task_if_running();
    FILE *f = s_rec_file; s_rec_file = NULL;
    if (s_shine) { int wr=0; unsigned char *d=shine_flush(s_shine, &wr); if(d&&wr>0&&f) fwrite(d,1,wr,f); shine_close(s_shine); s_shine=NULL; }
    if (f) fclose(f);
    if (s_pcm_buf) { heap_caps_free(s_pcm_buf); s_pcm_buf=NULL; }
    char saved_path[128];
    strlcpy(saved_path, s_rec_path, sizeof(saved_path));
    uint32_t saved_bytes = s_rec_bytes.load(std::memory_order_relaxed);
    audio_unlock();
    char r[256]; snprintf(r,sizeof(r),"{\"ok\":1,\"file\":\"%s\",\"bytes\":%lu}", saved_path, (unsigned long)saved_bytes);
    ESP_LOGI(TAG,"Saved: %s (%lu)", saved_path, (unsigned long)saved_bytes);
    httpd_resp_send(req, r, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /api/audio/record_status */
static esp_err_t h_rec_status(httpd_req_t *req) {
    char r[128];
    if (s_is_recording) {
        /* Take audio mutex to get consistent snapshot of recording state.
         * s_rec_bytes is updated by the recording task via fetch_add
         * and read here from the httpd task — atomic ensures no data race. */
        audio_lock();
        uint32_t bytes = s_rec_bytes.load(std::memory_order_relaxed);
        uint32_t start_ms = s_rec_start_ms.load(std::memory_order_relaxed);
        audio_unlock();
        uint32_t e = (uint32_t)((esp_timer_get_time() / 1000 - start_ms) / 1000);
        snprintf(r,sizeof(r),"{\"recording\":1,\"seconds\":%lu,\"bytes\":%lu}",(unsigned long)e,(unsigned long)bytes);
    } else snprintf(r,sizeof(r),"{\"recording\":0}");
    httpd_resp_send(req, r, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /api/audio/list */
static esp_err_t h_list(httpd_req_t *req) {
    /* Only need SD card for file listing, not audio codec.
     * Using __sd_ensure() avoids claiming I2S resources that conflict
     * with CameraStream's MJPEG streaming. */
    if (!__sd_ensure()) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    DIR *d = opendir(SDMMC_MOUNT_POINT);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        if (d) closedir(d);
        if (arr) cJSON_Delete(arr);
        if (root) cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    cJSON_AddItemToObject(root, "files", arr);
    if(d){ struct dirent *e; while((e=readdir(d))){ if(e->d_name[0]=='.') continue; char *x=strrchr(e->d_name,'.'); if(x&&strcasecmp(x,".mp3")==0) cJSON_AddItemToArray(arr,cJSON_CreateString(e->d_name)); } closedir(d); }
    char *j = cJSON_PrintUnformatted(root);
    if (!j) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, j, strlen(j));
    cJSON_free(j);
    cJSON_Delete(root);
    return ESP_OK;
}

/* URL-decode %XX sequences in-place.
 * Validates hex digits: non-hex characters after '%' are left as-is. */
static int _hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = c & 0xDF;  /* toupper */
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;     /* not a hex digit */
}

static void _url_decode(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '%' && r[1] && r[2]) {
            int hi = _hex_digit(r[1]);
            int lo = _hex_digit(r[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
            } else {
                /* Invalid hex — copy '%' literally and continue */
                *w++ = *r++;
            }
        } else *w++ = *r++;
    }
    *w = '\0';
}

/* Playback callbacks */
static int _asp_out(uint8_t *d, int sz, void *_) { (void)_;
    return PeripheralManager::instance().codec_write(d, sz); }
static int _asp_evt(esp_asp_event_pkt_t *pkt, void *_) { (void)_;
    if(pkt->type==ESP_ASP_EVENT_TYPE_STATE){ int s=*(esp_asp_state_t*)pkt->payload;
        if(s==ESP_ASP_STATE_FINISHED||s==ESP_ASP_STATE_STOPPED||s==ESP_ASP_STATE_ERROR) s_playing=false; } return 0; }

/* GET /api/audio/play?file=xxx.mp3 */
static esp_err_t h_play(httpd_req_t *req) {
    audio_lock();
    if(!__audio_init()){ audio_unlock(); httpd_resp_sendstr(req,"{\"ok\":0,\"error\":\"Init fail\"}"); return ESP_OK; }
    char q[256]={},fn[128]={};
    if(httpd_req_get_url_query_str(req,q,sizeof(q))!=ESP_OK||!strlen(q)){ audio_unlock(); httpd_resp_sendstr(req,"{\"ok\":0}"); return ESP_OK; }
    httpd_query_key_value(q,"file",fn,sizeof(fn));
    if(!strlen(fn)){ audio_unlock(); httpd_resp_sendstr(req,"{\"ok\":0}"); return ESP_OK; }
    _url_decode(fn);
    char safe[256];
    if (!__path_sanitize(fn, safe, sizeof(safe))) {
        audio_unlock();
        httpd_resp_sendstr(req,"{\"ok\":0,\"error\":\"Invalid path\"}");
        return ESP_OK;
    }
    char uri[300]; snprintf(uri,sizeof(uri),"file://%s",safe);
    if (s_fm_busy) { audio_unlock(); httpd_resp_sendstr(req,"{\"ok\":0,\"error\":\"File manager busy\"}"); return ESP_OK; }
    /* Refuse playback while web recording is active — recording and playback share I2S hardware */
    if (s_is_recording) { audio_unlock(); httpd_resp_sendstr(req,"{\"ok\":0,\"error\":\"Recording in progress\"}"); return ESP_OK; }
    /* Stop + destroy previous player for clean state (matching Music App lifecycle).
     * Release audio_lock during stop/destroy to avoid blocking other audio handlers
     * and to prevent deadlock if GMF output callback needs codec access.
     * destroy() internally waits for GMF task to reach STOPPED state. */
    if (s_asp) {
        esp_asp_handle_t old_asp = s_asp;
        s_asp = NULL;
        s_playing = false;
        audio_unlock();
        esp_audio_simple_player_stop(old_asp);
        esp_audio_simple_player_destroy(old_asp);
        audio_lock();
    }
    s_playing = false;
    esp_asp_cfg_t c={.out={.cb=_asp_out},.task_prio=3,.task_stack=8192,.task_core=1,.task_stack_in_ext=true};
    if(esp_audio_simple_player_new(&c,&s_asp)!=ESP_GMF_ERR_OK||!s_asp){ audio_unlock(); httpd_resp_sendstr(req,"{\"ok\":0}"); return ESP_OK; }
    esp_audio_simple_player_set_event(s_asp,_asp_evt,NULL);
    esp_gmf_err_t ret = esp_audio_simple_player_run(s_asp, uri, NULL);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Play failed: %d, uri=%s", ret, uri);
        esp_audio_simple_player_destroy(s_asp);
        s_asp = NULL;
        audio_unlock();
        httpd_resp_sendstr(req,"{\"ok\":0}"); return ESP_OK;
    }
    s_playing=true; audio_unlock(); ESP_LOGI(TAG,"Play: %s",uri);
    httpd_resp_sendstr(req,"{\"ok\":1}"); return ESP_OK;
}

/* GET /api/audio/stop */
static esp_err_t h_stop(httpd_req_t *req) {
    audio_lock();
    if(s_asp){ esp_audio_simple_player_stop(s_asp); s_playing=false; }
    audio_unlock();
    httpd_resp_sendstr(req,"{\"ok\":1}"); return ESP_OK;
}

/*============================================================================
 * File Manager Handlers — list / download / delete files on SD card
 *============================================================================*/

/* GET /api/files/list?dir=/ */
static esp_err_t h_files_list(httpd_req_t *req) {
    ESP_LOGI(TAG, "[FM] list request received");

    if (!__sd_ensure()) {
        ESP_LOGW(TAG, "[FM] list: SD card not available");
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"SD card not available\"}");
        return ESP_OK;
    }

    char q[256] = {};
    char raw_dir[128] = "/";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && strlen(q) > 0) {
        httpd_query_key_value(q, "dir", raw_dir, sizeof(raw_dir));
    }
    if (raw_dir[0] == '\0') {
        strlcpy(raw_dir, "/", sizeof(raw_dir));
    }
    _url_decode(raw_dir);  /* httpd_query_key_value does NOT decode %XX */
    ESP_LOGI(TAG, "[FM] list: raw_dir=\"%s\"", raw_dir);

    char dir[256];
    if (!__path_sanitize(raw_dir, dir, sizeof(dir))) {
        ESP_LOGW(TAG, "[FM] list: invalid path \"%s\"", raw_dir);
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Invalid path\"}");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "[FM] list: sanitized dir=\"%s\"", dir);

    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "[FM] list: opendir(\"%s\") failed", dir);
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Cannot open directory\"}");
        return ESP_OK;
    }

    /* Get filesystem capacity via fatfs (statvfs not supported on ESP-IDF FAT VFS) */
    uint64_t total_kb = 0, free_kb = 0;
    {
        DIR *d2 = opendir("/sdcard");
        if (d2) {
            /* fatfs f_getfree uses the drive associated with this DIR */
            FATFS *fs;
            DWORD free_clust;
            FRESULT fr = f_getfree("", &free_clust, &fs);
            if (fr == FR_OK && fs) {
                uint64_t tot = (fs->n_fatent - 2) * fs->csize;
                uint64_t fre = free_clust * fs->csize;
                total_kb = tot * fs->ssize / 1024;
                free_kb  = fre * fs->ssize / 1024;
                ESP_LOGI(TAG, "[FM] capacity: total=%llu MB, free=%llu MB", total_kb / 1024, free_kb / 1024);
            } else {
                ESP_LOGW(TAG, "[FM] f_getfree failed: %d", fr);
            }
            closedir(d2);
        } else {
            ESP_LOGW(TAG, "[FM] capacity: opendir(/sdcard) failed");
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "ok", 1);
    cJSON *files_arr = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "files", files_arr);

    /* Show current path relative to /sdcard */
    const char *display = dir;
    if (strncmp(dir, "/sdcard", 7) == 0) {
        display = dir + 7;  /* skip "/sdcard" */
        if (display[0] == '\0') display = "/";
    }
    cJSON_AddStringToObject(root, "current", display);
    cJSON_AddNumberToObject(root, "total_kb", (double)total_kb);
    cJSON_AddNumberToObject(root, "free_kb", (double)free_kb);

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;  /* skip . and .. */

        char fpath[512];
        snprintf(fpath, sizeof(fpath), "%s/%s", dir, e->d_name);

        struct stat st;
        bool is_dir = false;
        int64_t fsize = 0;
        time_t mtime = 0;
        if (stat(fpath, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            fsize = is_dir ? 0 : (int64_t)st.st_size;
            mtime = st.st_mtime;
        }

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", e->d_name);
        cJSON_AddBoolToObject(item, "is_dir", is_dir);
        cJSON_AddNumberToObject(item, "size", (double)fsize);
        cJSON_AddNumberToObject(item, "mtime", (double)mtime);
        cJSON_AddItemToArray(files_arr, item);
    }
    closedir(d);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (json) {
        ESP_LOGI(TAG, "[FM] list response: %d files, %zu bytes", cJSON_GetArraySize(files_arr), strlen(json));
        httpd_resp_send(req, json, strlen(json));
        cJSON_free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON build failed");
    }
    cJSON_Delete(root);
    return ESP_OK;
}

/* GET /api/files/download?path=xxx */
static esp_err_t h_files_download(httpd_req_t *req) {
    if (!__sd_ensure()) {
        httpd_resp_sendstr(req, "SD card not available");
        return ESP_OK;
    }

    /* Block concurrent audio recording/playback (TOCTOU-safe: check under mutex) */
    audio_lock();
    if (s_is_recording || s_playing) {
        audio_unlock();
        httpd_resp_sendstr(req, "Audio is active — stop recording/playback first");
        return ESP_OK;
    }
    audio_unlock();

    char q[256] = {}, raw[256] = {};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK || !strlen(q)) {
        httpd_resp_sendstr(req, "Missing path");
        return ESP_OK;
    }
    httpd_query_key_value(q, "path", raw, sizeof(raw));
    if (!strlen(raw)) {
        httpd_resp_sendstr(req, "Missing path");
        return ESP_OK;
    }
    _url_decode(raw);

    char fpath[320];
    if (!__path_sanitize(raw, fpath, sizeof(fpath))) {
        httpd_resp_sendstr(req, "Invalid path");
        return ESP_OK;
    }

    /* Only allow files, not directories */
    struct stat st;
    if (stat(fpath, &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_sendstr(req, "Not a file");
        return ESP_OK;
    }

    FILE *f = fopen(fpath, "rb");
    if (!f) {
        httpd_resp_sendstr(req, "Cannot open file");
        return ESP_OK;
    }

    /* Mark FM busy before streaming — prevents audio from starting mid-transfer */
    audio_lock();
    if (s_is_recording || s_playing) {
        audio_unlock();
        fclose(f);
        httpd_resp_sendstr(req, "Audio is active — stop recording/playback first");
        return ESP_OK;
    }
    s_fm_busy = true;
    audio_unlock();

    /* Get file size for Content-Length */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    /* Extract filename from path for Content-Disposition.
     * Sanitize double-quotes and backslashes to prevent HTTP header injection. */
    const char *fname = strrchr(fpath, '/');
    fname = fname ? fname + 1 : fpath;

    char safe_fname[256];
    {
        const char *src = fname;
        char *dst = safe_fname;
        const char *end = safe_fname + sizeof(safe_fname) - 1;
        while (*src && dst < end) {
            if (*src == '"' || *src == '\\') {
                /* Skip dangerous characters for HTTP header safety */
                src++;
            } else {
                *dst++ = *src++;
            }
        }
        *dst = '\0';
    }

    char disp[384];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", safe_fname);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_type(req, "application/octet-stream");

    if (fsize > 0) {
        char clen[32];
        snprintf(clen, sizeof(clen), "%ld", fsize);
        httpd_resp_set_hdr(req, "Content-Length", clen);
    }

    /* Stream file in chunks (heap-allocated to avoid httpd task stack overflow) */
    uint8_t *chunk = (uint8_t *)malloc(1024);
    if (!chunk) {
        fclose(f);
        s_fm_busy = false;
        httpd_resp_sendstr(req, "Out of memory");
        return ESP_OK;
    }
    size_t n;
    while ((n = fread(chunk, 1, 1024, f)) > 0) {
        if (httpd_resp_send_chunk(req, (const char *)chunk, (int)n) != ESP_OK) break;
    }
    free(chunk);
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(f);

    s_fm_busy = false;
    ESP_LOGI(TAG, "File downloaded: %s (%ld bytes)", fpath, fsize);
    return ESP_OK;
}

/* POST /api/files/delete — body: {"path": "xxx"} */
static esp_err_t h_files_delete(httpd_req_t *req) {
    if (!__sd_ensure()) {
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"SD card not available\"}");
        return ESP_OK;
    }

    /* Block concurrent audio recording/playback (TOCTOU-safe: check under mutex) */
    audio_lock();
    if (s_is_recording || s_playing) {
        audio_unlock();
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Audio is active\"}");
        return ESP_OK;
    }
    s_fm_busy = true;
    audio_unlock();

    char buf[512];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        s_fm_busy = false;
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Empty body\"}");
        return ESP_OK;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        s_fm_busy = false;
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Invalid JSON\"}");
        return ESP_OK;
    }

    cJSON *j_path = cJSON_GetObjectItem(root, "path");
    if (!j_path || !cJSON_IsString(j_path) || !j_path->valuestring) {
        s_fm_busy = false;
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Missing path\"}");
        cJSON_Delete(root);
        return ESP_OK;
    }

    char fpath[320];
    bool safe = __path_sanitize(j_path->valuestring, fpath, sizeof(fpath));
    cJSON_Delete(root);

    if (!safe) {
        s_fm_busy = false;
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Invalid path\"}");
        return ESP_OK;
    }

    struct stat st;
    if (stat(fpath, &st) != 0) {
        s_fm_busy = false;
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"File not found\"}");
        return ESP_OK;
    }

    /* Only delete regular files (not directories), for safety */
    if (S_ISDIR(st.st_mode)) {
        /* Allow deleting empty directories */
        if (rmdir(fpath) != 0) {
            s_fm_busy = false;
            httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Cannot delete directory (not empty?)\"}");
            return ESP_OK;
        }
    } else {
        if (unlink(fpath) != 0) {
            s_fm_busy = false;
            httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Delete failed\"}");
            return ESP_OK;
        }
    }

    ESP_LOGI(TAG, "File deleted: %s", fpath);
    s_fm_busy = false;
    httpd_resp_sendstr(req, "{\"ok\":1}");
    return ESP_OK;
}

/* POST /api/files/delete_batch — body: {"paths": ["xxx", "yyy"]} */
static esp_err_t h_files_delete_batch(httpd_req_t *req) {
    if (!__sd_ensure()) {
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"SD card not available\"}");
        return ESP_OK;
    }

    audio_lock();
    if (s_is_recording || s_playing) {
        audio_unlock();
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Audio is active\"}");
        return ESP_OK;
    }
    s_fm_busy = true;
    audio_unlock();

    /* Reject oversized request bodies upfront to avoid silent truncation */
    const size_t kMaxBody = 4096;
    if (req->content_len > kMaxBody) {
        s_fm_busy = false;
        char err[80];
        snprintf(err, sizeof(err), "{\"ok\":0,\"error\":\"Request too large (max %u bytes)\"}", (unsigned)kMaxBody);
        httpd_resp_sendstr(req, err);
        return ESP_OK;
    }

    /* Heap-allocate receive buffer to avoid httpd task stack overflow
     * (buf[4096] + fpath[320] + struct stat + __path_sanitize full[256] ≈ 5KB
     *  on an 8KB stack — same pattern as h_files_download) */
    char *buf = (char *)malloc(kMaxBody);
    if (!buf) {
        s_fm_busy = false;
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Out of memory\"}");
        return ESP_OK;
    }

    int received = httpd_req_recv(req, buf, kMaxBody - 1);
    if (received <= 0) {
        free(buf);
        s_fm_busy = false;
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Empty body\"}");
        return ESP_OK;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        s_fm_busy = false;
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Invalid JSON\"}");
        return ESP_OK;
    }

    cJSON *j_paths = cJSON_GetObjectItem(root, "paths");
    if (!j_paths || !cJSON_IsArray(j_paths)) {
        s_fm_busy = false;
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Missing paths array\"}");
        cJSON_Delete(root);
        return ESP_OK;
    }

    int deleted = 0, failed = 0;
    char fpath[320];

    cJSON *item;
    cJSON_ArrayForEach(item, j_paths) {
        if (!cJSON_IsString(item) || !item->valuestring) {
            failed++;
            continue;
        }
        bool safe = __path_sanitize(item->valuestring, fpath, sizeof(fpath));
        if (!safe) { failed++; continue; }

        struct stat st;
        if (stat(fpath, &st) != 0) { failed++; continue; }

        int ret = S_ISDIR(st.st_mode) ? rmdir(fpath) : unlink(fpath);
        if (ret == 0) {
            ESP_LOGI(TAG, "Batch deleted: %s", fpath);
            deleted++;
        } else {
            failed++;
        }
    }

    cJSON_Delete(root);
    s_fm_busy = false;

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":1,\"deleted\":%d,\"failed\":%d}", deleted, failed);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

/* ULog endpoints */
static esp_err_t ulog_status_handler(httpd_req_t *req)
{
    ulog_writer_t *ulog = ulog_writer_get();
    cJSON *root = cJSON_CreateObject();
    if (!root) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    cJSON_AddBoolToObject(root, "running", ulog_writer_get_state(ulog) == ULOG_STATE_RUNNING);
    const char *fp = ulog_writer_get_filepath(ulog);
    cJSON_AddStringToObject(root, "filepath", fp ? fp : "");
    cJSON_AddNumberToObject(root, "bytes_written", (double)ulog_writer_get_bytes_written(ulog));
    const char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    if (json) {
        httpd_resp_sendstr(req, json);
        cJSON_free((void *)json);
    } else {
        httpd_resp_sendstr(req, "{}");
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t ulog_start_handler(httpd_req_t *req)
{
    /* Ensure SD card is mounted */
    if (!SDCardDriver::instance().init()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"SD card not available\"}");
        return ESP_OK;
    }
    ulog_writer_t *ulog = ulog_writer_get();
    esp_err_t err = ulog_writer_start(ulog);
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":1}");
    } else {
        httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Start failed\"}");
    }
    return ESP_OK;
}

static esp_err_t ulog_stop_handler(httpd_req_t *req)
{
    ulog_writer_t *ulog = ulog_writer_get();
    ulog_writer_stop(ulog);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":1}");
    return ESP_OK;
}

/* GET /api/system_stats — current system performance snapshot */
static esp_err_t h_system_stats(httpd_req_t *req)
{
    system_stats_s stats = SystemMonitor::instance().get_latest();

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    cJSON *memory = cJSON_CreateObject();
    cJSON_AddNumberToObject(memory, "free_internal_kb", (double)(stats.free_internal / 1024));
    cJSON_AddNumberToObject(memory, "free_psram_kb", (double)(stats.free_psram / 1024));
    cJSON_AddNumberToObject(memory, "min_free_internal_kb", (double)(stats.min_free_internal / 1024));
    cJSON_AddNumberToObject(memory, "min_free_psram_kb", (double)(stats.min_free_psram / 1024));
    cJSON_AddItemToObject(root, "memory", memory);

    cJSON_AddNumberToObject(root, "task_count", (double)stats.task_count);
    cJSON_AddNumberToObject(root, "cpu_pct", (double)stats.total_cpu_pct / 100.0);
    cJSON_AddNumberToObject(root, "core0_cpu_pct", (double)stats.core0_cpu_pct / 100.0);
    cJSON_AddNumberToObject(root, "core1_cpu_pct", (double)stats.core1_cpu_pct / 100.0);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (json) {
        httpd_resp_sendstr(req, json);
        cJSON_free(json);
    } else {
        httpd_resp_sendstr(req, "{}");
    }
    cJSON_Delete(root);
    return ESP_OK;
}

/* GET /api/system_alerts — current alert state */
static cJSON *_build_alert_obj(const system_alert_s &a)
{
    cJSON *obj = cJSON_CreateObject();
    if (a.timestamp == 0) {
        cJSON_AddBoolToObject(obj, "active", false);
        return obj;
    }
    cJSON_AddBoolToObject(obj, "active", true);
    const char *type_str = "unknown";
    switch (a.alert_type) {
    case SYS_ALERT_CPU_HIGH:          type_str = "cpu_high"; break;
    case SYS_ALERT_MEM_INTERNAL_HIGH: type_str = "mem_internal_high"; break;
    case SYS_ALERT_MEM_PSRAM_HIGH:    type_str = "mem_psram_high"; break;
    default: break;
    }
    cJSON_AddStringToObject(obj, "type", type_str);
    const char *sev_str = "info";
    switch (a.severity) {
    case SYS_ALERT_SEVERITY_WARNING:  sev_str = "warning"; break;
    case SYS_ALERT_SEVERITY_CRITICAL: sev_str = "critical"; break;
    default: break;
    }
    cJSON_AddStringToObject(obj, "severity", sev_str);
    cJSON_AddNumberToObject(obj, "current_pct", (double)a.current_value / 100.0);
    cJSON_AddNumberToObject(obj, "threshold_pct", (double)a.threshold / 100.0);
    if (a.task_name[0] != '\0') {
        cJSON_AddStringToObject(obj, "task_name", a.task_name);
        cJSON_AddNumberToObject(obj, "task_cpu_pct", (double)a.task_cpu_pct / 100.0);
    }
    cJSON_AddNumberToObject(obj, "free_internal_kb", (double)(a.free_internal / 1024));
    cJSON_AddNumberToObject(obj, "free_psram_kb", (double)(a.free_psram / 1024));
    return obj;
}

static esp_err_t h_system_alerts(httpd_req_t *req)
{
    system_alert_s cpu_alert = {}, mem_int_alert = {}, mem_psram_alert = {};
    SystemMonitor::instance().get_alerts(&cpu_alert, &mem_int_alert, &mem_psram_alert);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    cJSON_AddItemToObject(root, "cpu", _build_alert_obj(cpu_alert));
    cJSON_AddItemToObject(root, "mem_internal", _build_alert_obj(mem_int_alert));
    cJSON_AddItemToObject(root, "mem_psram", _build_alert_obj(mem_psram_alert));

    /* Also include thresholds from Kconfig */
    cJSON *thresholds = cJSON_CreateObject();
    cJSON_AddNumberToObject(thresholds, "cpu_pct", CONFIG_APP_SYS_MONITOR_CPU_ALERT_PCT);
    cJSON_AddNumberToObject(thresholds, "mem_pct", CONFIG_APP_SYS_MONITOR_MEM_ALERT_PCT);
    cJSON_AddNumberToObject(thresholds, "cooldown_s", CONFIG_APP_SYS_MONITOR_ALERT_COOLDOWN_S);
    cJSON_AddItemToObject(root, "thresholds", thresholds);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (json) {
        httpd_resp_sendstr(req, json);
        cJSON_free(json);
    } else {
        httpd_resp_sendstr(req, "{}");
    }
    cJSON_Delete(root);
    return ESP_OK;
}

/* CORS preflight handler — responds to OPTIONS requests for POST endpoints.
 * Browsers require this before cross-origin POST with Content-Type: application/json. */
static esp_err_t cors_preflight_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/*============================================================================
 * WeChat QR Login API handlers
 *============================================================================*/
#ifdef CONFIG_APP_CLAW_CAP_IM_WECHAT
static esp_err_t h_wx_login_start(httpd_req_t *req)
{
    char body[128] = {0};
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    (void)ret;
    esp_err_t err = cap_im_wechat_qr_login_start("default", false);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "QR login start failed");
        return ESP_FAIL;
    }
    cap_im_wechat_qr_login_status_t st = {};
    cap_im_wechat_qr_login_get_status(&st);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "status", st.status);
    cJSON_AddStringToObject(resp, "message", st.message);
    if (st.qr_data_url[0]) cJSON_AddStringToObject(resp, "qr_data_url", st.qr_data_url);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    if (json) {
        httpd_resp_sendstr(req, json);
        cJSON_free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON build failed");
    }
    return ESP_OK;
}

static esp_err_t h_wx_login_status(httpd_req_t *req)
{
    cap_im_wechat_qr_login_status_t st = {};
    esp_err_t err = cap_im_wechat_qr_login_get_status(&st);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get status");
        return ESP_FAIL;
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "active", st.active);
    cJSON_AddBoolToObject(resp, "configured", st.configured);
    cJSON_AddBoolToObject(resp, "completed", st.completed);
    cJSON_AddBoolToObject(resp, "persisted", st.persisted);
    cJSON_AddStringToObject(resp, "status", st.status);
    cJSON_AddStringToObject(resp, "message", st.message);
    if (st.qr_data_url[0]) cJSON_AddStringToObject(resp, "qr_data_url", st.qr_data_url);
    if (st.account_id[0]) cJSON_AddStringToObject(resp, "account_id", st.account_id);
    if (st.user_id[0]) cJSON_AddStringToObject(resp, "user_id", st.user_id);
    if (st.base_url[0]) cJSON_AddStringToObject(resp, "base_url", st.base_url);
    if (st.completed && st.token[0]) cJSON_AddBoolToObject(resp, "has_token", true);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    if (json) {
        httpd_resp_sendstr(req, json);
        cJSON_free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON build failed");
    }
    return ESP_OK;
}

static esp_err_t h_wx_login_cancel(httpd_req_t *req)
{
    cap_im_wechat_qr_login_cancel();
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "QR login cancelled");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    if (json) {
        httpd_resp_sendstr(req, json);
        cJSON_free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON build failed");
    }
    return ESP_OK;
}

static esp_err_t h_wx_login_persist(httpd_req_t *req)
{
    cap_im_wechat_qr_login_status_t st = {};
    cap_im_wechat_qr_login_get_status(&st);
    if (!st.completed || !st.token[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No completed login to persist");
        return ESP_FAIL;
    }
    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open("claw_im", NVS_READWRITE, &nvs_h);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS open failed");
        return ESP_FAIL;
    }
    nvs_set_str(nvs_h, "wx_token", st.token);
    if (st.base_url[0]) nvs_set_str(nvs_h, "wx_base_url", st.base_url);
    nvs_commit(nvs_h);
    nvs_close(nvs_h);
    cap_im_wechat_qr_login_mark_persisted();

    /* Apply the freshly scanned credentials to the live session. The poll task
     * reads s_wechat.token / s_wechat.base_url on every iteration, so updating
     * them here is enough for getupdates to switch to the new token on its next
     * call. start() is a no-op if the gateway is already running, otherwise it
     * launches it. No stop()/free is needed, which avoids blocking the httpd
     * task while the long-poll exits. */
    cap_im_wechat_client_config_t cfg = {
        .token = st.token,
        .base_url = st.base_url[0] ? st.base_url : "https://ilinkai.weixin.qq.com",
        .cdn_base_url = "https://novac2c.cdn.weixin.qq.com/c2c",
        .account_id = st.account_id[0] ? st.account_id : "default",
        .app_id = "bot",
        .client_version = "131329",
        .route_tag = NULL,
    };
    if (cap_im_wechat_set_client_config(&cfg) == ESP_OK) {
        cap_im_wechat_start();
    }
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "Token saved to NVS");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    if (json) {
        httpd_resp_sendstr(req, json);
        cJSON_free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON build failed");
    }
    return ESP_OK;
}
#endif /* CONFIG_APP_CLAW_CAP_IM_WECHAT */

/*============================================================================
 * LLM/AI Agent Configuration API
 *============================================================================*/
/* GET /api/llm/config — return current LLM config from NVS */
static esp_err_t h_llm_config_get(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    nvs_handle_t nvs_h;
    if (nvs_open("claw_llm", NVS_READONLY, &nvs_h) == ESP_OK) {
        char buf[320] = {0};
        size_t len;
        len = sizeof(buf);
        if (nvs_get_str(nvs_h, "provider", buf, &len) == ESP_OK) cJSON_AddStringToObject(resp, "provider", buf);
        len = sizeof(buf);
        if (nvs_get_str(nvs_h, "api_key", buf, &len) == ESP_OK) {
            /* Never expose plaintext API key over unencrypted HTTP —
             * return has_api_key flag like WiFi password pattern (S15) */
            cJSON_AddBoolToObject(resp, "has_api_key", true);
        }
        len = sizeof(buf);
        if (nvs_get_str(nvs_h, "model", buf, &len) == ESP_OK) cJSON_AddStringToObject(resp, "model", buf);
        len = sizeof(buf);
        if (nvs_get_str(nvs_h, "base_url", buf, &len) == ESP_OK) cJSON_AddStringToObject(resp, "base_url", buf);
        nvs_close(nvs_h);
    }
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    if (json) {
        httpd_resp_sendstr(req, json);
        cJSON_free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON build failed");
    }
    return ESP_OK;
}

/* POST /api/llm/config — save LLM config to NVS
 *
 * Sub-setting completeness check (requirement rule #1): the LLM sub-setting
 * consists of {provider, api_key, model, base_url}. If ANY field is missing
 * or empty, the ENTIRE LLM sub-setting NVS update is skipped (nothing is
 * written) and an error is returned. Other sub-settings (WiFi/volume/IM)
 * update independently. Only when all fields are present and non-empty are
 * they all written together. */
static esp_err_t h_llm_config_set(httpd_req_t *req)
{
    const size_t kMaxBody = 4096;
    if (req->content_len > kMaxBody) {
        ESP_LOGW(TAG, "LLM config save failed: request too large (%ld bytes)", (long)req->content_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }
    char *body = (char *)calloc(1, req->content_len + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory"); return ESP_ERR_NO_MEM; }
    int ret = httpd_req_recv(req, body, req->content_len);
    if (ret <= 0) { free(body); ESP_LOGW(TAG, "LLM config save failed: recv error"); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body"); return ESP_FAIL; }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { ESP_LOGW(TAG, "LLM config save failed: invalid JSON body"); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    auto field_ok = [](cJSON *f) -> bool {
        return f && cJSON_IsString(f) && f->valuestring && strlen(f->valuestring) > 0;
    };

    cJSON *j_provider = cJSON_GetObjectItemCaseSensitive(root, "provider");
    cJSON *j_api_key  = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    cJSON *j_model    = cJSON_GetObjectItemCaseSensitive(root, "model");
    cJSON *j_base_url = cJSON_GetObjectItemCaseSensitive(root, "base_url");

    bool all_ok = field_ok(j_provider) && field_ok(j_api_key) &&
                  field_ok(j_model) && field_ok(j_base_url);

    cJSON *resp = cJSON_CreateObject();
    if (!resp) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }

    if (!all_ok) {
        ESP_LOGW(TAG, "LLM config incomplete (empty field) — skipping whole LLM NVS update");
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error",
            "All LLM fields (provider, api_key, model, base_url) must be non-empty");
        cJSON_Delete(root);
        char *json = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        httpd_resp_set_type(req, "application/json");
        if (json) { httpd_resp_sendstr(req, json); cJSON_free(json); }
        else { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON build failed"); }
        return ESP_OK;
    }

    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open("claw_llm", NVS_READWRITE, &nvs_h);
    if (err != ESP_OK) { ESP_LOGW(TAG, "LLM config save failed: nvs_open error (%s)", esp_err_to_name(err)); cJSON_Delete(root); cJSON_Delete(resp); return ESP_FAIL; }
    nvs_set_str(nvs_h, "provider", j_provider->valuestring);
    nvs_set_str(nvs_h, "api_key", j_api_key->valuestring);
    nvs_set_str(nvs_h, "model", j_model->valuestring);
    nvs_set_str(nvs_h, "base_url", j_base_url->valuestring);
    nvs_commit(nvs_h);
    nvs_close(nvs_h);

    ESP_LOGI(TAG, "LLM config saved successfully (provider=%s, model=%s)",
             j_provider && j_provider->valuestring ? j_provider->valuestring : "",
             j_model && j_model->valuestring ? j_model->valuestring : "");

    cJSON_Delete(root);

    cJSON_AddBoolToObject(resp, "ok", true);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    if (json) {
        httpd_resp_sendstr(req, json);
        cJSON_free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON build failed");
    }
    return ESP_OK;
}

/*============================================================================
 * Feishu / QQ / Telegram Configuration API
 *============================================================================*/
#ifdef CONFIG_APP_CLAW_CAP_IM_FEISHU
static esp_err_t h_feishu_config(httpd_req_t *req)
{
    const size_t kMaxBody = 4096;
    if (req->content_len > kMaxBody) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }
    char *body = (char *)calloc(1, req->content_len + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory"); return ESP_ERR_NO_MEM; }
    int ret = httpd_req_recv(req, body, req->content_len);
    if (ret <= 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body"); return ESP_FAIL; }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }
    cJSON *app_id_j = cJSON_GetObjectItemCaseSensitive(root, "app_id");
    cJSON *app_secret_j = cJSON_GetObjectItemCaseSensitive(root, "app_secret");
    bool fs_ok = app_id_j && cJSON_IsString(app_id_j) && app_id_j->valuestring &&
                 strlen(app_id_j->valuestring) > 0 &&
                 app_secret_j && cJSON_IsString(app_secret_j) && app_secret_j->valuestring &&
                 strlen(app_secret_j->valuestring) > 0;
    if (fs_ok) {
        nvs_handle_t nvs_h;
        if (nvs_open("claw_im", NVS_READWRITE, &nvs_h) == ESP_OK) {
            nvs_set_str(nvs_h, "fs_app_id", app_id_j->valuestring);
            nvs_set_str(nvs_h, "fs_app_secret", app_secret_j->valuestring);
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
        }
    } else {
        ESP_LOGW(TAG, "Feishu config incomplete (empty field) — skipping whole Feishu NVS update");
    }
    cJSON_Delete(root);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", fs_ok);
    if (!fs_ok) cJSON_AddStringToObject(resp, "error", "app_id and app_secret must be non-empty");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    if (json) {
        httpd_resp_sendstr(req, json);
        cJSON_free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON build failed");
    }
    return ESP_OK;
}
#endif

#ifdef CONFIG_APP_CLAW_CAP_IM_QQ
static esp_err_t h_qq_config(httpd_req_t *req)
{
    const size_t kMaxBody = 4096;
    if (req->content_len > kMaxBody) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }
    char *body = (char *)calloc(1, req->content_len + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory"); return ESP_ERR_NO_MEM; }
    int ret = httpd_req_recv(req, body, req->content_len);
    if (ret <= 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body"); return ESP_FAIL; }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }
    cJSON *app_id_j = cJSON_GetObjectItemCaseSensitive(root, "app_id");
    cJSON *app_secret_j = cJSON_GetObjectItemCaseSensitive(root, "app_secret");
    bool qq_ok = app_id_j && cJSON_IsString(app_id_j) && app_id_j->valuestring &&
                 strlen(app_id_j->valuestring) > 0 &&
                 app_secret_j && cJSON_IsString(app_secret_j) && app_secret_j->valuestring &&
                 strlen(app_secret_j->valuestring) > 0;
    if (qq_ok) {
        nvs_handle_t nvs_h;
        if (nvs_open("claw_im", NVS_READWRITE, &nvs_h) == ESP_OK) {
            nvs_set_str(nvs_h, "qq_app_id", app_id_j->valuestring);
            nvs_set_str(nvs_h, "qq_app_secret", app_secret_j->valuestring);
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
        }
    } else {
        ESP_LOGW(TAG, "QQ config incomplete (empty field) — skipping whole QQ NVS update");
    }
    cJSON_Delete(root);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", qq_ok);
    if (!qq_ok) cJSON_AddStringToObject(resp, "error", "app_id and app_secret must be non-empty");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    if (json) {
        httpd_resp_sendstr(req, json);
        cJSON_free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON build failed");
    }
    return ESP_OK;
}
#endif

#ifdef CONFIG_APP_CLAW_CAP_IM_TG
static esp_err_t h_tg_config(httpd_req_t *req)
{
    const size_t kMaxBody = 4096;
    if (req->content_len > kMaxBody) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }
    char *body = (char *)calloc(1, req->content_len + 1);
    if (!body) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory"); return ESP_ERR_NO_MEM; }
    int ret = httpd_req_recv(req, body, req->content_len);
    if (ret <= 0) { free(body); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to read request body"); return ESP_FAIL; }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }
    cJSON *token_j = cJSON_GetObjectItemCaseSensitive(root, "token");
    bool tg_ok = token_j && cJSON_IsString(token_j) && token_j->valuestring &&
                 strlen(token_j->valuestring) > 0;
    if (tg_ok) {
        nvs_handle_t nvs_h;
        if (nvs_open("claw_im", NVS_READWRITE, &nvs_h) == ESP_OK) {
            nvs_set_str(nvs_h, "tg_token", token_j->valuestring);
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
        }
    } else {
        ESP_LOGW(TAG, "Telegram config incomplete (empty token) — skipping whole Telegram NVS update");
    }
    cJSON_Delete(root);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", tg_ok);
    if (!tg_ok) cJSON_AddStringToObject(resp, "error", "token must be non-empty");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    if (json) {
        httpd_resp_sendstr(req, json);
        cJSON_free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON build failed");
    }
    return ESP_OK;
}
#endif

/* ── Agent Chat API ── */
#if defined(CONFIG_APP_CLAW_CAP_IM_WECHAT) || defined(CONFIG_APP_CLAW_CAP_IM_FEISHU) || \
    defined(CONFIG_APP_CLAW_CAP_IM_QQ) || defined(CONFIG_APP_CLAW_CAP_IM_TG)

/* Try to lazily initialize the agent if LLM is configured but agent wasn't started at boot.
 * Returns ESP_OK if agent is ready, or an error if it can't be started. */
static esp_err_t ensure_agent_started(void)
{
    /* Fast path: try event router first */
    claw_event_router_result_t result = {};
    claw_event_t probe = {};
    strlcpy(probe.event_type, "ping", sizeof(probe.event_type));
    if (claw_event_router_handle_event(&probe, &result) == ESP_OK) {
        return ESP_OK;
    }

    /* Router not up — try direct agent mgr */
    if (claw_agent_mgr_submit_root_text("ping", NULL, 0, 100, NULL) == ESP_OK ||
        claw_agent_mgr_get_root_core() != NULL) {
        return ESP_OK;
    }

    /* Agent mgr not up — check if LLM is configured in NVS and try to init */
    nvs_handle_t nvs_h;
    char api_key[320] = {0};
    size_t len = sizeof(api_key);
    if (nvs_open("claw_llm", NVS_READONLY, &nvs_h) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (nvs_get_str(nvs_h, "api_key", api_key, &len) != ESP_OK || len <= 1) {
        nvs_close(nvs_h);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Agent chat: LLM configured but agent not started — hot-initializing...");

    /* Load full LLM config */
    cap_llm_config_t llm_cfg = {};
    strlcpy(llm_cfg.api_key, api_key, sizeof(llm_cfg.api_key));

    char buf[320] = {0};
    len = sizeof(buf);
    if (nvs_get_str(nvs_h, "provider", buf, &len) == ESP_OK) {
        if (strcmp(buf, "openai") == 0) strlcpy(llm_cfg.backend_type, "openai_compatible", sizeof(llm_cfg.backend_type));
        else if (strcmp(buf, "anthropic") == 0) strlcpy(llm_cfg.backend_type, "anthropic", sizeof(llm_cfg.backend_type));
        else strlcpy(llm_cfg.backend_type, "openai_compatible", sizeof(llm_cfg.backend_type));
    } else {
        strlcpy(llm_cfg.backend_type, "openai_compatible", sizeof(llm_cfg.backend_type));
    }
    len = sizeof(buf);
    if (nvs_get_str(nvs_h, "model", buf, &len) == ESP_OK) strlcpy(llm_cfg.model, buf, sizeof(llm_cfg.model));
    else strlcpy(llm_cfg.model, "deepseek-chat", sizeof(llm_cfg.model));
    len = sizeof(buf);
    if (nvs_get_str(nvs_h, "base_url", buf, &len) == ESP_OK) strlcpy(llm_cfg.base_url, buf, sizeof(llm_cfg.base_url));
    strlcpy(llm_cfg.auth_type, "bearer", sizeof(llm_cfg.auth_type));
    strlcpy(llm_cfg.max_tokens, "4096", sizeof(llm_cfg.max_tokens));
    nvs_close(nvs_h);

    /* Set claw_paths if not already set */
    if (!claw_paths_get(CLAW_PATH_DATA)) {
        claw_paths_set(CLAW_PATH_DATA, "/sdcard/claw");
        claw_paths_set(CLAW_PATH_SYSTEM, "/sdcard");
    }

    /* Init cap framework if needed */
    static std::atomic<bool> s_caps_initialized{false};
    if (!s_caps_initialized.load(std::memory_order_acquire)) {
        claw_cap_init();
        cap_system_register_group();
        cap_files_register_group();
        cap_http_request_register_group();
        cap_http_request_set_allowlist("api.openweathermap.org,api.weatherapi.com,wttr.in");
        cap_lua_register_group();
        cap_lua_add_package_path_dir("/sdcard/claw/lua");
        cap_web_search_register_group();
        cap_llm_config_register_group();

        /* Memory/skill init (must precede session manager registration) */
        claw_memory_config_t mem_cfg = {};
        mem_cfg.session_root_dir = "/sdcard/claw/sessions";
        mem_cfg.memory_root_dir = "/sdcard/claw/memory";
        mem_cfg.max_message_chars = 4096;
        claw_memory_init(&mem_cfg);

        claw_skill_config_t skill_cfg = {};
        skill_cfg.session_state_root_dir = "/sdcard/claw/skills";
        skill_cfg.max_file_bytes = 32768;
        claw_skill_init(&skill_cfg);

        cap_session_mgr_set_session_root_dir("/sdcard/claw/sessions");
        cap_session_mgr_register_group();

        cap_scheduler_register_group();
        cap_agent_mgr_register_group();
        cap_router_mgr_register_group();
        cap_skill_mgr_register_group("/sdcard/claw/skills");
        s_caps_initialized.store(true, std::memory_order_release);
    }

    /* Init event router */
    claw_event_router_config_t er_cfg = {};
    er_cfg.event_queue_len = 16;
    er_cfg.task_stack_size = 8192;
    er_cfg.task_priority = 5;
    er_cfg.task_core = tskNO_AFFINITY;
    er_cfg.default_route_messages_to_agent = true;
    esp_err_t err = claw_event_router_init(&er_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Hot-init: event router init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Init agent manager and create root agent */
    claw_core_config_t core_cfg = {};
    core_cfg.api_key = llm_cfg.api_key;
    core_cfg.backend_type = llm_cfg.backend_type;
    core_cfg.model = llm_cfg.model;
    core_cfg.base_url = llm_cfg.base_url[0] ? llm_cfg.base_url : NULL;
    core_cfg.auth_type = llm_cfg.auth_type;
    core_cfg.max_tokens = 4096;
    core_cfg.timeout_ms = 30000;
    core_cfg.supports_tools = true;
    core_cfg.supports_vision = true;

    claw_agent_mgr_config_t mgr_cfg = { .core_config = &core_cfg };
    err = claw_agent_mgr_init(&mgr_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Hot-init: agent mgr init failed: %s", esp_err_to_name(err));
        return err;
    }
    const char *root_id = NULL;
    err = claw_agent_mgr_create_root_agent(&root_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Hot-init: create root agent failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Start services */
    claw_event_router_start();
    claw_cap_start_all();

    /* Bind outbound channels */
    claw_event_router_register_outbound_binding("wechat", "cap_im_wechat");
    claw_event_router_register_outbound_binding("telegram", "cap_im_tg");
    claw_event_router_register_outbound_binding("feishu", "cap_im_feishu");
    claw_event_router_register_outbound_binding("qq", "cap_im_qq");

    ESP_LOGI(TAG, "Hot-init: agent started successfully (model: %s)", llm_cfg.model);
    return ESP_OK;
}

static esp_err_t h_agent_chat(httpd_req_t *req)
{
    const size_t kMaxBody = 4096;
    if (req->content_len > kMaxBody) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }
    char *buf = (char *)calloc(req->content_len + 1, 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) { free(buf); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Read failed"); return ESP_FAIL; }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *msg_j = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (!msg_j || !cJSON_IsString(msg_j) || !msg_j->valuestring[0]) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "message field required");
        return ESP_FAIL;
    }

    /* Ensure agent is running (lazy hot-init if needed) */
    esp_err_t err = ensure_agent_started();

    if (err == ESP_OK) {
        /* Submit via event router for full IM→Agent pipeline */
        claw_event_t event = {};
        strlcpy(event.event_type, "message", sizeof(event.event_type));
        strlcpy(event.source_channel, "web_chat", sizeof(event.source_channel));
        strlcpy(event.content_type, "text/plain", sizeof(event.content_type));
        event.text = msg_j->valuestring;

        claw_event_router_result_t result = {};
        err = claw_event_router_handle_event(&event, &result);
    }

    cJSON_Delete(root);

    /* Build response */
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    bool ok = (err == ESP_OK);
    cJSON_AddBoolToObject(resp, "ok", ok);
    if (ok) {
        cJSON_AddStringToObject(resp, "reply", "Message submitted to agent. Check IM channel for response.");
    } else {
        const char *hint = (err == ESP_ERR_INVALID_STATE)
            ? "LLM not configured. Save API key in the AI Agent card first."
            : esp_err_to_name(err);
        cJSON_AddStringToObject(resp, "error", hint);
    }

    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    if (json) {
        httpd_resp_sendstr(req, json);
        cJSON_free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON build failed");
    }
    return ESP_OK;
}
#endif /* IM channels */

static void _register_web_config_uris(httpd_handle_t hd)
{
    /* Core */
    httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
    httpd_uri_t uri_status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };
    httpd_uri_t uri_settings = { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_handler, .user_ctx = NULL };
    httpd_uri_t uri_cam = { .uri = "/api/camera_stream", .method = HTTP_POST, .handler = camera_stream_handler, .user_ctx = NULL };
    httpd_uri_t uri_reset = { .uri = "/api/factory_reset", .method = HTTP_POST, .handler = factory_reset_handler, .user_ctx = NULL };
    httpd_register_uri_handler(hd, &uri_index);
    httpd_register_uri_handler(hd, &uri_status);
    httpd_register_uri_handler(hd, &uri_settings);
    httpd_register_uri_handler(hd, &uri_cam);
    httpd_register_uri_handler(hd, &uri_reset);

    /* Audio recording / playback */
    httpd_uri_t uri_r_start = { .uri = "/api/audio/record_start", .method = HTTP_GET, .handler = h_rec_start };
    httpd_uri_t uri_r_stop  = { .uri = "/api/audio/record_stop",  .method = HTTP_GET, .handler = h_rec_stop };
    httpd_uri_t uri_r_stat  = { .uri = "/api/audio/record_status",.method = HTTP_GET, .handler = h_rec_status };
    httpd_uri_t uri_a_list  = { .uri = "/api/audio/list",         .method = HTTP_GET, .handler = h_list };
    httpd_uri_t uri_a_play  = { .uri = "/api/audio/play",          .method = HTTP_GET, .handler = h_play };
    httpd_uri_t uri_a_stop  = { .uri = "/api/audio/stop",          .method = HTTP_GET, .handler = h_stop };
    httpd_register_uri_handler(hd, &uri_r_start);
    httpd_register_uri_handler(hd, &uri_r_stop);
    httpd_register_uri_handler(hd, &uri_r_stat);
    httpd_register_uri_handler(hd, &uri_a_list);
    httpd_register_uri_handler(hd, &uri_a_play);
    httpd_register_uri_handler(hd, &uri_a_stop);

    /* File Manager */
    httpd_uri_t uri_fm_list = { .uri = "/api/files/list", .method = HTTP_GET, .handler = h_files_list };
    httpd_uri_t uri_fm_dl   = { .uri = "/api/files/download", .method = HTTP_GET, .handler = h_files_download };
    httpd_uri_t uri_fm_del  = { .uri = "/api/files/delete", .method = HTTP_POST, .handler = h_files_delete };
    httpd_uri_t uri_fm_del_batch = { .uri = "/api/files/delete_batch", .method = HTTP_POST, .handler = h_files_delete_batch };
    httpd_register_uri_handler(hd, &uri_fm_list);
    httpd_register_uri_handler(hd, &uri_fm_dl);
    httpd_register_uri_handler(hd, &uri_fm_del);
    httpd_register_uri_handler(hd, &uri_fm_del_batch);

    /* CORS preflight (OPTIONS) handlers for POST endpoints */
    httpd_uri_t uri_cors_settings = { .uri = "/api/settings", .method = HTTP_OPTIONS, .handler = cors_preflight_handler, .user_ctx = NULL };
    httpd_uri_t uri_cors_cam = { .uri = "/api/camera_stream", .method = HTTP_OPTIONS, .handler = cors_preflight_handler, .user_ctx = NULL };
    httpd_uri_t uri_cors_reset = { .uri = "/api/factory_reset", .method = HTTP_OPTIONS, .handler = cors_preflight_handler, .user_ctx = NULL };
    httpd_uri_t uri_cors_fm_del = { .uri = "/api/files/delete", .method = HTTP_OPTIONS, .handler = cors_preflight_handler, .user_ctx = NULL };
    httpd_uri_t uri_cors_fm_del_batch = { .uri = "/api/files/delete_batch", .method = HTTP_OPTIONS, .handler = cors_preflight_handler, .user_ctx = NULL };
    httpd_register_uri_handler(hd, &uri_cors_settings);
    httpd_register_uri_handler(hd, &uri_cors_cam);
    httpd_register_uri_handler(hd, &uri_cors_reset);
    httpd_register_uri_handler(hd, &uri_cors_fm_del);
    httpd_register_uri_handler(hd, &uri_cors_fm_del_batch);

    /* ULog endpoints */
    httpd_uri_t uri_ulog_status = { .uri = "/api/ulog/status", .method = HTTP_GET, .handler = ulog_status_handler, .user_ctx = NULL };
    httpd_uri_t uri_ulog_start = { .uri = "/api/ulog/start", .method = HTTP_POST, .handler = ulog_start_handler, .user_ctx = NULL };
    httpd_uri_t uri_ulog_stop = { .uri = "/api/ulog/stop", .method = HTTP_POST, .handler = ulog_stop_handler, .user_ctx = NULL };
    httpd_uri_t uri_ulog_cors = { .uri = "/api/ulog/start", .method = HTTP_OPTIONS, .handler = cors_preflight_handler, .user_ctx = NULL };
    httpd_uri_t uri_ulog_cors2 = { .uri = "/api/ulog/stop", .method = HTTP_OPTIONS, .handler = cors_preflight_handler, .user_ctx = NULL };
    httpd_register_uri_handler(hd, &uri_ulog_status);
    httpd_register_uri_handler(hd, &uri_ulog_start);
    httpd_register_uri_handler(hd, &uri_ulog_stop);
    httpd_register_uri_handler(hd, &uri_ulog_cors);
    httpd_register_uri_handler(hd, &uri_ulog_cors2);

    /* System stats */
    httpd_uri_t uri_sys_stats = { .uri = "/api/system_stats", .method = HTTP_GET, .handler = h_system_stats, .user_ctx = NULL };
    httpd_uri_t uri_sys_alerts = { .uri = "/api/system_alerts", .method = HTTP_GET, .handler = h_system_alerts, .user_ctx = NULL };
    httpd_register_uri_handler(hd, &uri_sys_stats);
    httpd_register_uri_handler(hd, &uri_sys_alerts);

    /* ── WeChat QR Login API ── */
#ifdef CONFIG_APP_CLAW_CAP_IM_WECHAT
    httpd_uri_t uri_wx_start  = { .uri = "/api/wechat/login/start",   .method = HTTP_POST, .handler = h_wx_login_start };
    httpd_uri_t uri_wx_status = { .uri = "/api/wechat/login/status",  .method = HTTP_GET,  .handler = h_wx_login_status };
    httpd_uri_t uri_wx_cancel = { .uri = "/api/wechat/login/cancel",  .method = HTTP_POST, .handler = h_wx_login_cancel };
    httpd_uri_t uri_wx_save   = { .uri = "/api/wechat/login/persist", .method = HTTP_POST, .handler = h_wx_login_persist };
    httpd_uri_t cors_wx_start = { .uri = "/api/wechat/login/start",   .method = HTTP_OPTIONS, .handler = cors_preflight_handler };
    httpd_uri_t cors_wx_stat  = { .uri = "/api/wechat/login/status",  .method = HTTP_OPTIONS, .handler = cors_preflight_handler };
    httpd_uri_t cors_wx_cancel= { .uri = "/api/wechat/login/cancel",  .method = HTTP_OPTIONS, .handler = cors_preflight_handler };
    httpd_uri_t cors_wx_save  = { .uri = "/api/wechat/login/persist", .method = HTTP_OPTIONS, .handler = cors_preflight_handler };
    httpd_register_uri_handler(hd, &uri_wx_start);
    httpd_register_uri_handler(hd, &uri_wx_status);
    httpd_register_uri_handler(hd, &uri_wx_cancel);
    httpd_register_uri_handler(hd, &uri_wx_save);
    httpd_register_uri_handler(hd, &cors_wx_start);
    httpd_register_uri_handler(hd, &cors_wx_stat);
    httpd_register_uri_handler(hd, &cors_wx_cancel);
    httpd_register_uri_handler(hd, &cors_wx_save);
    ESP_LOGI("webcfg", "WeChat QR login API registered (/api/wechat/login/*)");
#endif /* CONFIG_APP_CLAW_CAP_IM_WECHAT */

    /* ── LLM/AI Agent Configuration API ── */
    httpd_uri_t uri_llm_get = { .uri = "/api/llm/config", .method = HTTP_GET, .handler = h_llm_config_get };
    httpd_uri_t uri_llm_set = { .uri = "/api/llm/config", .method = HTTP_POST, .handler = h_llm_config_set };
    httpd_uri_t cors_llm_get = { .uri = "/api/llm/config", .method = HTTP_OPTIONS, .handler = cors_preflight_handler };
    httpd_register_uri_handler(hd, &uri_llm_get);
    httpd_register_uri_handler(hd, &uri_llm_set);
    httpd_register_uri_handler(hd, &cors_llm_get);

    /* ── Feishu / QQ / Telegram Config API ── */
#ifdef CONFIG_APP_CLAW_CAP_IM_FEISHU
    { httpd_uri_t u = { .uri = "/api/feishu/config", .method = HTTP_POST, .handler = h_feishu_config }; httpd_register_uri_handler(hd, &u); }
    { httpd_uri_t u = { .uri = "/api/feishu/config", .method = HTTP_OPTIONS, .handler = cors_preflight_handler }; httpd_register_uri_handler(hd, &u); }
#endif
#ifdef CONFIG_APP_CLAW_CAP_IM_QQ
    { httpd_uri_t u = { .uri = "/api/qq/config", .method = HTTP_POST, .handler = h_qq_config }; httpd_register_uri_handler(hd, &u); }
    { httpd_uri_t u = { .uri = "/api/qq/config", .method = HTTP_OPTIONS, .handler = cors_preflight_handler }; httpd_register_uri_handler(hd, &u); }
#endif
#ifdef CONFIG_APP_CLAW_CAP_IM_TG
    { httpd_uri_t u = { .uri = "/api/tg/config", .method = HTTP_POST, .handler = h_tg_config }; httpd_register_uri_handler(hd, &u); }
    { httpd_uri_t u = { .uri = "/api/tg/config", .method = HTTP_OPTIONS, .handler = cors_preflight_handler }; httpd_register_uri_handler(hd, &u); }
#endif

    /* ── Agent Chat API ── */
#if defined(CONFIG_APP_CLAW_CAP_IM_WECHAT) || defined(CONFIG_APP_CLAW_CAP_IM_FEISHU) || \
    defined(CONFIG_APP_CLAW_CAP_IM_QQ) || defined(CONFIG_APP_CLAW_CAP_IM_TG)
    { httpd_uri_t u = { .uri = "/api/agent/chat", .method = HTTP_POST, .handler = h_agent_chat }; httpd_register_uri_handler(hd, &u); }
    { httpd_uri_t u = { .uri = "/api/agent/chat", .method = HTTP_OPTIONS, .handler = cors_preflight_handler }; httpd_register_uri_handler(hd, &u); }
#endif
}

/*============================================================================
 * Self-heal watchdog
 *============================================================================*/
/* Probe the HTTP server by connecting to its own STA IP and issuing a tiny
 * GET. This catches the case where httpd silently stops *accepting* new
 * connections (e.g. all session slots occupied by stale/aborted sockets with
 * lru_purge off, or the httpd main thread wedged in a handler) even though the
 * handle/wifi still report UP. Returns true if the server responds. */
static bool web_config_self_probe(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.ip.addr == 0) return false;

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return false;

    struct timeval to = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(WEB_CONFIG_PORT),
        .sin_addr   = { .s_addr = ip.ip.addr },
    };

    bool ok = false;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        /* Use HTTP/1.1 with Connection: close for clean server-side lifecycle.
         * HTTP/1.0 caused http_parser HPE_INVALID_URL (errno 16) because the
         * server sometimes received only a partial request (0/24 bytes parsed)
         * before the client closed — the truncated request line looked invalid. */
        const char *req = "GET /api/status HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        /* Send the full request before signalling EOF. A short send() must not
         * be cut off by shutdown(SHUT_WR), or the server gets a truncated
         * request (HPE_INVALID_URL / partial request). */
        ssize_t sent = 0, total = (ssize_t)strlen(req);
        while (sent < total) {
            ssize_t r = send(fd, req + sent, (size_t)(total - sent), 0);
            if (r <= 0) break;
            sent += r;
        }
        if (sent == total) {
            /* Signal end-of-request so the server doesn't wait for more data. */
            shutdown(fd, SHUT_WR);
            char buf[64];
            int n;
            bool got_any = false;
            /* Drain the entire response (until the server closes with FIN).
             * Reading only part then close()ing leaves unread data in the
             * socket, which makes LWIP emit an RST and the server logs
             * ECONNRESET/ENOTSOCK. Draining avoids that self-inflicted noise. */
            while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) got_any = true;
            if (got_any) ok = true;
        }
    }
    close(fd);
    return ok;
}

/*============================================================================
 * Task
 *============================================================================*/
static void web_config_task(void *arg)
{
    /* Subscribe to wifi_state uORB topic (published by PhoneAppSettings) */
    if (s_wifi_state_sub < 0) {
        s_wifi_state_sub = orb_subscribe(ORB_ID(wifi_state));
    }

    /* Create audio mutex BEFORE any HTTP handlers can run.
     * Avoids race where two handlers lazily create separate mutexes. */
    s_audio_mutex = xSemaphoreCreateMutex();

    /* Create NVS cache mutex to protect s_nvs_cache / s_nvs_cache_count
     * from concurrent access across HTTP, audio, and WiFi tasks. */
    s_nvs_cache_mutex = xSemaphoreCreateMutex();

    /* Mark running early so web_config_server_stop() can signal us
     * even during the WiFi-wait window below. */
    s_running = true;

    /* Wait for WiFi connection before starting HTTP server.
     * LWIP TCPIP mbox is only valid after netif is up. */
    ESP_LOGI(TAG, "Web config waiting for WiFi connection...");

    bool stop_requested = false;
    /* Check if already connected (event may have fired before this task) */
    if (!wifi_sta_is_connected()) {
        ESP_LOGI(TAG, "Waiting for wifi_state topic...");
        struct wifi_state_s ws = {};
        while (1) {
            if (!s_running) { stop_requested = true; break; }
            if (s_wifi_state_sub >= 0) {
                orb_copy(ORB_ID(wifi_state), s_wifi_state_sub, &ws);
                if (ws.connected) break;
            }
            /* Also check directly in case uORB isn't set up yet */
            if (wifi_sta_is_connected()) break;
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    if (stop_requested) goto cleanup;

    ESP_LOGI(TAG, "WiFi connected, starting web config server on port %d...",
             WEB_CONFIG_PORT);

    /* Start SNTP for wall-clock time; ULog auto-starts on SNTP sync */
    sntp_start_and_ulog_autostart();

    {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_CONFIG_PORT;
    config.ctrl_port   = WEB_CONFIG_PORT + 1;  /* 8081 — avoid collision with CameraStream ctrl=32768 */
    config.max_uri_handlers = 48;  /* 5 core + 6 audio + 4 file mgr + 5 CORS + 5 ULog + 2 system + 8 WeChat + 3 LLM + 2 Feishu + 2 QQ + 2 TG + 2 agent chat + 2 spare */
    config.max_open_sockets = 12;  /* Headroom for Web UI keep-alive connections; lru_purge keeps accept() always active */
    config.stack_size = 8192;      /* default 4096 overflows: file download handler has ~1.4KB
                                      stack vars + ESP_LOGI → uart_write → recursive mutex
                                      needs deep call chain; logger vprintf hook adds more */
    config.lru_purge_enable = true;
    config.core_id = 0;  /* Pin to Core 0 — Core 1 runs LVGL rendering */
    /* TCP keep-alive: detect dead connections quickly so sockets don't
     * leak when clients disconnect abruptly (ECONNRESET/EAGAIN).  Without
     * keep-alive, a half-closed TCP can block select() indefinitely,
     * preventing new connections and making the server appear unreachable. */
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;        /* seconds before first probe */
    config.keep_alive_interval = 5;    /* seconds between probes */
    config.keep_alive_count = 3;       /* failed probes → close */

    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %d", WEB_CONFIG_PORT);
        goto cleanup;
    }

    _register_web_config_uris(s_httpd);

    /* mDNS: advertise web config on the local network */
    if (shared_mdns_ensure()) {
        mdns_instance_name_set("esp-web-config");

        mdns_txt_item_t txt[] = {
            {(char *)"board", (char *)CONFIG_IDF_TARGET},
            {(char *)"path",  (char *)"/"},
        };
        mdns_service_add("ESP32-WebConfig", "_http", "_tcp", WEB_CONFIG_PORT,
                         txt, sizeof(txt) / sizeof(txt[0]));
        s_mdns_running = true;
        ESP_LOGI(TAG, "mDNS: esp-web.local:%d + %s.local:%d", WEB_CONFIG_PORT, shared_mdns_hostname(), WEB_CONFIG_PORT);
    } else {
        ESP_LOGW(TAG, "mDNS init failed");
    }

    ESP_LOGI(TAG, "Web config server started on port %d", WEB_CONFIG_PORT);

    /* Auto-start camera stream if NVS says it was enabled */
    if (nvs_get_i32_def(NVS_KEY_CAM_STREAM, 0)) {
        ESP_LOGI(TAG, "NVS cam_stream=1, auto-starting camera stream...");
        CameraStream::instance().start();
    }

    /* Idle — HTTP server runs in its own internal threads.
     * Check s_running flag for clean exit when web_config_server_stop() is called.
     * Periodically probe httpd health and detect WiFi disconnection to prevent
     * stale TCP sessions from blocking the httpd select() loop. */
    int health_log_counter = 0;
    int probe_fail_count = 0;
    bool prev_wifi_up = true;
    bool ulog_autostart_done = false;
    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* ULog auto-start: deferred from SNTP callback (which runs in lwIP
         * tcpip task with only 3KB stack — too small for ulog_writer_start()'s
         * heavy file I/O). Check the flag set by the callback and start here
         * in the web_config_task (8KB stack). Only auto-start once per boot;
         * if user manually stops ULog, it stays stopped.
         *
         * Race condition guard: SNTP may sync before ULog is initialized
         * (state == ULOG_STATE_UNINIT). In that case, skip this iteration
         * without setting ulog_autostart_done so we retry on the next pass
         * once ULog init completes and state transitions to IDLE. */
        if (s_sntp_synced.load(std::memory_order_acquire) && !ulog_autostart_done) {
            ulog_writer_t *ulog = ulog_writer_get();
            ulog_state_t state = ulog_writer_get_state(ulog);
            if (state == ULOG_STATE_IDLE) {
                ulog_autostart_done = true;
                esp_err_t err = ulog_writer_start(ulog);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "ULog auto-started after SNTP sync");
                } else {
                    ESP_LOGW(TAG, "ULog auto-start failed: %s", esp_err_to_name(err));
                }
            } else if (state != ULOG_STATE_UNINIT) {
                /* RUNNING, ERROR, etc. — no point retrying */
                ulog_autostart_done = true;
            }
            /* else: UNINIT — ULog not initialized yet, retry next iteration */
        }

        bool wifi_up = wifi_sta_is_connected();

        /* When WiFi goes down, stop httpd to flush stale TCP sessions.
         * Without this, half-open connections from the WiFi outage can
         * exhaust max_open_sockets and block new connections even after
         * WiFi recovers. Restarting httpd is the cleanest way to reset. */
        if (prev_wifi_up && !wifi_up && s_httpd) {
            ESP_LOGW(TAG, "WiFi down — stopping httpd to flush stale sessions");
            httpd_stop(s_httpd);
            s_httpd = NULL;
        }
        /* When WiFi is up but httpd is not running, start it.
         * This handles both WiFi recovery and httpd_start failure retry. */
        if (wifi_up && !s_httpd) {
            ESP_LOGI(TAG, "WiFi up — starting httpd on port %d", WEB_CONFIG_PORT);
            httpd_config_t config = HTTPD_DEFAULT_CONFIG();
            config.server_port = WEB_CONFIG_PORT;
            config.ctrl_port   = WEB_CONFIG_PORT + 1;
            config.max_uri_handlers = 48;  /* must match initial start value */
            /* Large enough socket pool so the web UI's multiple keep-alive
             * connections don't exhaust it. */
            config.max_open_sockets = 12;
            config.stack_size = 8192;
            /* CRITICAL: keep the listen socket always monitored so new
             * connections are accepted even when all slots look busy.
             * Without this, httpd stops calling accept() once max_open_sockets
             * are occupied (e.g. by stale/aborted half-open sockets) and the
             * server silently rejects all reconnect attempts until reboot. */
            config.lru_purge_enable = true;
            config.core_id = 0;
            config.keep_alive_enable = true;
            config.keep_alive_idle = 5;
            config.keep_alive_interval = 5;
            config.keep_alive_count = 3;
            if (httpd_start(&s_httpd, &config) == ESP_OK) {
                _register_web_config_uris(s_httpd);
                probe_fail_count = 0;
                ESP_LOGI(TAG, "httpd started successfully");
            } else {
                ESP_LOGE(TAG, "httpd start failed — will retry next cycle");
            }
        }
        prev_wifi_up = wifi_up;

        /* Self-heal watchdog: directly probe the HTTP server from this task.
         * Detects the "handle=UP but can't reconnect" condition that WiFi
         * transitions don't cover. On sustained probe failure, bounce httpd
         * so stale sessions are flushed and accept() resumes. */
        if (wifi_up && s_httpd && ++health_log_counter >= 15) {  /* every 15s */
            health_log_counter = 0;
            if (web_config_self_probe()) {
                probe_fail_count = 0;
            } else if (++probe_fail_count >= 2) {  /* ~30s of failures */
                ESP_LOGW(TAG, "httpd probe FAILED %d times — restarting httpd to recover",
                         probe_fail_count);
                httpd_stop(s_httpd);
                s_httpd = NULL;
                probe_fail_count = 0;
            }
        }
    }

    } /* end if (!stop_requested) */

    /* Clean exit path: stop HTTP server and mDNS from within the task */
cleanup:
    if (s_mdns_running) {
        mdns_service_remove("_http", "_tcp");
        shared_mdns_release();
        s_mdns_running = false;
    }
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_audio_mutex) {
        vSemaphoreDelete(s_audio_mutex);
        s_audio_mutex = NULL;
    }
    if (s_nvs_cache_mutex) {
        vSemaphoreDelete(s_nvs_cache_mutex);
        s_nvs_cache_mutex = NULL;
    }
    /* TCB is pre-allocated and never freed — ~340B internal SRAM,
     * negligible cost for permanent use in embedded firmware.
     * Avoids TCB use-after-free race with idle task entirely. */
    if (s_wifi_state_sub >= 0) {
        orb_unsubscribe(s_wifi_state_sub);
        s_wifi_state_sub = -1;
    }

    s_running = false;
    s_task_handle.store(nullptr, std::memory_order_release);
    vTaskDelete(NULL);
}

/*============================================================================
 * Public API
 *============================================================================*/
void web_config_server_start(void)
{
    if (s_running || s_task_handle.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Web config already running");
        return;
    }
    TaskHandle_t tmp_handle = nullptr;
    BaseType_t ret = xTaskCreatePinnedToCore(
        web_config_task, "web_config", TASK_STACK_SIZE,
        NULL, TASK_PRIORITY, &tmp_handle, 1);  /* Core 1 */
    if (ret == pdPASS) {
        s_task_handle.store(tmp_handle, std::memory_order_release);
    }
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create web_config task");
    }
}

void web_config_server_stop(void)
{
    if (!s_running) return;

    /* Stop audio */
    audio_lock();
    s_is_recording = false; s_audio_running = false;
    audio_unlock();
    vTaskDelay(pdMS_TO_TICKS(300));
    audio_lock();
    if (s_shine) { int wr=0; unsigned char *d=shine_flush(s_shine,&wr); if(d&&wr>0&&s_rec_file) fwrite(d,1,wr,s_rec_file); shine_close(s_shine); s_shine=NULL; }
    if (s_rec_file) { fclose(s_rec_file); s_rec_file=NULL; }
    if (s_pcm_buf) { heap_caps_free(s_pcm_buf); s_pcm_buf=NULL; }
    s_playing = false;
    if (s_asp) { esp_audio_simple_player_stop(s_asp); esp_audio_simple_player_destroy(s_asp); s_asp=NULL; }
    if (s_audio_inited.load(std::memory_order_acquire)) { PeripheralManager::instance().deinit_audio(); s_audio_inited.store(false, std::memory_order_release); }
    audio_unlock();

    /* Signal task to exit its idle loop — it will clean up HTTP server
     * and mDNS from within its own context, then self-delete. */
    s_running = false;

    /* Wait for task to self-delete (max 3s).
     * Task clears s_task_handle before vTaskDelete(NULL), so we just
     * check the handle — no need for eTaskGetState() which races with
     * the idle task reclaiming the TCB. */
    int timeout = 0;
    while (s_task_handle.load(std::memory_order_acquire) && timeout < 30) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout++;
    }
    TaskHandle_t h = s_task_handle.exchange(nullptr, std::memory_order_acq_rel);
    if (h != nullptr) {
        ESP_LOGW(TAG, "Web config task did not exit, force-killing");
        vTaskDelete(h);
    }

    /* Fallback: if task already exited but HTTP/mDNS not cleaned up */
    if (s_mdns_running) {
        mdns_service_remove("_http", "_tcp");
        shared_mdns_release();
        s_mdns_running = false;
    }
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_audio_mutex) {
        vSemaphoreDelete(s_audio_mutex);
        s_audio_mutex = NULL;
    }
    if (s_nvs_cache_mutex) {
        vSemaphoreDelete(s_nvs_cache_mutex);
        s_nvs_cache_mutex = NULL;
    }
    /* TCB is pre-allocated and never freed — ~340B internal SRAM,
     * negligible cost for permanent use in embedded firmware.
     * Avoids TCB use-after-free race with idle task entirely. */
    if (s_wifi_state_sub >= 0) {
        orb_unsubscribe(s_wifi_state_sub);
        s_wifi_state_sub = -1;
    }

    ESP_LOGI(TAG, "Web config server stopped");
}
