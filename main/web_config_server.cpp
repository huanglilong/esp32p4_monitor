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
#include <sys/time.h>
#include "driver/i2s_std.h"
#include "esp_audio_simple_player.h"
extern "C" {
#include "layer3.h"
}

static const char *TAG = "WebConfig";

/* Board detection — still extern until fully migrated */
extern bool g_has_lcd;

#define WEB_CONFIG_PORT         8080

#define NVS_NAMESPACE           "settings"
#define NVS_KEY_WIFI_EN         "wifi_en"
#define NVS_KEY_WIFI_SSID       "ssid"
#define NVS_KEY_WIFI_PASS       "pass"
#define NVS_KEY_VOLUME          "volume"
#define NVS_KEY_CAM_STREAM      "cam_stream"

#define VOLUME_MIN              0
#define VOLUME_MAX              100
#define VOLUME_DEFAULT          60

#define TASK_STACK_SIZE         (4 * 1024)
#define TASK_PRIORITY           1
#define WIFI_CONNECT_TIMEOUT_MS 15000  /* Max wait for STA connection before giving up */
#define WIFI_CONNECTED_BIT      BIT0

static httpd_handle_t s_httpd = NULL;
static TaskHandle_t   s_task_handle = NULL;
static volatile bool  s_running = false;
static bool           s_mdns_running = false;

/*============================================================================
 * Audio state — lazy-init when camera stream is OFF
 *============================================================================*/
#define REC_DIR              "/sdcard"
#define REC_BUF_SAMPLES      480
#define REC_BUF_BYTES        (REC_BUF_SAMPLES * 2 * sizeof(int16_t))
#define ENC_SAMPLES_PER_CH   1152
#define PCM_BUF_SAMPLES      (ENC_SAMPLES_PER_CH * 2)

static bool           s_audio_inited = false;
static TaskHandle_t   s_audio_task = NULL;
static volatile bool  s_audio_running = false;
static volatile bool  s_is_recording = false;
static shine_t        s_shine = NULL;
static int16_t       *s_pcm_buf = NULL;
static int            s_pcm_count = 0;
static FILE          *s_rec_file = NULL;
static uint32_t       s_rec_bytes = 0;
static uint32_t       s_rec_start_ms = 0;
static char           s_rec_path[128];

/* Playback */
static esp_asp_handle_t s_asp = NULL;
static volatile bool    s_playing = false;

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
    if (!s_audio_task && !s_audio_running) {
        return;
    }

    s_audio_running = false;
    for (int i = 0; i < 10 && s_audio_task; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (s_audio_task) {
        ESP_LOGW(TAG, "Audio task did not exit in time, force deleting");
        vTaskDelete(s_audio_task);
        s_audio_task = NULL;
    }
}

/* Audio recording task: I2S RX → shine MP3 → SD card */
static void audio_task(void *arg)
{
    (void)arg;
    int16_t *buf = (int16_t *)calloc(1, REC_BUF_BYTES);
    if (!buf) { s_audio_running = false; s_audio_task = NULL; vTaskDelete(NULL); return; }
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
                s_pcm_buf[s_pcm_count * 2]     = buf[i * 2];
                s_pcm_buf[s_pcm_count * 2 + 1] = buf[i * 2 + 1];
                if (++s_pcm_count >= ENC_SAMPLES_PER_CH) {
                    int wr = 0;
                    unsigned char *mp3 = shine_encode_buffer_interleaved(s_shine, s_pcm_buf, &wr);
                    if (mp3 && wr > 0 && s_rec_file) s_rec_bytes += fwrite(mp3, 1, wr, s_rec_file);
                    s_pcm_count = 0;
                }
            }
        }
    }
    free(buf);
    s_audio_task = NULL;
    vTaskDelete(NULL);
}

/*============================================================================
 * NVS Helpers — with RAM cache for frequently-read integer keys.
 *
 * Inspired by PX4's layered parameter system: read from RAM cache (O(1)),
 * fall through to NVS on cache miss, and invalidate on write.
 * This avoids ~1ms flash access per nvs_get_i32_def() call.
 *============================================================================*/

