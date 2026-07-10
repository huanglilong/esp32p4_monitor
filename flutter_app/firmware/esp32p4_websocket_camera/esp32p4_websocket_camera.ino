/**
 * ESP32-P4 Camera + WebSocket Server
 *
 * 功能：
 *   1. 采集摄像头画面并压缩为 JPEG
 *   2. 通过 WebSocket 推送到局域网 App
 *   3. 支持文字/状态数据推送
 *
 * 硬件要求：
 *   - ESP32-P4 开发板
 *   - 摄像头模块 (OV2640/OV5640)
 *   - WiFi 连接
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <esp_camera.h>

// ===== 配置 =====
// ===== WiFi 配置（请修改为你自己的 WiFi） =====
// 如果你的 ESP32-P4 不支持代码中硬编码 WiFi 凭据，
// 也可以使用 WiFiManager 库来支持配网页面。
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// WebServer (HTTP) + WebSocket
WebServer server(80);
WebSocketsServer webSocket(81);  // WebSocket on port 81

// 摄像头引脚配置 (根据你的开发板修改)
// ESP32-P4 标准引脚映射
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     23
#define SIOC_GPIO_NUM     22
#define Y9_GPIO_NUM       5
#define Y8_GPIO_NUM       18
#define Y7_GPIO_NUM       19
#define Y6_GPIO_NUM       21
#define Y5_GPIO_NUM       13
#define Y4_GPIO_NUM       14
#define Y3_GPIO_NUM       25
#define Y2_GPIO_NUM       26
#define VSYNC_GPIO_NUM    27
#define HREF_GPIO_NUM     32
#define PCLK_GPIO_NUM     33

// 帧率控制
unsigned long lastFrameTime = 0;
const int frameInterval = 200;  // ms (~5 FPS)

void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== ESP32-P4 Camera WebSocket Server ===");

  // 1. 初始化摄像头
  if (!initCamera()) {
    Serial.println("Camera init FAILED! Rebooting...");
    delay(3000);
    ESP.restart();
  }
  Serial.println("Camera initialized OK");

  // 2. 连接 WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // 3. 启动 HTTP 服务 (用于探测)
  server.on("/", []() {
    server.send(200, "text/plain", "ESP32-P4 Camera Server");
  });
  server.begin();
  Serial.println("HTTP server started on port 80");

  // 4. 启动 WebSocket 服务
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  Serial.println("WebSocket server started on port 81");
}

void loop() {
  webSocket.loop();
  server.handleClient();

  // 定时推送摄像头帧
  unsigned long now = millis();
  if (now - lastFrameTime >= frameInterval) {
    lastFrameTime = now;
    sendCameraFrame();
  }
}

// ===== WebSocket 事件处理 =====
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected\n", num);
      break;

    case WStype_CONNECTED:
      Serial.printf("[%u] Connected from %s\n", num, webSocket.remoteIP(num).toString().c_str());
      // 发送欢迎 + 状态消息
      webSocket.sendTXT(num, "{\"type\":\"status\",\"data\":\"connected\"}");
      break;

    case WStype_TEXT:
      Serial.printf("[%u] RX: %s\n", num, (char*)payload);
      // 可以在这里处理来自 App 的命令
      break;
  }
}

// ===== 摄像头帧推送 =====
void sendCameraFrame() {
  if (webSocket.connectedClients() == 0) return;

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return;
  }

  // 推送二进制 JPEG 数据给所有连接的客户端
  webSocket.broadcastBIN(fb->buf, fb->len);

  esp_camera_fb_return(fb);
}

// ===== 传感器数据推送示例 =====
void sendSensorData(float temperature, float humidity) {
  if (webSocket.connectedClients() == 0) return;

  char buf[128];
  snprintf(buf, sizeof(buf),
    "{\"type\":\"text\",\"data\":\"Temperature: %.1f°C, Humidity: %.1f%%\"}",
    temperature, humidity);

  webSocket.broadcastTXT(buf);
}

// ===== 摄像头初始化 =====
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // 根据你的摄像头模组选择分辨率
  // OV2640: 最大 UXGA (1600x1200)
  // OV5640: 最大 QSXGA (2592x1944)
  config.frame_size   = FRAMESIZE_VGA;    // 640x480
  config.jpeg_quality = 12;               // 0-63, 越小质量越好
  config.fb_count     = 1;

  esp_err_t err = esp_camera_init(&config);
  return err == ESP_OK;
}
