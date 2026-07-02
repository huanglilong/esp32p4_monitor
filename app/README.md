# ESP32-P4 Viewer App

Cross-platform Flutter app for receiving camera feeds and sensor data from **ESP32-P4** over WiFi.

## Supported Platforms

- ✅ macOS (Desktop)
- ✅ Linux (Desktop)
- ✅ iOS (iPhone/iPad)
- ✅ Android (Phone/Tablet)

## Architecture

```
┌─────────────────────────────────┐
│       Flutter App (本工程)       │
│  ┌──────────┐ ┌──────────────┐ │
│  │ mDNS发现  │ │ WebSocket    │ │
│  │ (UDP多播) │ │ 数据流处理    │ │
│  └─────┬────┘ └──────┬───────┘ │
│        │             │         │
│  ┌─────┴─────────────┴──────┐  │
│  │   图像渲染 + 文字显示      │  │
│  └──────────────────────────┘  │
│  ┌──────────────────────────┐  │
│  │ macOS / iOS / Android    │  │
│  └──────────────────────────┘  │
└──────────────┬─────────────────┘
               │ WiFi (局域网)
               ▼
┌─────────────────────────────────┐
│         ESP32-P4                │
│  摄像头 → JPEG压缩 → WebSocket  │
│  传感器数据 → JSON 消息推送      │
└─────────────────────────────────┘
```

## 快速开始

### 1. 生成平台项目文件

```bash
# 克隆到临时目录，复制 platform 文件夹到本工程
cd /tmp
flutter create --org com.esp32p4 --project-name esp32p4_app \
  --platforms ios,android,macos esp32p4_platform
cp -r esp32p4_platform/ios esp32p4_platform/android esp32p4_platform/macos \
  /path/to/esp32p4_app/
```

### 2. 运行

```bash
flutter pub get
flutter run -d macos   # macOS
flutter run -d linux   # Linux
flutter run -d ios     # iOS (需连接iPhone)
flutter run -d android # Android (需连接设备)
```

### 3. ESP32-P4 固件

`firmware/` 目录下有 ESP32-P4 的 Arduino 示例代码，支持：
- 摄像头 JPEG 采集
- WebSocket 实时推送
- 双向通信

## 通信协议

### WebSocket 消息格式

**设备 → App (JSON):**
```json
{"type": "frame", "data": "<base64-jpeg>"}
{"type": "text",  "data": "Temperature: 28.5°C"}
{"type": "status","data": "running"}
```

**App → 设备 (JSON):**
```json
{"command": "set_resolution", "value": "640x480"}
```

## 项目结构

```
lib/
├── main.dart                    # 入口 + InheritedWidget provider
├── models/
│   ├── esp32_device.dart        # 设备模型
│   └── esp32_message.dart       # 消息模型
├── services/
│   ├── websocket_service.dart   # WebSocket 连接管理
│   └── device_discovery.dart    # mDNS + HTTP 设备发现
├── providers/
│   └── app_state.dart           # 全局状态管理
├── screens/
│   ├── home_screen.dart         # 设备列表/发现页
│   └── camera_screen.dart       # 摄像头画面全屏页
└── widgets/
    ├── device_card.dart         # 设备卡片组件
    └── image_viewer.dart        # JPEG 图像渲染组件
```