/** Cached NVS integer entry. */
typedef struct {
    const char *key;
    int32_t     value;
    bool        valid;    /* true after first read from NVS */
} nvs_cache_entry_t;

#define NVS_CACHE_MAX 8
static nvs_cache_entry_t s_nvs_cache[NVS_CACHE_MAX];
static int s_nvs_cache_count;

/** Find or create a cache slot for the given key. Returns slot index, or -1. */
static int nvs_cache_find(const char *key)
{
    for (int i = 0; i < s_nvs_cache_count; i++) {
        if (strcmp(s_nvs_cache[i].key, key) == 0) return i;
    }
    if (s_nvs_cache_count < NVS_CACHE_MAX) {
        int i = s_nvs_cache_count++;
        s_nvs_cache[i].key   = key;
        s_nvs_cache[i].valid = false;
        return i;
    }
    return -1;
}

static int32_t nvs_get_i32_def(const char *key, int32_t def)
{
    /* Check RAM cache first (O(1), no flash access) */
    int ci = nvs_cache_find(key);
    if (ci >= 0 && s_nvs_cache[ci].valid) {
        return s_nvs_cache[ci].value;
    }

    /* Cache miss: read from NVS */
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return def;
    int32_t val = def;
    nvs_get_i32(h, key, &val);
    nvs_close(h);

    /* Populate cache */
    if (ci >= 0) {
        s_nvs_cache[ci].value = val;
        s_nvs_cache[ci].valid = true;
    }
    return val;
}

static void nvs_set_i32(const char *key, int32_t value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, key, value);
    nvs_commit(h);
    nvs_close(h);

    /* Update cache (write-through) */
    int ci = nvs_cache_find(key);
    if (ci >= 0) {
        s_nvs_cache[ci].value = value;
        s_nvs_cache[ci].valid = true;
    }
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
"justify-content:center;padding:16px}"
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
"<div class=\"container\">"
"<h1>ESP32-P4 Configs</h1>"
"<div class=\"card\">"
"<h2>WiFi Settings</h2>"
"<div class=\"toggle-row\">"
"<span>WiFi Enable</span>"
"<label class=\"toggle\">"
"<input type=\"checkbox\" id=\"wifi_en\" onchange=\"updateUI()\">"
"<span class=\"slider\"></span></label></div>"
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
"<h2>Audio Recorder</h2>"
"<div style=\"display:flex;gap:8px;margin-bottom:8px\">"
"<button id=\"btn_rec\" onclick=\"onRecord()\" style=\"flex:1\">Start Record</button>"
"<button id=\"btn_stop\" onclick=\"onStopAll()\" style=\"flex:0 0 auto;width:auto;padding:12px 16px;background:#e65100;display:none\">Stop</button></div>"
"<div id=\"rec_stat\" style=\"font-size:12px;color:#4caf50;margin-bottom:8px;min-height:16px\"></div>"
"<div id=\"file_list\" style=\"font-size:13px;max-height:160px;overflow-y:auto\"></div>"
"</div>"
"<div class=\"card\">"
"<h2>Volume</h2>"
"<div class=\"slider-container\">"
"<input type=\"range\" id=\"volume\" min=\"0\" max=\"100\" value=\"60\" oninput=\"updateVol()\">"
"<span class=\"slider-val\" id=\"vol_val\">60</span></div>"
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
"document.getElementById('ssid').disabled=!document.getElementById('wifi_en').checked;"
"document.getElementById('pass').disabled=!document.getElementById('wifi_en').checked}"
"async function loadStatus(){"
"try{let r=await fetch('/api/status');let j=await r.json();"
"document.getElementById('wifi_en').checked=j.wifi_en!=0;"
"document.getElementById('ssid').value=j.ssid||'';"
"document.getElementById('pass').placeholder=j.has_pass?'(saved)':'WiFi password';"
"document.getElementById('pass').value='';"
"document.getElementById('volume').value=j.volume||60;"
"document.getElementById('vol_val').textContent=j.volume||60;"
"document.getElementById('cam_stream').checked=j.cam_stream!=0;"
"let cs=document.getElementById('cam_status');"
"cs.textContent=j.cam_running?'● Streaming active':'○ Stopped';"
"cs.style.color=j.cam_running?'#4caf50':'#a0a0b0';"
"document.getElementById('audio_card').style.display=j.cam_running?'none':'block';"
"if(!j.cam_running)loadFiles();"
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
"cs.textContent=j.running?'● Streaming active':'○ Stopped';"
"cs.style.color=j.running?'#4caf50':'#a0a0b0';"
"document.getElementById('audio_card').style.display=j.running?'none':'block';"
"if(!j.running)loadFiles();"
"showStatus(j.running?'Stream started':'Stream stopped','success')}"
"else{showStatus(j.error||'Failed','error');loadStatus()}}"
"catch(e){showStatus('Connection error','error');loadStatus()}}"
"var recTimer=null;"
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
"showStatus('Recording...','success');recTimer=setInterval(refreshRec,500)}"
"else showStatus('Error: '+j.error,'error')}"
"catch(e){showStatus('Error','error')}}else endRecord()}"
"async function endRecord(){"
"try{var r=await fetch('/api/audio/record_stop');var j=await r.json();"
"var b=document.getElementById('btn_rec');"
"b.textContent='Start Record';b.style.background='';"
"clearInterval(recTimer);"
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
"async function saveSettings(){"
"let data={wifi_en:document.getElementById('wifi_en').checked?1:0,"
"ssid:document.getElementById('ssid').value,"
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
"loadStatus();"
"</script></body></html>";

