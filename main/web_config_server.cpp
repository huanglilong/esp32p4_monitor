/*
 * Web Config Server — HTTP-based settings UI with mDNS discovery.
 *
 * Provides a web page to configure WiFi credentials, speaker volume,
 * and other settings normally set through the LCD UI on LCD-4B boards.
 * All settings are persisted to NVS namespace "settings".
 *
 * Serves on port 8080 on both boards (coexists with CameraStream port 80/81).
 * Advertised via mDNS as esp-web.local (shared hostname with CameraStream).
 *
 * Assumption: NVS already contains WiFi SSID/password.
 * The device must already be connected to a network before the web UI
 * is reachable.  (TODO: AP-mode provisioning for first-boot — see PROJECT.md)
 */

#include "web_config_server.hpp"
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "esp_log.h"
#include "esp_err.h"
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

static const char *TAG = "WebConfig";

/* Extern: set by main.cpp after GT911 I2C probe */
#include "esp_codec_dev.h"

extern bool g_has_lcd;
extern esp_codec_dev_handle_t s_codec_handle;
extern SemaphoreHandle_t s_codec_mutex;

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
static bool           s_running = false;
static bool           s_mdns_running = false;

/*============================================================================
 * NVS Helpers
 *============================================================================*/
static int32_t nvs_get_i32_def(const char *key, int32_t def)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return def;
    int32_t val = def;
    nvs_get_i32(h, key, &val);
    nvs_close(h);
    return val;
}

static void nvs_set_i32(const char *key, int32_t value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, key, value);
    nvs_commit(h);
    nvs_close(h);
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
"showStatus(j.running?'Stream started':'Stream stopped','success')}"
"else{showStatus(j.error||'Failed','error');loadStatus()}}"
"catch(e){showStatus('Connection error','error');loadStatus()}}"
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
static EventGroupHandle_t s_wifi_evt_group = NULL;

static void _wifi_evt_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (s_wifi_evt_group) xEventGroupSetBits(s_wifi_evt_group, WIFI_CONNECTED_BIT);
    }
}

static void _wifi_evt_ensure_listening(void)
{
    if (s_wifi_evt_group) return;
    s_wifi_evt_group = xEventGroupCreate();
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               _wifi_evt_handler, NULL);
}

static bool wifi_sta_is_connected(void)
{
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
        /* Apply to codec immediately (thread-safe via mutex) */
        if (s_codec_handle && s_codec_mutex &&
            xSemaphoreTake(s_codec_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            esp_codec_dev_set_out_vol(s_codec_handle, vol);
            xSemaphoreGive(s_codec_mutex);
        }
    }

    /* WiFi: try connecting first, save to NVS only on success */
    bool wifi_ok = true;  /* default true if no WiFi change */
    bool need_reconnect = (j_wifi_en && j_wifi_en->valueint != 0
                           && j_ssid && cJSON_IsString(j_ssid)
                           && j_ssid->valuestring && strlen(j_ssid->valuestring) > 0);
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

        /* Wait for connection result */
        _wifi_evt_ensure_listening();
        xEventGroupClearBits(s_wifi_evt_group, WIFI_CONNECTED_BIT);
        EventBits_t bits = xEventGroupWaitBits(s_wifi_evt_group, WIFI_CONNECTED_BIT,
                                               pdFALSE, pdTRUE,
                                               pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
        if (bits & WIFI_CONNECTED_BIT) {
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
    } else if (j_wifi_en) {
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
    /* Wait for WiFi connection before starting HTTP server.
     * LWIP TCPIP mbox is only valid after netif is up. */
    ESP_LOGI(TAG, "Web config waiting for WiFi connection...");

    _wifi_evt_ensure_listening();
    xEventGroupClearBits(s_wifi_evt_group, WIFI_CONNECTED_BIT);

    /* Check if already connected (event may have fired before this task) */
    if (!wifi_sta_is_connected()) {
        ESP_LOGI(TAG, "Waiting for IP_EVENT_STA_GOT_IP...");
        xEventGroupWaitBits(s_wifi_evt_group, WIFI_CONNECTED_BIT,
                            pdFALSE, pdTRUE, portMAX_DELAY);
    }

    ESP_LOGI(TAG, "WiFi connected, starting web config server on port %d...",
             WEB_CONFIG_PORT);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_CONFIG_PORT;
    config.ctrl_port   = WEB_CONFIG_PORT + 1;  /* 8081 — avoid collision with CameraStream ctrl=32768 */
    config.max_uri_handlers = 11;
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
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("esp-web");
        mdns_instance_name_set("esp-web-config");

        netbiosns_init();
        netbiosns_set_name("esp-web");

        mdns_txt_item_t txt[] = {
            {(char *)"board", (char *)CONFIG_IDF_TARGET},
            {(char *)"path",  (char *)"/"},
        };
        mdns_service_add("ESP32-WebConfig", "_http", "_tcp", WEB_CONFIG_PORT,
                         txt, sizeof(txt) / sizeof(txt[0]));
        s_mdns_running = true;
        ESP_LOGI(TAG, "mDNS: esp-web.local:%d (NetBIOS: esp-web)", WEB_CONFIG_PORT);
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

    /* Idle — HTTP server runs in its own internal threads */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
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
    if (s_mdns_running) {
        mdns_free();
        s_mdns_running = false;
    }
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_task_handle) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }
    s_running = false;
    ESP_LOGI(TAG, "Web config server stopped");
}
