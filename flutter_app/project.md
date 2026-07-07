# ESP32-P4 Viewer App — Project Notes

## Why

需要一个跨平台 App（macOS / iOS / Android），通过 WiFi 接收局域网 ESP32-P4 的摄像头实时图像和传感器数据。

### 技术选型

| 方案 | 选择理由 |
|------|---------|
| **Flutter** | 一套代码覆盖 macOS + iOS + Android |
| **零第三方依赖** | WebSocket 和 HTTP 都只需 `dart:io` 内置 API |
| **MJPEG over HTTP** | ESP32-P4 官方固件使用 MJPEG + chunked transfer 流 |

### 替代方案评估

| 方案 | 放弃理由 |
|------|---------|
| WebSocket | ESP32-P4 实际跑的是 ESP-IDF `simple_video_server`（MJPEG HTTP），不是 WebSocket |
| 轮询 `/api/capture_binary` | 每次请求竞争摄像头资源，ESP32 超时；改用 MJPEG 长连接 |
| 手动 dechunk | Dart `HttpClient` 已自动处理 chunked transfer，比自己解析更可靠 |

---

## How

### 架构

```
┌─────────────────────────────────────────────┐
│  Flutter App                                 │
│  ┌──────────────┐  ┌──────────────────────┐ │
│  │ DeviceDiscovery│  │ Esp32HttpService     │ │
│  │ · mDNS (_http) │  │ · GET /stream (MJPEG)│ │
│  │ · HTTP probe   │  │ · POST set_camera_cfg│ │
│  └──────┬───────┘  │ · GET capture_image   │ │
│         │          │ · GET capture_binary   │ │
│         │          └───────────┬────────────┘ │
│         │                      │              │
│  ┌──────┴──────────────────────┴───────────┐  │
│  │  AppState (ChangeNotifier)              │  │
│  │  · 帧限流 ~30fps                        │  │
│  │  · notifyListeners() 精准触发            │  │
│  └─────────────────────────────────────────┘  │
│  ┌─────────────────────────────────────────┐  │
│  │  CameraScreen (StatefulWidget)          │  │
│  │  · ImageViewer (gaplessPlayback)        │  │
│  │  · Quality 滑块 + 300ms 防抖            │  │
│  │  · Save JPEG / Save Raw → ~/Downloads   │  │
│  └─────────────────────────────────────────┘  │
└─────────────────────┬───────────────────────┘
                      │ WiFi (192.168.1.x)
                      ▼
┌─────────────────────────────────────────────┐
│  ESP32-P4                                   │
│  ESP-IDF simple_video_server                │
│  · Port 80: API (camera_info, capture, cfg) │
│  · Port 81: MJPEG stream (/stream)          │
│  · mDNS: esp-web-XXXXXX.local (primary)      │
│           esp-web.local (alias)               │
└─────────────────────────────────────────────┘
```

### 关键文件

| 文件 | 作用 | 说明 |
|------|------|------|
| `lib/services/http_service.dart` | HTTP 通信层 | MJPEG 流解析 + capture/save/setQuality；所有 HttpClient 添加 `findProxy = 'DIRECT'` 绕过 Linux 系统代理 |
| `lib/services/device_discovery.dart` | 设备发现 | mDNS `_http._tcp` + HTTP 端口探测；添加 `joinMulticast()` 修复 Linux 组播接收；添加 `_detectLocalSubnet()` 回退探测本机网段 |
| `lib/services/ulog_parser.dart` | ULog 解析器 | 解析 PX4 ULog 二进制格式，提取 camera_frame JPEG 帧（移植自 tools/ulog_extract_frames.py） |
| `lib/providers/app_state.dart` | 全局状态 | 帧限流、save/setQuality 委托 |
| `lib/screens/camera_screen.dart` | 摄像头画面 | API 按钮、Quality 滑块（300ms防抖）、文件保存到系统临时目录 |
| `lib/screens/ulog_viewer_screen.dart` | ULog 视频查看 | 下载/解析 .ulg 文件，帧缩略图网格，幻灯片播放，单帧/全帧保存 |
| `lib/screens/settings_screen.dart` | 设置+音频+文件管理 | WiFi/音量/ULog 录制控制 + SD 卡文件浏览器（.ulg 点击查看，.mp3 播放） |
| `lib/widgets/image_viewer.dart` | 图像渲染 | `gaplessPlayback: true` 消除闪烁 |
| `macos/Runner/*.entitlements` | 沙箱权限 | 添加 `network.client` 出站网络权限 |