/*============================================================================
 * WiFi Event Handler (used by settings_handler for connection verification)
 *============================================================================*/
/* WiFi state from uORB — published by PhoneAppSettings */
static orb_sub_t s_wifi_state_sub = -1;

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
    /* Fallback: direct WiFi check */
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) return false;
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
    int32_t wifi_en = nvs_get_i32_def(NVS_KEY_WIFI_EN, 0);
    int32_t volume  = nvs_get_i32_def(NVS_KEY_VOLUME, VOLUME_DEFAULT);
    int32_t cam_en = g_has_lcd ? 0 : nvs_get_i32_def(NVS_KEY_CAM_STREAM, 0);
    bool cam_running = g_has_lcd ? false : CameraStream::instance().isRunning();

    /* Build JSON safely with cJSON — avoids injection from SSID special chars */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "wifi_en", wifi_en);
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddBoolToObject(root, "has_pass", strlen(pass) > 0);  /* Never expose plaintext password */
    cJSON_AddNumberToObject(root, "volume", volume);
    cJSON_AddNumberToObject(root, "cam_stream", cam_en);
    cJSON_AddBoolToObject(root, "cam_running", cam_running);

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

    cJSON *j_wifi_en = cJSON_GetObjectItem(root, "wifi_en");
    cJSON *j_ssid    = cJSON_GetObjectItem(root, "ssid");
    cJSON *j_pass    = cJSON_GetObjectItem(root, "pass");
    cJSON *j_volume  = cJSON_GetObjectItem(root, "volume");

    /* Log old vs new values for debugging */
    {
        int32_t old_wifi_en = nvs_get_i32_def(NVS_KEY_WIFI_EN, -1);
        int32_t old_volume  = nvs_get_i32_def(NVS_KEY_VOLUME, -1);
        char old_ssid[33] = {}; nvs_get_str(NVS_KEY_WIFI_SSID, old_ssid, sizeof(old_ssid));
        char old_pass[65] = {}; nvs_get_str(NVS_KEY_WIFI_PASS, old_pass, sizeof(old_pass));

        int32_t new_wifi_en = j_wifi_en ? j_wifi_en->valueint : old_wifi_en;
        const char *new_ssid = (j_ssid && cJSON_IsString(j_ssid) && j_ssid->valuestring)
                               ? j_ssid->valuestring : old_ssid;
        const char *new_pass = (j_pass && cJSON_IsString(j_pass) && j_pass->valuestring)
                               ? j_pass->valuestring : old_pass;
        int32_t new_volume = j_volume ? j_volume->valueint : old_volume;
        if (new_volume < VOLUME_MIN) new_volume = VOLUME_MIN;
        if (new_volume > VOLUME_MAX) new_volume = VOLUME_MAX;

        ESP_LOGI(TAG, "Settings requested: wifi_en=%ld->%ld, ssid=\"%s\"->\"%s\", "
                 "pass_len=%d->%d, volume=%ld->%ld",
                 old_wifi_en, new_wifi_en,
                 old_ssid, new_ssid,
                 (int)strlen(old_pass), (int)strlen(new_pass),
                 old_volume, new_volume);
    }

    /* Volume: save to NVS AND apply to hardware immediately */
    if (j_volume) {
        int32_t vol = j_volume->valueint;
        if (vol < VOLUME_MIN) vol = VOLUME_MIN;
        if (vol > VOLUME_MAX) vol = VOLUME_MAX;
        nvs_set_i32(NVS_KEY_VOLUME, vol);
        /* Apply to codec immediately (thread-safe via PeripheralManager) */
        PeripheralManager::instance().set_volume((int)vol);
    }

    /* WiFi: skip if ssid or password is explicitly provided but empty */
    bool skip_wifi = false;
    if (j_ssid && cJSON_IsString(j_ssid) && j_ssid->valuestring &&
        strlen(j_ssid->valuestring) == 0) {
        ESP_LOGW(TAG, "WiFi SSID is empty — skipping WiFi settings");
        skip_wifi = true;
    }
    if (j_pass && cJSON_IsString(j_pass) && j_pass->valuestring &&
        strlen(j_pass->valuestring) == 0) {
        ESP_LOGW(TAG, "WiFi password is empty — skipping WiFi settings");
        skip_wifi = true;
    }

    /* WiFi: try connecting first, save to NVS only on success */
    bool wifi_ok = true;  /* default true if no WiFi change */
    bool need_reconnect = !skip_wifi
                          && j_wifi_en && j_wifi_en->valueint != 0
                          && j_ssid && cJSON_IsString(j_ssid)
                          && j_ssid->valuestring && strlen(j_ssid->valuestring) > 0;
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
            nvs_set_i32(NVS_KEY_WIFI_EN, 1);
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
    } else if (j_wifi_en && !skip_wifi) {
        nvs_set_i32(NVS_KEY_WIFI_EN, j_wifi_en->valueint);
        if (j_wifi_en->valueint == 0) {
            /* WiFi OFF: disconnect and stop WiFi hardware immediately */
            ESP_LOGI(TAG, "WiFi OFF requested — stopping WiFi");
            esp_wifi_disconnect();
            esp_wifi_stop();
        } else {
            /* WiFi ON (no new SSID): start WiFi and connect with saved credentials */
            ESP_LOGI(TAG, "WiFi ON requested — connecting with saved credentials");
            char saved_ssid[33] = {};
            char saved_pass[65] = {};
            nvs_get_str(NVS_KEY_WIFI_SSID, saved_ssid, sizeof(saved_ssid));
            nvs_get_str(NVS_KEY_WIFI_PASS, saved_pass, sizeof(saved_pass));
            if (strlen(saved_ssid) > 0) {
                esp_wifi_start();
                vTaskDelay(pdMS_TO_TICKS(500));
                wifi_config_t wifi_cfg = {};
                strlcpy((char *)wifi_cfg.sta.ssid, saved_ssid, sizeof(wifi_cfg.sta.ssid));
                if (strlen(saved_pass) > 0)
                    strlcpy((char *)wifi_cfg.sta.password, saved_pass, sizeof(wifi_cfg.sta.password));
                esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
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
    if (g_has_lcd) {
        /* LCD-4B: Camera Stream managed by Phone App, not available via web */
        const char *resp = "{\"ok\":0,\"error\":\"Use Camera Stream App on the display\"}";
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
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
        }
    } else {
        if (CameraStream::instance().isRunning()) {
            ESP_LOGI(TAG, "Stopping camera stream...");
            CameraStream::instance().stop();
        }
    }

    /* Persist intent to NVS */
    nvs_set_i32(NVS_KEY_CAM_STREAM, enable ? 1 : 0);

    cJSON_Delete(root);

    bool running = CameraStream::instance().isRunning();
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"ok\":1,\"enabled\":%s,\"running\":%s}",
             enable ? "true" : "false", running ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Factory reset requested! Erasing NVS settings...");

    /* Invalidate RAM cache — all cached values are stale */
    for (int i = 0; i < s_nvs_cache_count; i++) {
        s_nvs_cache[i].valid = false;
    }

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

    return ESP_OK;
}

/*============================================================================
 * Audio Handlers — record / play / list mp3 files on SD card
 *============================================================================*/
static bool __cam_running(void) {
    extern bool g_has_lcd;
    return (!g_has_lcd) && CameraStream::instance().isRunning();
}

/* Lazy-init SD card + audio codec on headless boards */
static bool __audio_init(void) {
    if (s_audio_inited) return true;
    if (!PeripheralManager::instance().init_sdcard()) { ESP_LOGE(TAG,"SD init fail"); return false; }
    PeripheralManager::instance().init_audio();
    s_audio_inited = true;
    return true;
}

/* GET /api/audio/record_start */
static esp_err_t h_rec_start(httpd_req_t *req) {
    if (__cam_running()) { httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Camera running\"}"); return ESP_OK; }
    audio_lock();
    if (s_is_recording)   { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":1}"); return ESP_OK; }
    if (!__audio_init())  { audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Init fail\"}"); return ESP_OK; }
    if (!s_audio_task) {
        s_audio_running = true;
        if (xTaskCreatePinnedToCore(audio_task, "w_audio", 12*1024, NULL, 1, &s_audio_task, 0) != pdPASS)
            { s_audio_running = false; audio_unlock(); httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK; }
    }
    s_pcm_buf = (int16_t*)heap_caps_calloc(1, PCM_BUF_SAMPLES*sizeof(int16_t), MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if (!s_pcm_buf) {
        _stop_audio_task_if_running();
        audio_unlock();
        httpd_resp_sendstr(req, "{\"ok\":0}");
        return ESP_OK;
    }
    s_pcm_count = 0;
    shine_config_t c; shine_set_config_mpeg_defaults(&c.mpeg);
    c.wave.channels = PCM_STEREO; c.wave.samplerate = 48000; c.mpeg.mode = STEREO; c.mpeg.bitr = 128;
    s_shine = shine_initialise(&c);
    if (!s_shine) {
        free(s_pcm_buf);
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
        snprintf(s_rec_path, sizeof(s_rec_path), REC_DIR "/rec_%04d%02d%02d_%02d%02d%02d.mp3",
                 tm_buf.tm_year+1900, tm_buf.tm_mon+1, tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    } else {
        uint32_t mono_ms = (uint32_t)(esp_timer_get_time() / 1000);
        snprintf(s_rec_path, sizeof(s_rec_path), REC_DIR "/rec_%lu.mp3", (unsigned long)mono_ms);
    }
    s_rec_file = fopen(s_rec_path, "wb");
    if (!s_rec_file) { shine_close(s_shine); s_shine = NULL; free(s_pcm_buf); s_pcm_buf = NULL;
        _stop_audio_task_if_running();
        httpd_resp_sendstr(req, "{\"ok\":0}"); return ESP_OK; }
    s_rec_bytes = 0; s_rec_start_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_is_recording = true;
    audio_unlock();
    ESP_LOGI(TAG, "Recording: %s", s_rec_path);
    httpd_resp_sendstr(req, "{\"ok\":1}");
    return ESP_OK;
}

/* GET /api/audio/record_stop */
static esp_err_t h_rec_stop(httpd_req_t *req) {
    if (__cam_running()) { httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Camera running\"}"); return ESP_OK; }
    audio_lock();
    if (!s_is_recording) {
        /* Recover from stale state if a previous record_start failed mid-way. */
        _stop_audio_task_if_running();
        if (s_shine) { shine_close(s_shine); s_shine = NULL; }
        if (s_pcm_buf) { free(s_pcm_buf); s_pcm_buf = NULL; }
        if (s_rec_file) { fclose(s_rec_file); s_rec_file = NULL; }
        audio_unlock();
        httpd_resp_sendstr(req, "{\"ok\":1}");
        return ESP_OK;
    }
    s_is_recording = false;
    audio_unlock();  /* Release lock before blocking _stop_audio_task_if_running */
    _stop_audio_task_if_running(); /* Stop audio task to release I2S RX */
    audio_lock();
    FILE *f = s_rec_file; s_rec_file = NULL;
    if (s_shine) { int wr=0; unsigned char *d=shine_flush(s_shine, &wr); if(d&&wr>0&&f) fwrite(d,1,wr,f); shine_close(s_shine); s_shine=NULL; }
    if (f) fclose(f);
    if (s_pcm_buf) { free(s_pcm_buf); s_pcm_buf=NULL; }
    audio_unlock();
    char r[256]; snprintf(r,sizeof(r),"{\"ok\":1,\"file\":\"%s\",\"bytes\":%lu}", s_rec_path, (unsigned long)s_rec_bytes);
    ESP_LOGI(TAG,"Saved: %s (%lu)", s_rec_path, (unsigned long)s_rec_bytes);
    httpd_resp_send(req, r, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /api/audio/record_status */
static esp_err_t h_rec_status(httpd_req_t *req) {
    if (__cam_running()) { httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Camera running\"}"); return ESP_OK; }
    char r[128];
    if (s_is_recording) {
        uint32_t e = (uint32_t)((esp_timer_get_time() / 1000 - s_rec_start_ms) / 1000);
        snprintf(r,sizeof(r),"{\"recording\":1,\"seconds\":%lu,\"bytes\":%lu}",(unsigned long)e,(unsigned long)s_rec_bytes); }
    else snprintf(r,sizeof(r),"{\"recording\":0}");
    httpd_resp_send(req, r, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /api/audio/list */
static esp_err_t h_list(httpd_req_t *req) {
    if (__cam_running()) { httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Camera running\"}"); return ESP_OK; }
    (void)__audio_init(); /* Lazy-mount SD card so file list works even before first record */
    DIR *d=opendir(REC_DIR); cJSON *root=cJSON_CreateObject(),*arr=cJSON_CreateArray();
    cJSON_AddItemToObject(root,"files",arr);
    if(d){ struct dirent *e; while((e=readdir(d))){ if(e->d_name[0]=='.') continue; char *x=strrchr(e->d_name,'.'); if(x&&strcasecmp(x,".mp3")==0) cJSON_AddItemToArray(arr,cJSON_CreateString(e->d_name)); } closedir(d); }
    char *j=cJSON_PrintUnformatted(root); httpd_resp_set_type(req,"application/json"); httpd_resp_send(req,j,strlen(j)); free(j); cJSON_Delete(root);
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
    if(__cam_running()){ httpd_resp_sendstr(req,"{\"ok\":0,\"error\":\"Camera running\"}"); return ESP_OK; }
    if(!__audio_init()){ httpd_resp_sendstr(req,"{\"ok\":0,\"error\":\"Init fail\"}"); return ESP_OK; }
    char q[256]={},fn[128]={};
    if(httpd_req_get_url_query_str(req,q,sizeof(q))!=ESP_OK||!strlen(q)){ httpd_resp_sendstr(req,"{\"ok\":0}"); return ESP_OK; }
    httpd_query_key_value(q,"file",fn,sizeof(fn));
    if(!strlen(fn)){ httpd_resp_sendstr(req,"{\"ok\":0}"); return ESP_OK; }
    _url_decode(fn);
    char uri[160]; snprintf(uri,sizeof(uri),"file://" REC_DIR "/%s",fn);
    audio_lock();
    /* Stop + destroy previous player for clean state (matching Music App lifecycle) */
    if (s_asp) {
        esp_audio_simple_player_stop(s_asp);
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_audio_simple_player_destroy(s_asp);
        s_asp = NULL;
    }
    s_playing = false;
    esp_asp_cfg_t c={.out={.cb=_asp_out},.task_prio=5,.task_stack=4096,.task_core=0};
    if(esp_audio_simple_player_new(&c,&s_asp)!=ESP_GMF_ERR_OK||!s_asp){ audio_unlock(); httpd_resp_sendstr(req,"{\"ok\":0}"); return ESP_OK; }
    esp_audio_simple_player_set_event(s_asp,_asp_evt,NULL);
    esp_gmf_err_t ret = esp_audio_simple_player_run(s_asp, uri, NULL);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Play failed: %d, uri=%s", ret, uri);
        audio_unlock();
        httpd_resp_sendstr(req,"{\"ok\":0}"); return ESP_OK;
    }
    s_playing=true; audio_unlock(); ESP_LOGI(TAG,"Play: %s",uri);
    httpd_resp_sendstr(req,"{\"ok\":1}"); return ESP_OK;
}

/* GET /api/audio/stop */
static esp_err_t h_stop(httpd_req_t *req) {
    if (__cam_running()) { httpd_resp_sendstr(req, "{\"ok\":0,\"error\":\"Camera running\"}"); return ESP_OK; }
    audio_lock();
    if(s_asp){ esp_audio_simple_player_stop(s_asp); s_playing=false; }
    audio_unlock();
    httpd_resp_sendstr(req,"{\"ok\":1}"); return ESP_OK;
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

    /* Wait for WiFi connection before starting HTTP server.
     * LWIP TCPIP mbox is only valid after netif is up. */
    ESP_LOGI(TAG, "Web config waiting for WiFi connection...");

    /* Check if already connected (event may have fired before this task) */
    if (!wifi_sta_is_connected()) {
        ESP_LOGI(TAG, "Waiting for wifi_state topic...");
        struct wifi_state_s ws = {};
        while (1) {
            if (s_wifi_state_sub >= 0) {
                orb_copy(ORB_ID(wifi_state), s_wifi_state_sub, &ws);
                if (ws.connected) break;
            }
            /* Also check directly in case uORB isn't set up yet */
            if (wifi_sta_is_connected()) break;
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    ESP_LOGI(TAG, "WiFi connected, starting web config server on port %d...",
             WEB_CONFIG_PORT);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_CONFIG_PORT;
    config.ctrl_port   = WEB_CONFIG_PORT + 1;  /* 8081 — avoid collision with CameraStream ctrl=32768 */
    config.max_uri_handlers = 16;  /* 5 core + 6 audio + 3 CORS + 2 spare */
    config.lru_purge_enable = true;

    if (httpd_start(&s_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %d", WEB_CONFIG_PORT);
        vTaskDelete(NULL);
        return;
    }

    httpd_uri_t uri_index = {
        .uri = "/", .method = HTTP_GET,
        .handler = index_handler, .user_ctx = NULL
    };
    httpd_uri_t uri_status = {
        .uri = "/api/status", .method = HTTP_GET,
        .handler = status_handler, .user_ctx = NULL
    };
    httpd_uri_t uri_settings = {
        .uri = "/api/settings", .method = HTTP_POST,
        .handler = settings_handler, .user_ctx = NULL
    };
    httpd_uri_t uri_cam = {
        .uri = "/api/camera_stream", .method = HTTP_POST,
        .handler = camera_stream_handler, .user_ctx = NULL
    };
    httpd_uri_t uri_reset = {
        .uri = "/api/factory_reset", .method = HTTP_POST,
        .handler = factory_reset_handler, .user_ctx = NULL
    };

    httpd_register_uri_handler(s_httpd, &uri_index);
    httpd_register_uri_handler(s_httpd, &uri_status);
    httpd_register_uri_handler(s_httpd, &uri_settings);
    httpd_register_uri_handler(s_httpd, &uri_cam);
    httpd_register_uri_handler(s_httpd, &uri_reset);

    /* Audio recording / playback */
    httpd_uri_t uri_r_start = { .uri = "/api/audio/record_start", .method = HTTP_GET, .handler = h_rec_start };
    httpd_uri_t uri_r_stop  = { .uri = "/api/audio/record_stop",  .method = HTTP_GET, .handler = h_rec_stop };
    httpd_uri_t uri_r_stat  = { .uri = "/api/audio/record_status",.method = HTTP_GET, .handler = h_rec_status };
    httpd_uri_t uri_a_list  = { .uri = "/api/audio/list",         .method = HTTP_GET, .handler = h_list };
    httpd_uri_t uri_a_play  = { .uri = "/api/audio/play",          .method = HTTP_GET, .handler = h_play };
    httpd_uri_t uri_a_stop  = { .uri = "/api/audio/stop",          .method = HTTP_GET, .handler = h_stop };
    httpd_register_uri_handler(s_httpd, &uri_r_start);
    httpd_register_uri_handler(s_httpd, &uri_r_stop);
    httpd_register_uri_handler(s_httpd, &uri_r_stat);
    httpd_register_uri_handler(s_httpd, &uri_a_list);
    httpd_register_uri_handler(s_httpd, &uri_a_play);
    httpd_register_uri_handler(s_httpd, &uri_a_stop);

    /* CORS preflight (OPTIONS) handlers for POST endpoints */
    httpd_uri_t uri_cors_settings = {
        .uri = "/api/settings", .method = HTTP_OPTIONS,
        .handler = cors_preflight_handler, .user_ctx = NULL
    };
    httpd_uri_t uri_cors_cam = {
        .uri = "/api/camera_stream", .method = HTTP_OPTIONS,
        .handler = cors_preflight_handler, .user_ctx = NULL
    };
    httpd_uri_t uri_cors_reset = {
        .uri = "/api/factory_reset", .method = HTTP_OPTIONS,
        .handler = cors_preflight_handler, .user_ctx = NULL
    };
    httpd_register_uri_handler(s_httpd, &uri_cors_settings);
    httpd_register_uri_handler(s_httpd, &uri_cors_cam);
    httpd_register_uri_handler(s_httpd, &uri_cors_reset);

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
        ESP_LOGI(TAG, "mDNS: %s.local:%d (primary) + esp-web.local:%d (alias)", shared_mdns_hostname(), WEB_CONFIG_PORT, WEB_CONFIG_PORT);
    } else {
        ESP_LOGW(TAG, "mDNS init failed");
    }

    ESP_LOGI(TAG, "Web config server started on port %d", WEB_CONFIG_PORT);
    s_running = true;

    /* Auto-start camera stream if NVS says it was enabled (WIFI6 only) */
    if (!g_has_lcd && nvs_get_i32_def(NVS_KEY_CAM_STREAM, 0)) {
        ESP_LOGI(TAG, "NVS cam_stream=1, auto-starting camera stream...");
        CameraStream::instance().start();
    }

    /* Idle — HTTP server runs in its own internal threads.
     * Check s_running flag for clean exit when web_config_server_stop() is called. */
    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* Clean exit path: stop HTTP server and mDNS from within the task */
    if (s_mdns_running) {
        mdns_service_remove("_http", "_tcp");
        mdns_free();
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
    s_task_handle = NULL;
    vTaskDelete(NULL);
}

/*============================================================================
 * Public API
 *============================================================================*/
void web_config_server_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "Web config already running");
        return;
    }
    BaseType_t ret = xTaskCreate(
        web_config_task, "web_config", TASK_STACK_SIZE,
        NULL, TASK_PRIORITY, &s_task_handle);
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
    if (s_pcm_buf) { free(s_pcm_buf); s_pcm_buf=NULL; }
    s_playing = false;
    if (s_asp) { esp_audio_simple_player_stop(s_asp); esp_audio_simple_player_destroy(s_asp); s_asp=NULL; }
    if (s_audio_inited) { PeripheralManager::instance().deinit_audio(); PeripheralManager::instance().deinit_sdcard(); s_audio_inited=false; }
    audio_unlock();

    /* Signal task to exit its idle loop — it will clean up HTTP server
     * and mDNS from within its own context, then self-delete. */
    s_running = false;

    /* Wait for task to self-delete (max 3s) */
    int timeout = 0;
    while (s_task_handle && timeout < 30) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout++;
    }
    if (s_task_handle && eTaskGetState(s_task_handle) != eDeleted) {
        ESP_LOGW(TAG, "Web config task did not exit, force-killing");
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    } else if (s_task_handle) {
        /* Task already deleted — just clear handle */
        s_task_handle = NULL;
    }

    /* Fallback: if task already exited but HTTP/mDNS not cleaned up */
    if (s_mdns_running) {
        mdns_free();
        s_mdns_running = false;
    }
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }

    ESP_LOGI(TAG, "Web config server stopped");
}