### 通信协议

**MJPEG 流**（Port 81 `GET /stream`）：
```
HTTP/1.1 200 OK
Content-Type: multipart/x-mixed-replace;boundary=... 
Transfer-Encoding: chunked

[chunk-size]\r\n
\r\n--BOUNDARY\r\nContent-Type: image/jpeg\r\nContent-Length: N\r\n\r\n[JPEG DATA]\r\n
[chunk-size]\r\n
\r\n--BOUNDARY\r\n...
```

**API**（Port 80）：

| 方法 | 端点 | 用途 |
|------|------|------|
| GET | `/api/get_camera_info` | 获取摄像头信息 |
| POST | `/api/set_camera_config` | 设置 quality（body: `{index, image_format, jpeg_quality}`） |
| GET | `/api/capture_image?source=0` | 下载单帧 JPEG |
| GET | `/api/capture_binary?source=0` | 下载裸数据 |

---

## TODOs / Roadmap

### ✅ 已完成

- [x] 项目脚手架：Flutter + macOS/Linux/iOS/Android 平台配置
- [x] MJPEG 流实时显示（HttpClient 自动 dechunk）
- [x] 帧限流 ~30fps（33ms interval）
- [x] `gaplessPlayback: true` 消除换帧闪烁
- [x] mDNS 发现 + HTTP 端口探测
- [x] 手动添加设备（IP + Port）
- [x] Quality 滑块 → POST 到 ESP32（+300ms 防抖）
- [x] Save JPEG → 系统临时目录（沙箱兼容）
- [x] Save Raw → 系统临时目录（沙箱兼容）
- [x] macOS 沙箱网络权限（`network.client`）
- [x] Camera info 显示（分辨率 + 帧率）
- [x] 事件日志面板（图像下方可滚动日志，支持复制、折叠）
- [x] PopScope 拦截系统返回，确保断开连接
- [x] 重连死循环修复（attempt 计数器只在成功连接后重置，删除 `_scheduleReconnect`）
- [x] 设备发现优化（HttpClient 探测、完整 /24 扫描、全私有网段、超时适配高延迟网络）
- [x] Linux 组播修复：添加 `joinMulticast()` + `findProxy: 'DIRECT'` + 网段回退检测
- [x] ULog 视频查看：.ulg 文件下载 → 解析 camera_frame JPEG 帧 → 缩略图网格 + 幻灯片播放 + 单帧/全帧保存
- [x] 全页面文字可选中复制（每个 Scaffold 外包 SelectionArea）

### 🔜 待优化

- [ ] iOS / Android 真机运行测试
- [ ] 文件保存改用系统分享（`share_plus`）或添加 Downloads entitlements 存到 `~/Downloads/`
- [ ] 双摄像头支持（port 82）
- [ ] 图像格式切换（`image_format` 参数）
- [ ] Web 版本（`flutter run -d chrome`，WebSocket 适配）
- [ ] 亚稳态：`ListenableBuilder` 重建整个 CameraScreen → 改为只重建 ImageViewer
- [ ] App 图标 + 应用名本地化

### 🐛 已知问题

- `HttpException: unsolicited response` — Dart HttpClient 连接复用警告，不影响使用，重连后自动恢复
- 滑块快速拖动时 ESP32 串口会打印大量 `set jpeg quality` 日志 — 300ms 防抖已缓解
- macOS 沙箱导致不能写 `~/Desktop/` — 已改为 `~/Downloads/`
- Linux 上系统代理（环境变量 `http_proxy` 或桌面代理设置）会导致 `HttpClient` 将本地 IP 请求转发至代理，必须显式设置 `findProxy = (url) => 'DIRECT'`；同样影响 `NetworkInterface.list()` 可能返回空列表，需 `_detectLocalSubnet()` 回退
