# ESP32-P4 Monitor Project Setup

> 📋 本文档是**软件/架构/实现**的唯一参考 (含硬件平台、驱动架构、FreeRTOS 任务调度、各模块实现细节)。
> 📋 需求清单、已修复问题登记 (R/S/M 列表) 与变更记录见 [PROJECT_REQUIREMENTS.md](PROJECT_REQUIREMENTS.md)。
> 📋 硬件接线与规格见 [README.md](README.md)。

## 项目概述
基于 ESP32-P4 + Waveshare ESP32-P4-WiFi6-Touch-LCD-4B 开发板的综合监控项目,集成:
- **MIPI DSI** 显示 (720x720, ST7703, 通过 Waveshare BSP)
- **MIPI CSI** 摄像头 (OV5647, ISP 处理 RAW8→RGB565)
- **SDMMC** SD 卡 (SDSPI 1-bit 模式, SPI 20MHz, FAT 文件系统)
- **音频输入/输出** (ES8311 DAC + ES7210 ADC, I2S)
- **UI** ESP-Brookesia Phone 桌面 (LVGL v9.2.2) + 自定义 App
- **多板支持** 通过 GT911 I2C 自动检测 LCD-4B / WIFI6，单一固件适配
- **Web 配置** (端口 8080) WiFi/音量网页设置, WiFi 连接验证后写 NVS, SD 卡文件管理器 (浏览/下载/删除)
- **Camera Frame Recording** JPEG 帧录制到 ULog 文件 (PPA 300×300 路径, ~5-8KB/帧, 2fps, 自动随 Camera Stream 启停)

### ESP-Brookesia App 列表

| App | 类名 | 功能 |
|-----|------|------|
| 📷 Camera | `PhoneAppCamera` | OV5647 实时预览, V4L2 (esp_video) 驱动, 800×800 → 720×720 显示 (纯预览, 检测已移至 CameraStream) |
| 🎤 Audio | `PhoneAppAudio` | 双 Mic 实时电平监控 + **MP3 录音 (SD 卡)** |
| 🎨 Squareline | `PhoneAppSquareline` | ESP-Brookesia 内置 Squareline 示例 |
| 🎵 Music | `PhoneAppMusic` | MP3/WAV 播放器, SD 卡, ESP-GMF 音频管道 |
| 🌐 **Camera Stream** | `PhoneAppCameraStream` | WiFi 启动后通过浏览器实时查看 MJPEG 摄像头流, mDNS 发现, **PPA 硬件加速检测**, CPU/PSRAM 监控 |
| ⚙️ **Settings** | `PhoneAppSettings` | **音量/亮度 滑条 + WiFi** (WiFi 始终启用, 后台运行, 退出 App 保持连接) |

## 开发环境
- **芯片**: ESP32-P4NRW32
- **ESP-IDF 版本**: v6.x
- **Flash**: 32MB (QIO)
- **PSRAM**: 32MB (200MHz, XIP)
- **CPU 频率**: 360 MHz

## 项目结构

```
esp32p4_monitor/
├── CMakeLists.txt              # 顶层项目配置
├── sdkconfig.defaults          # 默认 Kconfig 配置
├── partitions.csv              # 分区表 (15M app)
├── main/
│   ├── CMakeLists.txt          # 主组件编译配置 (C++)
│   ├── idf_component.yml       # 组件依赖声明
│   ├── Kconfig.projbuild           # 项目 Kconfig 菜单
│   ├── example_config.h        # 引脚/参数/NVS键/音量亮度常量/共享mDNS API 宏定义
│   ├── main.cpp                    # 主程序 (C++): 多板支持, 按需初始化
│   ├── peripherals.hpp             # PeripheralManager facade — 委托给独立 Driver 模块
│   ├── peripherals.cpp             # PeripheralManager 实现 (thin facade)
│   ├── drivers/                    # Driver 模块 (PX4-style 目录结构)
│   │   ├── audio/
│   │   │   ├── audio_driver.hpp    # AudioDriver — I2S + codec 生命周期 + volume uORB
│   │   │   └── audio_driver.cpp
│   │   ├── sdcard/
│   │   │   ├── sdcard_driver.hpp   # SDCardDriver — SD 卡 mount/unmount + LDO
│   │   │   └── sdcard_driver.cpp
│   │   └── camera/
│   │       ├── camera_driver.hpp   # CameraDriver — camera_state pub/sub + claim/release (owner-tracked)
│   │       └── camera_driver.cpp
│   │   └── system_monitor/
│   │       ├── system_monitor.hpp  # SystemMonitor — CPU/memory 采样 + uORB 发布
│   │       └── system_monitor.cpp
│   ├── logger/                     # 文本日志模块
│   │   ├── logger.hpp              # Logger API (level 过滤, esp_log_set_vprintf 拦截)
│   │   └── logger.cpp              # Ring buffer + writer task → SD card text file, 文件轮转
│   ├── web_config_server.hpp       # Web 配置服务器头文件
│   ├── web_config_server.cpp       # Web 配置服务器 (HTTP :8080, WiFi/音量设置)
│   ├── phone_app_camera.hpp        # Camera App 头文件
│   ├── phone_app_camera.cpp    # Camera App (V4L2 + OV5647 sensor, 纯预览)
│   ├── phone_app_audio.hpp     # Audio App 头文件
│   ├── phone_app_audio.cpp     # Audio App (双 Mic 电平监控 + MP3 录音)
│   ├── phone_app_music.hpp     # Music App 头文件
│   ├── phone_app_music.cpp     # Music App (MP3/WAV 播放器)
│   ├── phone_app_settings.hpp     # Settings App 头文件
│   ├── phone_app_settings.cpp     # Settings App (音量/亮度 + WiFi, WiFi 始终启用)
│   ├── phone_app_camera_stream.hpp # Camera Stream App 头文件 (NEW)
│   ├── phone_app_camera_stream.cpp # Camera Stream App (WiFi状态 + MJPEG切换 + 系统监控)
│   ├── camera_stream.hpp          # Camera Stream 核心头文件
│   └── camera_stream.cpp          # Camera Stream 核心 (V4L2 + JPEG → capture task + HTTP MJPEG + mDNS)
│   ├── ppa_preprocessor.hpp       # PPA 硬件预处理 (RGB565→RGB888 resize)
│   └── ppa_preprocessor.cpp       # PPA SRM client: 缩放+格式转换, CPU 仅做量化
├── proto/                                    # uORB .msg 消息定义
│   ├── fps_stats.msg
│   ├── detection_result.msg
│   ├── wifi_state.msg
│   ├── audio_level.msg
│   ├── camera_state.msg
│   ├── recording_state.msg
│   └── volume_state.msg
│   └── system_stats.msg
│   └── system_alert.msg
├── tools/
│   └── msg_gen.py                    # uORB .msg → C 代码生成器
├── doc/
│   ├── waveshare_esp32p4_wifi_vs_lcd_4b.md  # 两板外设接线对比
│   ├── ESP32-P4-WIFI6-datasheet.pdf          # WIFI6 基板原理图
│   └── ESP32-P4-WIFI6-Touch-LCD-4B.pdf       # LCD-4B 原理图
├── components/
│   ├── espressif__esp_lvgl_port/     # 本地补丁版 esp_lvgl_port
│   ├── example_video_common/         # V4L2 视频初始化 + JPEG 编码
│   └── uorb/                         # uORB for FreeRTOS (pub/sub 消息总线)
│       ├── include/uorb.h
│       ├── uorb.c
│       └── CMakeLists.txt
└── project_setup.md            # 本文档
```

## 关键依赖

| 组件 | 版本 | 来源 |
|------|------|------|
| `espressif/esp-brookesia` | 0.5.0 | ESP Registry |
| `waveshare/esp32_p4_wifi6_touch_lcd_4b` | 2.0.0 | ESP Registry |
| `espressif/esp_codec_dev` | 1.5.10 | ESP Registry |
| `espressif/esp_cam_sensor` | 2.2.0 | ESP Registry |
| `espressif/esp_sccb_intf` | 0.0.8 | ESP Registry |
| `espressif/esp_video` | 2.2.0 | ESP Registry |
| `espressif/esp_new_jpeg` | 1.0.2 | ESP Registry |
| `espressif/mdns` | 1.11.2 | ESP Registry |
| `espressif/cjson` | 1.7.19 | ESP Registry |
| `protocol_examples_common` | local | IDF examples |
| `espressif/esp_lvgl_port` | 2.8.0~1 | **本地补丁版** |
| `lvgl/lvgl` | 9.2.2 | ESP Registry |
| `shine_encoder` | (local) | 本地组件 `components/shine_encoder/` |
| `espressif/esp_audio_simple_player` | ^1.0.0 | ESP Registry |
| `espressif/gmf_core` | ^1.0 | (间接依赖, 自动拉入) |
| `espressif/gmf_audio` | ^1.0 | (间接依赖, 自动拉入) |
| `espressif/gmf_io` | ^1.0 | (间接依赖, 自动拉入) |
| `uorb` (自定义) | 1.0.0 | 本地组件 `components/uorb/` — uORB for FreeRTOS |
| `ulog` (自定义) | 1.0.0 | 本地组件 `components/ulog/` — ULog 日志写入 (SD 卡, PX4 双模式命名) |

## 兼容补丁

`main/compat/` 目录存放第三方组件的兼容补丁，保持 `managed_components/` 不被修改：

| 文件 | 目标组件 | 原因 |
|------|----------|------|
| `compat/mbedtls/sha256.h` | `espressif/esp-dl` | ESP-IDF v6.x (mbedtls 4.x) 将 `sha256.h` 移至 `mbedtls/private/`，esp-dl 尚未适配 |

## uORB 消息总线

项目引入 **uORB for FreeRTOS**（仿 PX4 uORB API），实现统一的 task 间 pub/sub 通信。

### 工作流

```
proto/*.msg  ──→  tools/msg_gen.py  ──→  main/generated/*.h/.cpp
                                                    │
                    idf.py build 自动触发            ▼
                                             编译到固件
```

### 定义 topic 步骤

1. 在 `proto/` 下创建 `.msg` 文件（PX4 兼容格式）
2. 运行 `idf.py uorb_topics`（或直接 `idf.py build` 自动触发）
3. 各 task 通过 `orb_advertise()` / `orb_subscribe()` / `orb_publish()` / `orb_copy()` 通信

### 当前 topic 列表

| Topic | 结构体 | 队列深度 | 发布者 | 订阅者 |
|-------|--------|:-------:|--------|--------|
| `fps_stats` | `fps_stats_s` | 3 | CameraStream (capture task) | CameraStream App UI |
| `detection_result` | `detection_result_s` | 1 | NPU 检测 task | UI 绘图 timer |
| `wifi_state` | `wifi_state_s` | 1 | Settings (wifi_scan) | Web Config, UI |
| `audio_level` | `audio_level_s` | 1 | Audio task | UI 电平表 |
| `camera_state` | `camera_state_s` | 1 | CameraDriver | PeripheralManager, CameraStream, PhoneAppCamera |
| `recording_state` | `recording_state_s` | 1 | PhoneAppAudio | UI 录制状态, PhoneAppMusic 录制互斥 |
| `volume_state` | `volume_state_s` | 1 | AudioDriver (via PeripheralManager) | Music Playback |
| `system_stats` | `system_stats_s` | 3 | SystemMonitor | ULog, Web API (/api/system_stats) |
| `system_alert` | `system_alert_s` | 5 | SystemMonitor | ULog, Web API (/api/system_alerts) |
| `camera_frame` | `camera_frame_s` | 2 | CameraStream | ULog (camera JPEG frame recording) |

## Driver 模块架构 (Phase 3 重构)

PeripheralManager 从 monolithic facade 重构为 thin facade，外设逻辑拆分为独立 Driver 单例:

```
App 层 (Audio/Music/Camera/Settings/web_config)
         ↓ 调用 (API 不变)
PeripheralManager (thin facade)
  ├── AudioDriver::instance()     — I2S + codec 生命周期 + volume uORB
  ├── SDCardDriver::instance()    — SD mount/unmount + LDO power-cycle
  └── CameraDriver::instance()    — camera_state pub/sub + claim/release

SystemMonitor::instance()         — CPU/memory 采样 + system_stats uORB (独立于 PeripheralManager)
```

| Driver | 目录 | 职责 | 线程安全 |
|--------|------|------|----------|
| AudioDriver | `drivers/audio/` | I2S channel + ES8311/ES7210 codec init/deinit, volume/mic_gain/codec_write, volume_state uORB | lifecycle_mutex + codec_mutex |
| SDCardDriver | `drivers/sdcard/` | SDSPI init-once (LDO VO4 power-cycle), never unmount | _init_mutex |
| CameraDriver | `drivers/camera/` | camera hardware mutual exclusion via uORB, claim/release API with owner tracking | _mutex |
| SystemMonitor | `drivers/system_monitor/` | Per-core CPU busy% (via idle task runtime, no scheduler suspend) + heap/PSRAM 采样, system_stats uORB, ESP_LOG 摘要, Web API, 资源异常告警 (CPU>90% / Memory>85%) | _latest_mutex + _alert_mutex |

**设计要点**:
- PeripheralManager API 完全不变，app 模块无需修改
- 各 Driver 可独立使用 (如 `AudioDriver::instance().set_volume(80)`)
- CameraDriver 的 `claim(caller_id)/release(caller_id)` 替代了 CameraStream/PhoneAppCamera 中直接发布 camera_state uORB 的代码
- `claim()` 支持 owner tracking: 同一 caller_id 可重入, 不同 caller_id 互斥 (防止 CameraStream 运行时 Camera App 误操作 V4L2)

## FreeRTOS 任务调度

### 任务总表

| # | 任务名 | 优先级 | 栈 | 核心 | 创建者 | 生命周期 | 周期/触发 |
|---|--------|--------|-----|------|--------|----------|-----------|
| 1 | `main` | 1 (默认) | 10KB | 0 | ESP-IDF | **一次性** — setup 后 `vTaskDelete(NULL)` 回收 | — |
| 2 | `taskLVGL` | 4 | 10KB | 1 | `esp_lvgl_port` | 永久 | 20ms timer |
| 3 | `audio_echo` | 5 | 12KB (PSRAM) | 0† | `PhoneAppAudio::run()` | App 打开→关闭 | 10ms 循环 |
| 4 | `detect` | 2 | 16KB (PSRAM) | 0 (固定) | `PhoneAppCamera` (已移除, 检测迁至 CameraStream) | — | — |
| 5 | `GMF task` | 5 | 8KB (PSRAM) | 0 | `esp_audio_simple_player` | 按需创建/销毁 | 事件驱动 |
| 6 | `wifi_scan` | 1 | 6KB | 0† | `Settings` (WiFi ON) / Boot | WiFi ON→OFF | 500ms 轮询 |
| 7 | `wifi_conn` | 4 | 4KB | 0† | `Settings` (连接点击) | 一次性 | 15s 超时 |
| 8 | `httpd:80` | 默认 | 6KB | 0 (固定) | `CameraStream::start()` | Stream 打开→关闭 | 事件驱动 |
| 9 | `httpd:81` | 默认 | 6KB | 0 (固定) | `CameraStream::start()` | Stream 打开→关闭 | 事件驱动 (MJPEG) |
| 10 | `capture_task` | — | PSRAM | 0 | `CameraStream::start()` | Stream 打开→关闭 | DQBUF→encode→publish |
| 11 | `web_config` | 1 | 4KB | 0 (固定) | `web_config_server_start()` | 永久 | 1s 空闲 + HTTP 事件 |
| 12 | `w_audio` | 1 | 12KB (PSRAM) | 0 (固定) | `web_config_server` (录音时) | 录音→停止 | 100ms 循环 (I2S read) |
| 13 | `model_load` | 1 | 8KB (PSRAM) | 0† | `CameraStream::_init_detection()` | 一次性 | 模型加载后 `vTaskDelete(NULL)` |
| 14 | `sys_monitor` | 1 | 可配置 | — | `SystemMonitor::start()` | 永久 | `CONFIG_APP_SYS_MONITOR_INTERVAL_MS` |
| 15 | `ulog_writer` | 1 | 8KB (PSRAM, 静态 TCB) | — | `ulog_writer_start()` | Start→Stop | ring buffer 消费 |
| 16 | `logger` | — | — | — | `logger_init()` | 永久 | ring buffer → SD 文本文件 |

> † 使用 `xTaskCreate` (未指定核心), FreeRTOS 调度到 core 0 或 core 1

### 优先级与核心亲和性

```
Priority 5: audio_echo, GMF task     (最高 — 实时音频, PCM 不丢失)
Priority 4: taskLVGL, wifi_conn      (UI 渲染 / WiFi 连接)
Priority 2: detect                   (NPU 检测, 不抢占 UI)
Priority 1: main, wifi_scan, web_config, w_audio, model_load, sys_monitor, ulog (后台/辅助)
```

| 核心 | 任务 |
|------|------|
| **Core 0** | `main` (一次性), `audio_echo`, `GMF`, `wifi_scan/conn`, `httpd:80/81`, `capture_task`, `web_config`, `w_audio`, `model_load` |
| **Core 1** | `taskLVGL` (固定) |

- Core 1 专用于 LVGL 渲染, 避免 UI 抖动 (S176: httpd 3 实例绑定 core 0, LVGL timer 5→20ms)。
- Core 0 承载所有计算密集型任务 (音频编码, 协议栈, HTTP, NPU 推理)。
- 多数大栈任务 (audio/GMF/model_load/ulog/detect) 栈分配在 PSRAM, TCB 留 Internal SRAM (见 SRAM 优化)。
- `main` setup 完成后 `vTaskDelete(NULL)` 释放 ~4KB 栈和 TCB。

## 引脚分配

### MIPI DSI (2-lane) — 专用接口引脚

> **注意**: MIPI DSI 使用 ESP32-P4 专用接口引脚 (Dedicated Interface Pins, 电源域 VDD_MIPI_DPHY), 不是 GPIO。以下编号为芯片物理引脚号。

| 信号 | Pin | 说明 |
|------|-----|------|
| DSI_DATAP1 | 35 | FPC D1+ |
| DSI_DATAN1 | 36 | FPC D1- |
| DSI_CLKN | 37 | FPC CLK- |
| DSI_CLKP | 38 | FPC CLK+ |
| DSI_DATAP0 | 39 | FPC D0+ |
| DSI_DATAN0 | 40 | FPC D0- |

### MIPI CSI (2-lane, OV5647) — 专用接口引脚

> **注意**: MIPI CSI 使用 ESP32-P4 专用接口引脚 (Dedicated Interface Pins, 电源域 VDD_MIPI_DPHY), 不是 GPIO。以下编号为芯片物理引脚号。

| 信号 | Pin | 说明 |
|------|-----|------|
| CSI_DATAP0 | 43 | DAT0+ |
| CSI_DATAN0 | 42 | DAT0- |
| CSI_CLKP | 44 | CLK+ |
| CSI_CLKN | 45 | CLK- |
| CSI_DATAP1 | 47 | DAT1+ |
| CSI_DATAN1 | 46 | DAT1- |

### SDMMC/SDSPI — 真实 GPIO 引脚

> **注意**: SD 卡使用真实的 GPIO 引脚 (物理引脚 80-86, 电源域 VDD_IO_5)。
> **重要**: MIPI CSI (物理引脚 42-48) 与 SD 卡 (物理引脚 80-86) 是完全不同的物理引脚, 不存在引脚冲突!
>
> **SDSPI 1-bit 模式** (两板统一):
> - ESP32-P4 有 2 个 SDMMC Slot: Slot 0 和 Slot 1
> - **Slot 1**: 被 C6 WiFi 占用 (SDIO 4-bit, 40MHz)
> - **Slot 0**: BSP SDMMC 原生模式与 C6 SDIO 共享 host controller，初始化冲突。**两板统一使用 SDSPI** (`SPI2_HOST`)
> - **LCD-4B**: SDSPI (GPIO 39/42/43/44), LDO4 由 BSP display init 上电
> - **WIFI6**: SDSPI (GPIO 39/42/43/44), LDO4 由 `sd_pwr_ctrl` API 管理
> - **SD 常驻挂载**: boot 时挂载后永不下电，`SDCardDriver::init()` 幂等, `deinit()` no-op
> - VFS 表扩容: `CONFIG_VFS_MAX_COUNT=8→16`，SD 常驻 + camera ISP/CSI 需要更多槽位

| 信号 | GPIO | 物理引脚 | 说明 |
|------|------|----------|------|
| SD_CLK | 43 | 84 | Clock / SPI SCLK |
| SD_CMD | 44 | 86 | Command / SPI MOSI |
| SD_D0 | 39 | 80 | Data 0 / SPI MISO |
| SD_D1 | 40 | 81 | Data 1 (4-bit mode) |
| SD_D2 | 41 | 82 | Data 2 (4-bit mode) |
| SD_D3 | 42 | 83 | Data 3 / SPI CS |

### 音频 I2S + PA (ES8311 + ES7210)
| 信号 | GPIO | 说明 |
|------|------|------|
| I2S_MCLK | 13 | 主时钟 |
| I2S_BCLK | 12 | 位时钟 |
| I2S_LRCK | 10 | 左右声道时钟 |
| I2S_SDIN | 9 | ES8311 DAC 输入 |
| I2S_SDOUT | 11 | ES7210 ADC 输出 |
| **PA_CTRL** | **53** | **功放使能 (HIGH=ON)** |

### I2C 共享总线 (GT911触摸 + ES8311 + ES7210 + OV5647)
| 信号 | GPIO | 说明 |
|------|------|------|
| I2C_SDA | 7 | 数据 |
| I2C_SCL | 8 | 时钟 |

### I2C 设备地址
| 设备 | 地址 |
|------|------|
| GT911 触摸 | (BSP 内部处理) |
| ES8311 DAC | 0x30 |
| ES7210 ADC | 0x80 |
| OV5647 Camera | (auto-detect) |


## 外设初始化流程 (SD 卡启动挂载, 音频按需初始化)

```
app_main()
  ├─ 0a. mDNS mutex init (shared_mdns_mutex_init — 提前创建, 消除懒初始化竞态)
  ├─ 0. NVS Flash (nvs_flash_init + 亮度加载)
  ├─ 1. MIPI DSI Display (bsp_display_start_with_config)
  │      → ST7703 720×720 LCD + GT911 Touch
  │      → LVGL taskLVGL 创建
  │
  ├─ 2. SD 卡挂载 (boot时挂载, 永不下电, WiFi之前)
  │      → LCD-4B: SDSPI (LDO4由BSP display init上电)
  │      → WIFI6:  SDSPI (sd_pwr_ctrl API管理LDO4)
  │      → 读取 wifi.txt (首次配置fallback)
  │      → **SD 卡保持挂载, 不再 unmount**
  │
  ├─ 3. WiFi 自动连接 (SD之后 — C6 SDIO争用SDMMC host ctrl)
  │
  ├─ 4. ESP-Brookesia Phone UI (6 apps installed)
  │      → PhoneAppSquareline
  │      → PhoneAppCamera        (CSI camera: run→init, close→deinit)
  │      → PhoneAppAudio         (音频+SD: run→init SD, close→deinit audio only)
  │      → PhoneAppMusic         (音频+SD: run→init SD, close→deinit audio only)
  │      → PhoneAppSettings      (WiFi: run→init)
  │      → PhoneAppCameraStream  (CSI camera + mDNS + HTTP: run→init, close→deinit)
  │
  ├─ 5. Web Config Server (HTTP :8080)
  │
  ├─ 6. ULog Logger (仅当 SD 卡成功挂载时初始化, WiFi+SNTP 同步后自动 Start, Web/Flutter 可手动 Start/Stop)
  │    - 自动启动: web_config_server WiFi 连接后启动 SNTP, SNTP 同步回调自动 ulog_writer_start()
  │    - Web: `POST /api/ulog/start` / `POST /api/ulog/stop` / `GET /api/ulog/status`
  │    - Web UI: 设置页 "ULog Recording" 卡片, Start/Stop 按钮 + 状态显示
  │    - Flutter: SettingsScreen "ULog Logger" 卡片, Start/Stop 按钮 + 字节/文件路径
  │
  └─ 7. vTaskDelete(NULL) — 回收 app_main 任务栈
```

> **注意**: SD 卡在 `app_main()` 中挂载后**永不卸载**。
>   - 两板统一使用 SDSPI：BSP SDMMC 原生模式与 C6 SDIO 共享 host controller，LCD-4B 无法同时使用
>   - LCD-4B: BSP display init 已上电 LDO4，SDCardDriver 跳过 LDO 管理，直接用 SDSPI
>   - WIFI6: `SDCardDriver::init()` 通过 `sd_pwr_ctrl` API 管理 LDO4 + SDSPI, `deinit()` 为 no-op
>   - `SDCardDriver::init()` 支持幂等调用, `_has_lcd` flag 区分板型
>   - `PeripheralManager::init_sdcard()` / `deinit_sdcard()` 保持 API 兼容
>   - **音频 I2S** 仍由 Audio/Music App 在 `run()` 中按需初始化, `close()` 中释放 (引用计数)
>   - **VFS_MAX_COUNT=16**: SD 常驻 + camera ISP/CSI 需要更多 VFS 槽位 (原 8 不够)


## 关键修改和问题解决

### 1. esp_lvgl_port 兼容性问题
**问题**: ESP Registry 的 `espressif/esp_lvgl_port: 2.8.0~1` 引用了 `LV_COLOR_FORMAT_RGB565_SWAPPED`,该符号在 `lvgl: 9.2.2` 中不存在。

**解决**: 从 `phone_p4_function_ev_board` 项目复制了本地补丁版 `espressif__esp_lvgl_port` 到 `components/` 目录。

### 2. C++ 指定初始化器顺序 (Designated Initializers)
**问题**: C++ 对 struct 指定初始化器的字段顺序要求严格（必须与声明顺序一致）。

**解决**: 调整所有 struct 初始化顺序使其与头文件声明一致，包括:
- `esp_cam_ctlr_csi_config_t`: 添加 `clk_src`、`data_type`，调整 `data_lane_num`/`lane_bit_rate_mbps` 顺序
- `es8311_codec_cfg_t`: 添加 `digital_mic`、`invert_mclk`、`invert_sclk`、`no_dac_ref`
- `sccb_i2c_config_t`: 按 `dev_addr_length` → `device_address` → `scl_speed_hz` → `addr_bits_width` → `val_bits_width` 顺序
- `gpio_config_t`: 按 `pin_bit_mask` → `mode` → `pull_up_en` → `pull_down_en` → `intr_type` 顺序
- `isp_processor_cfg_t`: 添加 `clk_src`、`yuv_range`、`yuv_std`、`bayer_order`、`intr_priority` 字段

### 3. gpio_num_t 和 i2s_mclk_multiple_t 类型转换
**问题**: C++ 不允许 int→enum 隐式转换。

**解决**: GPIO 引脚用 `(gpio_num_t)` 显式转换，MCLK 倍频用 `I2S_MCLK_MULTIPLE_384` 枚举值。

### 4. I2C 总线共享
GT911 触摸控制器、ES8311、ES7210、OV5647 共享同一物理 I2C 总线 (GPIO7/8)。BSP 初始化 I2C_NUM_0 (`bsp_display_start` 内调用 `bsp_i2c_init`)，音频和摄像头复用 BSP 的 I2C 句柄 `bsp_i2c_get_handle()`。

### 5. ~~MIPI CSI 与 SDMMC 引脚冲突~~ (已勘误: 不存在引脚冲突)
> **勘误**: 经过对照 [ESP32-P4 数据手册](../doc/esp32-p4_datasheet_En.pdf) Table 2-1 (Pin Overview) 和 Table 2-9 (Dedicated Interface Pins) 确认:
> - **MIPI CSI** 使用专用接口引脚 (物理引脚 42-48, 电源域 VDD_MIPI_DPHY), **不是 GPIO**
> - **SD 卡** 使用真实 GPIO 引脚 (物理引脚 80-86, 电源域 VDD_IO_5), 包括 GPIO39/42/43/44
> - **两者是完全不同的物理引脚**, 不存在引脚冲突!
>
> 误判原因: 原理图中的 "43, 44, 39" 是 ESP32-P4 芯片的物理引脚号 (Pin Number), 被错误理解为 GPIO 号。详见 [pin_analysis_summary.md](../doc/pin_analysis_summary.md)

当前 Camera App 中卸载 SD 卡的操作 (`phone_app_camera.cpp:175`) 是基于此前误判的**遗留代码**。由于 MIPI CSI 与 SD 卡引脚独立, 理论上可以同时工作。保留卸载逻辑不会造成功能问题 (仅暂时不可用 SD 卡)。

> **⚠️ DMA 总线竞争**: 虽然物理引脚不冲突, 但 MIPI CSI ISP pipeline (esp_video) 和 SDSPI 共享同一 DMA 控制器和 PSRAM 带宽。在高负载场景 (如 Camera Stream WiFi 推流) 下, DMA 竞争可能导致 SD 卡读写变慢或 SDIO WiFi 超时。

### 6. Camera App 实现

`PhoneAppCamera` 继承 `ESP_Brookesia_PhoneApp`，纯预览应用 (检测已迁移至 CameraStream)。

**CSI+ISP 预览管道**:
```
OV5647 Sensor → MIPI CSI (RAW8, 800×800) → ISP (RAW8→RGB565, byte_swap=1) → PSRAM Buffer
                                                                                   ↓
                                                                    LVGL Canvas (800×800) → Display (720×720 裁剪)
```

**资源清理顺序** (关键 — 必须严格遵守, 否则 CSI 控制器泄漏导致二次打开失败):
```
Sensor: stop stream → del_dev  →  SCCB: del_i2c_io  →  CSI: stop → disable → del  →  ISP: disable → del_processor  →  Buffer: free
```

主要技术细节:

| 项目 | 配置 |
|------|------|
| 传感器 | OV5647, I2C auto-detect, 格式 `MIPI_2lane_24Minput_RAW8_800x800_50fps` (**VTS 运行时降为 24600 → ~2fps**) |
| CSI | 2-lane, 200Mbps, RAW8 input |
| ISP | RAW8→RGB565, 80MHz clock |
| ISP DMA | **~1.3 MB/s** (800×800×1×~2fps, VTS=24600) |
| 帧缓冲 | PSRAM, `heap_caps_aligned_alloc(128, ...)`, 128字节对齐 (cache line) |
| 显示 | LVGL `lv_canvas` + buffer 零拷贝, 30fps 定时器刷新 |
| 传感器初始化 | `esp_cam_sensor` auto-detect → `set_format` → VTS I2C write → `ioctl(S_STREAM)` |
| 清理顺序 | sensor stop → del_dev → sccb_del → CSI stop→disable→del → ISP disable→del → free buf |

**已解决的问题**:
- Buffer 溢出: OV5647 输出 800×800, 帧缓冲需匹配 800×800 (1.28MB)
- 显示适配: LVGL v9 不支持 `lv_image_set_src` 动态 buffer, 改用 `lv_canvas`
- CSI 控制器泄漏: 必须按 stop→disable→del 顺序, 否则第二次打开失败
- SCCB 链接: ESL 头文件需 `extern "C"` 包裹
- 音量振荡: 移除 mic→speaker 回声功能

### 7. PPA 硬件加速 + 人体检测 (CameraStream 专用)

> **⚠️ 架构变更**: 人体检测已从 PhoneAppCamera (Camera App) 完全移除, Camera App 现在是纯预览应用。
> 所有检测逻辑集中在 CameraStream, PPA 输出同时供 COCODetect 推理和 JPEG 编码, 避免重复处理。

**PPA 硬件加速架构** (`PPAPreprocessor` 类, CameraStream 使用):
```
V4L2 buffer RGB565 (800×800)
        ↓ PPA DMA 直接读取 (无需 memcpy)
   PPA SRM (hardware):
        ├── resize: 800×800 → 300×300  (scale 0.375, 4-bit frac quantized from 0.4)
        └── format: RGB565LE → BGR888  (PPA outputs BGR24 in memory)
        ↓ _out_buf (BGR888, 300×300 contiguous, allocated for 320×320)
        ├──→ COCODetect internal (CPU):
        │       ├── letterbox: 300×300 → 320×320 (10px gray border each side)
        │       └── quantize: BGR888 → RGB888_QINT8  (R↔B swap + normalize)
        │       ↓ model input tensor
        │   YOLO11n inference (~560ms)
        │       ↓ post-process
        │   Rescale boxes: 300×300 → 800×800  (using actual_width/height)
        │
        └──→ JPEG encoder (hardware):
                ├── input: 300×300 BGR24 (= JPEG_ENCODE_IN_FORMAT_RGB888)
                └── encode: ~30ms (vs ~200ms for 800×800 RGB565)
```
- PPA 将 resize + 格式转换从 CPU 卸载到硬件 (~1ms vs CPU ~30ms)
- PPA 4-bit frac 量化: 0.4 → 0.375, 实际输出 300×300 (非 320×320)
- PPA `pic_w/pic_h` 必须设为 actual 300×300 (非 requested 320×320), 否则行步长不匹配导致 COCODetect 读取错位数据
- PPA 输出 BGR24 内存布局, 传给 COCODetect 为 `BGR888`, 预处理自动 R↔B swap
- COCODetect letterbox 自动将 300×300 填充为 320×320 (10px 灰边框)
- **PPA 输出复用**: 同一个 BGR24 buffer 同时供 COCODetect (检测) 和 JPEG 编码器 (推流), 无需二次 PPA 调用
- **JPEG 编码优化**: PPA 输出 BGR24 = `JPEG_ENCODE_IN_FORMAT_RGB888` (ESP-IDF 定义), 编码器直接消费 PPA 输出, 编码时间 ~200ms → ~30ms, JPEG 体积 ~30-50KB → ~5-8KB
- **V4L2 buffer 直接 DMA**: PPA 直接从 V4L2 mmap buffer DMA 读取, 无需 memcpy 到中间缓冲区, V4L2 buffer 持有时间从 ~200ms 降至 ~1ms
- 自动降级: PPA 初始化失败时回退到 CPU 全流程 (RGB565→resize_nn→QINT8), 此时才分配 `_detect_in_buf`

**已知问题 (已修复)**:
- ~~检测框不缩放 → 太小~~ (COCODetect 内部已缩放，手动 scale 会双重缩放导致过大)
- ~~RGB565LE → PPA 不支持~~ (已通过直接调用 `ppa_do_scale_rotate_mirror` 绕过 ESP-DL `resize_ppa()` 限制)
- ~~PPA 输出 320×320 但实际只有 300×300 有效像素~~ (已改用 `actual_width/height` 跟踪 PPA 实际输出, 传给 COCODetect 300×300 使 letterbox 正确计算 10px padding)
- ~~PPA RGB888 输出实为 BGR24~~ (已改用 `DL_IMAGE_PIX_TYPE_BGR888`, COCODetect 预处理自动 R↔B swap)
- ~~Rescale 用 800/320=2.5~~ (已改用 800/300=2.667 基于 actual_width)
- ~~PPA pic_w=320 导致行步长错位~~ (PPA 输出 320 像素行步长但 COCODetect 按 300 像素行步长读取, 数据错位。已改为 `pic_w=actual_w=300` 使 PPA 输出连续紧凑数据)

**PhoneAppCamera (Camera App) — 纯预览模式**:
- Camera App 不再运行任何检测, 仅做 V4L2 DQBUF → memcpy → canvas 显示
- 移除了: COCODetect, PPAPreprocessor, _detect_in_buf (1.28MB PSRAM), _detect_mutex, _detect_task, _draw_box_on_canvas, uORB detection_result
- 简化了: _frame_update_timer_cb 不再需要 mutex 同步和检测框绘制, 直接 memcpy + cache sync + invalidate

### 8. Camera 红绿通道修正（Bayer + 字节序）

**问题**: Camera 预览画面中红色显示为绿色。两个根本原因:

1. **Bayer 模式不匹配**: OV5647 传感器输出 `GBRG` Bayer pattern (`ESP_CAM_SENSOR_BAYER_GBRG`)，但 ISP 配置为 `BGGR`（`COLOR_RAW_ELEMENT_ORDER_BGGR`）。ISP 去马赛克时通道分配错误，导致红色像素被当作绿色处理。

2. **像素字节序不匹配**: ISP RAW8→RGB565 输出的默认字节序 (`byte_swap_en=0`，大端优先) 与 LE CPU + LVGL/LCD 期望的小端字节序不匹配。

**解决**:
- 将 ISP `bayer_order` 从 `COLOR_RAW_ELEMENT_ORDER_BGGR` 改为 `COLOR_RAW_ELEMENT_ORDER_GBRG`，匹配 OV5647 传感器的实际输出模式。
- 设置 `flags.byte_swap_en = 1`，让 ISP 硬件自动输出小端字节序 RGB565，无需 CPU 手动字节交换（原软件 swap 存在 cache 一致性问题）。

### 8. Audio App 实现

`PhoneAppAudio` 继承 `ESP_Brookesia_PhoneApp`，双声道电平表 UI + MP3 录音:

| 项目 | 配置 |
|------|------|
| Mic 输入 | I2S RX, 48000Hz Stereo, 16-bit |
| Codec | ES8311 (speaker) + ES7210 (dual mic, 30dB gain) |
| PA | GPIO 53 High (功放使能) |
| 数据读取 | **直接 I2S RX** (`i2s_channel_read`)，绕过 `esp_codec_dev_read` 中间层 |
| 电平计算 | Per-channel peak detection → 0-100% LVGL bar |
| UI 刷新 | LVGL timer 20Hz (50ms) |
| 音频输出 | **不输出 speaker**（纯监控模式，无回声振荡） |
| **MP3 录音** | **Shine 定点 MP3 编码器 (128kbps stereo 48kHz) → SD 卡 `/sdcard/rec_*.mp3`** |
| 录音 UI | REC 按钮 (右上角), 录制时间/大小实时显示, 录音文件列表 |

**已解决的问题**:
- Speaker 无声: 缺少 GPIO 53 PA 使能, 缺少正确的 `hw_gain = {5.0, 3.3}` 配置
- Mic 无信号: `esp_codec_dev_read` 中间层与 I2S DMA 状态冲突, 改直接 `i2s_channel_read`
- ES7210 未初始化: BSP `bsp_audio_codec_microphone_init()` 不设置 mic gain, 需手动调 30dB
- BSP I2S 格式不匹配: `bsp_audio_init(NULL)` 默认 22kHz Mono, 需显式传入 16kHz Stereo 配置
- 回声振荡: mic→speaker 闭环, 改为纯监控不输出

### 8. ESP-Brookesia 样式表适配
720x720 分辨率没有对应的 ESP-Brookesia 预置样式表, 当前使用默认回退方案。

### 9. MP3 录音 (Shine Encoder)

使用 **Shine** 定点 MPEG Layer III 编码器 (toots/shine), 作为本地组件集成:

- **组件路径**: `components/shine_encoder/`
- **编码参数**: 48kHz stereo, 128kbps CBR, MPEG-I Layer III
- **帧大小**: 1152 samples/channel (SHINE_MAX_SAMPLES)
- **PCM 缓冲**: PSRAM 分配, 2304 int16_t interleaved (1152×2)
- **工作流程**:
  1. 用户按下 "REC" 按钮 → `_start_recording()`
  2. 初始化 Shine encoder, 打开 SD 卡文件 `/sdcard/rec_YYYYMMDD_HHMMSS.mp3`
  3. `_audio_task` 每 10ms 读取 I2S PCM 数据, 累积到 1152 samples/channel
  4. `shine_encode_buffer_interleaved()` 编码一帧, `fwrite()` 写入 SD 卡
  5. 用户按 "STOP" → `_stop_recording()`: `shine_flush()` + `shine_close()` + `fclose()`
- **线程安全**: `_is_recording=false` 后等待 50ms 确保 audio task 退出编码块

### 10. 音频初始化最终方案

放弃 `bsp_audio_init()` + `bsp_audio_codec_speaker_init()` 路线 (内部默认参数不匹配), 改为:

```cpp
// 1. PA GPIO 53 HIGH
gpio_set_level(53, 1);

// 2. I2S 手动初始化 (16000Hz, Stereo, Duplex)
i2s_new_channel(&chan_cfg, &tx, &rx);
i2s_channel_init_std_mode(tx, &std_cfg);   // MCLK=13, BCLK=12, WS=10, DOUT=9
i2s_channel_init_std_mode(rx, &std_cfg);   // MCLK=13, BCLK=12, WS=10, DIN=11

// 3. ES8311 Codec (speaker)
es8311_codec_new({ .codec_mode=BOTH, .pa_pin=53, .hw_gain={5.0,3.3}, .mclk_div=384 })
esp_codec_dev_open(s_codec_handle, &fs);  // fs = {16kHz, 16bit, Stereo}

// 4. ES7210 Codec (mic)
es7210_codec_new({ .mic_selected=MIC1|MIC2, .mclk_src=PAD, .mclk_div=384 })
esp_codec_dev_open(s_codec_mic_handle, &fs);
esp_codec_dev_set_in_gain(s_codec_mic_handle, 30);

// 5. 重新使能 TX (codec open 后 I2S 状态可能改变)
i2s_channel_disable(tx); i2s_channel_enable(tx);
```

### 11. Music App LVGL 线程安全修复

**播放管道**:
```
SD Card (.mp3/.wav) → GMF File IO → GMF Audio Pipeline (解码) → _asp_output_cb() → ES8311 Codec (I2S TX, 48kHz 16bit Stereo) → Speaker (PA GPIO53 HIGH)
```

**问题**: 播放完一首歌后 GMF 音频管道回调 `_asp_event_cb` (运行在 GMF task, priority 5) 直接调用 `_next()` → `_play()` 更新 LVGL UI (`lv_label_set_text` 等)。如果此时 `taskLVGL` (priority 4) 正在渲染 (`lv_timer_handler`), 高优先级的 GMF task 会抢占并调用 `lv_inv_area()`, 触发 LVGL 断言 `!disp->rendering_in_progress` → 系统崩溃。

**解决**: 在 `_asp_event_cb` 中调用 LVGL API 前加 `lvgl_port_lock(0)` / `lvgl_port_unlock()`。`lvgl_port_lock` 使用递归互斥锁:
- GMF task 调用 `lvgl_port_lock(0)` → 阻塞等待 LVGL 释放锁
- `taskLVGL` 渲染完毕后释放锁 → GMF task 安全更新 UI
- 递归互斥锁允许 LVGL task 内嵌套调用（按钮点击 → `_play()` 等）

### 12. Settings App LVGL 线程安全修复

**问题**: `wifiConnectTaskHandler` FreeRTOS task (line 635-636) 在无锁保护下调用了 `lv_label_get_text()` 和 `lv_textarea_get_text()`，这两个函数读取 LVGL 对象内部字符串指针。如果 `taskLVGL` 并发渲染时修改了这些对象，可能导致悬空指针访问或 `lv_inv_area` 断言崩溃。

**解决**: 在读取 UI 文本前加 `bsp_display_lock(0)`，将字符串复制到局部缓冲区后 `bsp_display_unlock()`，后续只使用局部副本。确保 LVGL 对象的内部指针不会在 taskLVGL 渲染期间被并发访问。

### 13. Settings App (NVS 持久化 + Camera Stream WiFi 推流)

`PhoneAppSettings` 继承 `ESP_Brookesia_PhoneApp`，提供系统设置界面:

| 设置项 | 控件 | NVS Key | 范围 | 默认值 |
|--------|------|---------|------|--------|
| 🔉 音量 | LVGL Slider | `volume` | 0-100 | 60 |
| ☀️ 屏幕亮度 | LVGL Slider | `brightness` | 20-100 | 80 |
| 📶 Wi-Fi | LVGL Switch + 扫描列表 + 密码输入 | `wifi_en`, `ssid`, `pass` | - | - |
| 📷 Camera Stream | LVGL Switch | **无 NVS 持久化** | - | OFF |

**Camera Stream 功能**:
- 通过 WiFi 将摄像头画面以 MJPEG 格式推流到 HTTP (port 81)
- 参考 `simple_video_server` 项目, 使用 V4L2 + `esp_new_jpeg` SW 编码器
- mDNS: `esp-web-XXXXXX.local` (primary, unique per device) + `esp-web.local` (convenient alias)
- 启动条件: Toggle ON + WiFi 已连接 + Settings App 运行中
- 自动停止: Toggle OFF / WiFi 断开 / 退出 Settings App
- **不持久化**: 重启后默认 OFF
- **JPEG 快照**: `/api/capture_image` 返回最新帧 (stream handler 每帧缓存)
- **内联人体检测**: 每 3 帧运行 COCODetect (PPA 加速预处理), 检测框绘制在 JPEG 帧上

**NVS 命名空间**: `"settings"`

**WiFi 说明**: WiFi 代码通过 `#ifdef SETTINGS_WIFI_ENABLED` 条件编译。ESP32-P4 使用 `CONFIG_ESP_HOST_WIFI_ENABLED=y`（而非 `CONFIG_ESP_WIFI_ENABLED`），因为 P4 本身没有内置 WiFi，需要 ESP32-C6 通过 SDIO 提供远程 WiFi 传输。

**全局共享**:
- 音量通过 `s_codec_handle` → `esp_codec_dev_set_out_vol()` 设置
- 亮度通过 `bsp_display_brightness_set()` 设置
- Music App 启动时从 NVS 读取音量
- 主程序启动后从 NVS 读取并应用亮度
- 所有更改立即保存到 NVS

### 14. Camera Stream App — 流媒体优化与 Web UI

`PhoneAppCameraStream` 独立于 Settings App, 通过浏览器实时查看 MJPEG 摄像头流。

| 项目 | 配置 |
|------|------|
| 传感器 | OV5647, VTS=24600 (~2fps) |
| 编码 | **HW JPEG** (`CONFIG_EXAMPLE_SELECT_JPEG_HW_DRIVER=y`, esp_driver_jpeg), CPU 几乎无负载 |
| 编码质量 | 30 (降低 JPEG 体积 → 减少 WiFi/SDIO 负载, ~5-8KB/帧 @ 300×300) |
| 推流分辨率 | 300×300 BGR24 (PPA 输出, 非 800×800 RGB565) |
| 帧率实测 | **~2fps** @ 2fps sensor, CPU ~5% |
| HTTP 架构 | 端口 80: Web UI + API (`/api/get_camera_info`, `/api/set_quality`, `/api/get_detection_info`, `/api/set_camera_config`), 端口 81: MJPEG `/stream` |
| Web UI | `<img>` 标签 + JavaScript 动态设置 `src` 至 `:81/stream`, AJAX 统计面板 (分辨率/帧率/帧数/画质滑块) |

**调试过程中发现并修复的关键问题**:
- **编码器信号量死锁**: `xSemaphoreGive` 在流处理器成功路径被误删, 第一帧后信号量永不归还, 后续帧全部超时 (0fps)。
- **`<iframe>` 不兼容 MJPEG**: Chrome 只能在 `<img>` 标签中渲染 MJPEG 流, `<iframe>` 无法显示。
- **JPEG 缓存对齐**: HW 编码器输出缓冲区需 `esp_cache_msync` 且大小向上取整到 128B 缓存行边界。
- **客户端断连处理**: `httpd_resp_send_chunk` 失败时 `break` 优雅退出, 避免 `httpd_sock_err` 刷屏。
- **TCP keep-alive**: 所有 httpd 实例启用 TCP keep-alive (idle=5s, interval=5s, count=3), 防止客户端断连后半开 TCP 连接阻塞 select() 导致 HTTP 服务器不可达。
- **WiFi 断连 httpd 重启**: Web Config (8080) 检测 WiFi 断连后自动停止 httpd 刷除残留 session, WiFi 恢复后自动重启并重新注册 URI, 防止 EHOSTUNREACH/EAGAIN 导致的 session 泄漏耗尽 `max_open_sockets`。URI 注册提取为 `_register_web_config_uris()` 复用。
- **lru_purge + 自检看门狗**: Web Config (8080) 原 `lru_purge_enable=false` + `max_open_sockets=3`, 客户端断开 (recv 113/ECONNABORTED) 后 session 被占满, httpd 静默停止 `accept()`, 因 WiFi 未断既有自愈不触发 → 无法重连只能重启。修复: `lru_purge_enable=true` (listen 始终监听) + `max_open_sockets` 3→12 + `web_config_self_probe()` 自检 (每 15s 本地 GET /api/status, 连续 2 次失败重启 httpd)。
- **LWIP_MAX_SOCKETS 扩容**: 22→28, 3 个 httpd 实例内部占用 17 个 socket, 加 WiFi/mDNS/SNTP 后 22 不足导致 `accept()` 返回 `ENOTSOCK`。
- **JPEG 编码器 OOM (LCD-4B)**: HW JPEG 编码器的 DMA 描述符 (rxlink/txlink) 需要 `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` (内部 SRAM)。LCD-4B 的 LVGL 绘制缓冲区 (720×50×2B ≈ 72KB) 也在内部 SRAM, 导致编码器分配失败。修复: ① LVGL 绘制缓冲区改用 PSRAM (`buff_spiram=true`, ESP32-P4 PSRAM 支持 DMA) ② JPEG 编码器在 start() 时直接初始化 (capture task 需要立即使用, 含 3 次重试) ③ `CONFIG_SPIRAM_TRY_ALLOCATE_DMA_BUFFER=y` 允许 DMA 缓冲区分配到 PSRAM。
- **JPEG 编码器初始化竞态**: `_encoder_init_in_progress` atomic flag + `_encoder_initialized` atomic 双阶段保护, 防止并发初始化。
- **独立 capture task 架构**: 帧采集/编码/uORB 发布从 stream_handler 分离到独立 `_capture_task` (FreeRTOS task)。即使无 HTTP 客户端连接, capture task 仍持续运行 → DQBUF → encode → publish (`fps_stats`, `camera_frame`) → store shared JPEG, 确保 ULog 录制模块始终能接收到 uORB topic。stream_handler 变为纯消费者: 等待 `_frame_ready_sem` → 从 `_shared_jpeg_buf` 读取最新 JPEG → 发送 MJPEG part。
- **内联人体检测**: CameraStream 在 capture task 中每 3 帧运行一次 COCODetect 推理 (PPA 加速预处理: RGB565→BGR888 resize)，检测框直接绘制在 PPA BGR24 输出 buffer 上再编码为 JPEG。V4L2 buffer 在 PPA 处理后立即归还 (~1ms)，不再持有到编码完成。JPEG 编码器直接消费 PPA 输出 (300×300 BGR24)，编码时间从 ~200ms 降至 ~30ms。
- **V4L2 QBUF 延迟 (JPEG sensor)**: JPEG sensor 路径中 `jpeg_data` 直接指向 V4L2 mmap buffer，QBUF 必须延迟至所有消费者 (`_publish_camera_frame`/`_store_shared_jpeg`) 完成后。PPA 路径 (PPA 输出独立) 和 CPU fallback 路径 (编码后 `jpeg_data` 指向 `_jpeg_out_buf`) 可安全提前 QBUF。

### 15. esp_hosted (WiFi over SDIO) 稳定性

ESP32-P4 通过 SDIO 连接 ESP32-C6 实现 WiFi。高 DMA 负载下已知 SDIO 死锁:

| Issue | 状态 | 根因 |
|-------|------|------|
| [#167](https://github.com/espressif/esp-hosted-mcu/issues/167) | Open | 多根因: C6 SLC DMA 同步 bug + CMD53 块模式死锁 |
| [#184](https://github.com/espressif/esp-hosted-mcu/issues/184) | Closed | TCP 入站 100KB 后停滞。修复: `OPTIMIZATION_RX_NONE` + `Q_SIZE=5` |
| [#197](https://github.com/espressif/esp-hosted-mcu/issues/197) | Open | SDIO RX mempool 耗尽硬死锁。**v2.12.7 是 Waveshare 硬件唯长期验证稳定版本** (12h+零错误) |

**当前稳定配置**:
- esp_hosted: **v2.12.7** (v2.12.8+ 的 PSRAM mempool 路径在 Waveshare 上验证更快死锁, #197)
- `SDIO_OPTIMIZATION_RX_STREAMING_MODE=y` + `TX_Q_SIZE=20` + `RX_Q_SIZE=20`
- `MEMPOOL_PREFER_SPIRAM=y` (必需: SRAM 被 LVGL 缓冲区消耗)
- `SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` (WiFi/LWIP 缓冲移至 PSRAM)
- 帧率降至 ~2fps (VTS=24600) 降低 DMA 压力: ISP 带宽 ~1.3 MB/s

> **已知风险**: 高带宽入站 TCP (>200KB/s) 在 v2.12.7 仍可能触发死锁 (#197)。Camera Stream 为出站 (MJPEG ~98KB/s @ 7fps), 验证稳定。

> **已修复 (2026-07-08)**: 客户端断连后 HTTP 服务器不可达。根因: `LWIP_MAX_SOCKETS=22` 太小 — 3 个 httpd 实例内部占用 17 个 socket (listen+ctrl×2+data), 加上 WiFi/mDNS/SNTP 后, socket 表耗尽导致 `accept()` 返回 `ENOTSOCK` (128), 监听 socket 失效, HTTP 永久不可达。修复: ① `LWIP_MAX_SOCKETS` 22→28 ② 所有 httpd 启用 TCP keep-alive ③ Web Config WiFi 断连自动重启 httpd。
>
> **已修复 (2026-07-08)**: Web Config (8080) 客户端断开 (recv error 113 / ECONNABORTED) 后, **WiFi 仍 UP 但无法重新连接**, 只能重启系统。根因: IDF `httpd_server()` 仅在 `lru_purge_enable || httpd_is_sess_available()` 时监听 listen socket (`httpd_main.c:276`)。原配置 `lru_purge_enable=false` + `max_open_sockets=3`, 一旦 3 个 session 被 keep-alive/半开/已断开但残留的 socket 占满, httpd 停止调用 `accept()`, 静默拒绝所有新连接; 而既有自愈逻辑只在 WiFi down→up 触发, 此时 WiFi 未断故永不重启。修复: ① `lru_purge_enable=true` (listen socket 始终被监听, 新连接总是被接受, 自动淘汰 LRU 残留 session) ② `max_open_sockets` 3→12 (给 Web UI 多个 keep-alive 连接留余量) ③ 新增 **自检看门狗** `web_config_self_probe()`: 每 15s 从本任务向 STA IP:8080 发 `GET /api/status`, 连续 2 次失败 (~30s) 则 `httpd_stop()` 重启 httpd 刷除残留 session, 覆盖 httpd 主线程卡死在 handler 等 watchdog 无法检测的场景。

### 16. 多板支持 (GT911 I2C 自动检测)

启动时通过 I2C 探测 GT911 触摸芯片 (0x5D) 自动识别开发板类型:

| 检测结果 | 板子 | 显示/LVGL | 音频 Codec | Web 配置 |
|------|------|:---:|------|:---:|
| GT911 响应 | LCD-4B | ✅ Phone UI | ES8311(0x30)+ES7210(0x80) | ✅ 8080 |
| 无响应 | WIFI6 基板 | ❌ 无屏幕 | ES8311(0x30) 单芯片 | ✅ 8080 |

全局变量 `g_has_lcd`（`volatile bool`）在 `main.cpp` 中设置，`web_config_server.cpp` 通过 `extern volatile` 引用。
无需任何编译期配置，单一固件自动适配。

### 17. Web 配置服务器 (web_config_server)

端口 **8080**，提供网页设置界面:
- **WiFi**: SSID / 密码 / 开关
- **音量**: 滑条 0-100
- **连接验证**: 点 Save 后先尝试连接，成功才写 NVS，失败回连旧 WiFi
- **WiFi 门控**: 任务等待 `IP_EVENT_STA_GOT_IP` 后才启动 HTTP，WiFi 未连时不占用 socket
- **音频录制**: Start/End 按钮, I2S RX → Shine MP3 编码 → SD 卡 (`/sdcard/rec_*.mp3`), 实时显示录制时长和文件大小
- **音频播放**: 列出 SD 卡上 `.mp3` 文件, Play 按钮通过 `esp_audio_simple_player` 播放
- **互斥保护**: 音频功能仅在 Camera Stream **未**运行时可操作, UI 自动隐藏/显示

> **已知限制**: 首次启动 NVS 为空时无法配网 (AP 模式因 esp-hosted SDIO 限制不稳定)。需预先通过 UART 烧录或 Settings App (LCD-4B) 写入 WiFi 凭据。

### 18. Web 音频录制/播放 (web_config_server 音频扩展)

| 项目 | 配置 |
|------|------|
| 录音输入 | I2S RX, 48000Hz Stereo, 16-bit (`s_rx_handle`) |
| 编码器 | Shine 定点 MP3, 128kbps CBR, 48kHz Stereo |
| 帧大小 | 1152 samples/channel (SHINE_MAX_SAMPLES) |
| PCM 缓冲 | PSRAM, 2304 int16_t interleaved |
| 录音任务 | `w_audio` (core 0, prio 1, **12KB** stack) |
| 播放器 | `esp_audio_simple_player` → ES8311 DAC (`s_codec_handle`) |
| 录音文件 | `/sdcard/rec_YYYYMMDD_HHMMSS.mp3` |
| 懒加载 | SD 卡 + 音频首次访问时才初始化 (`__audio_init()`) |
| 清理 | `web_config_server_stop()` 刷新编码器、关闭文件、释放 SD/音频 |

**API 端点** (5 core + 6 audio + 4 file mgr + 5 CORS + 5 ULog + 2 system + 8 WeChat + 3 LLM + 2 TG = 40 handlers, `max_uri_handlers=42`):
- `GET /` — Web UI 首页
- `GET /api/status` — 设备状态
- `POST /api/settings` — 保存 WiFi/音量
- `POST /api/camera_stream` — 开/关 Camera Stream
- `POST /api/factory_reset` — 恢复出厂设置
- `GET /api/audio/record_start` — 开始录音
- `GET /api/audio/record_stop` — 停止录音
- `GET /api/audio/record_status` — 录音状态 (秒数, 字节数)
- `GET /api/audio/list` — 列出 `.mp3` 文件
- `GET /api/audio/play?file=xxx.mp3` — 播放文件
- `GET /api/audio/stop` — 停止播放
- `GET /api/files/list?dir=/` — 列出目录内容 (JSON)
- `GET /api/files/download?path=xxx` — 下载文件 (binary)
- `POST /api/files/delete` — 删除文件 (body: `{"path": "xxx"}`)
- `POST /api/files/delete_batch` — 批量删除 (body: `{"paths": [...]}`)
- `GET /api/ulog/status` — 日志状态 (running/filepath/bytes_written)
- `POST /api/ulog/start` — 开始 ULog 录制
- `POST /api/ulog/stop` — 停止 ULog 录制
- `GET /api/system_stats` — CPU/内存/任务快照
- `GET /api/system_alerts` — CPU/内存告警状态 + 阈值
- `POST /api/wechat/login/start` — 微信扫码登录 (CONFIG_APP_CLAW_CAP_IM_WECHAT)
- `GET /api/wechat/login/status` — 微信登录状态
- `POST /api/wechat/login/cancel` — 取消微信登录
- `POST /api/wechat/login/persist` — 持久化微信凭据
- `GET /api/llm/config` — LLM/AI 配置 (has_api_key)
- `POST /api/llm/config` — 设置 LLM API key/model/base_url
- `POST /api/tg/config` — Telegram Bot 配置 (CONFIG_APP_CLAW_CAP_IM_TG)
- + 12 个 CORS OPTIONS 预检 handler (settings, camera_stream, factory_reset, files/delete, files/delete_batch, ulog/start, ulog/stop, wechat/login/start, wechat/login/status, wechat/login/cancel, wechat/login/persist, llm/config)

### 19. Web File Manager (web_config_server 文件管理)

浏览、下载、删除 SD 卡文件，通过 "Files" 按钮切换到文件管理模式。

| 项目 | 配置 |
|------|------|
| 目录浏览 | 递归, 面包屑导航, 目录优先排序 |
| 下载 | `GET /api/files/download?path=xxx` → binary stream + Content-Disposition |
| 删除 | `POST /api/files/delete` → 删除文件/空目录, 含前端确认弹窗 |
| 安全 | 路径穿越防护 (`..`, 绝对路径限制 `/sdcard/` 前缀) |
| 互斥 | 与 Audio Recorder 模式互斥: 切换时自动停止录音/播放; 录音中禁止文件操作 |

**板级兼容**: `monitor_init_audio()` 根据 `g_has_lcd` 自动选择:
- **LCD-4B**: ES8311 DAC (0x30) + ES7210 ADC (0x80, 双麦)
- **WIFI6**: ES8311 单芯片 (0x30, ADC+DAC) + NS4150B 功放
GPIO (I2S: 9-13, PA_CTRL: 53) 两块板子完全一致, 无需额外适配。

### 19. Flutter 跨平台 App (flutter_app/)

跨平台桌面/移动端应用，与 `web_config_server` 的 8080 API 通信:

| 功能 | 说明 |
|------|------|
| **设备发现** | mDNS + HTTP 子网扫描 |
| **设备排序** | 新扫描设备优先，历史设备在后，分区显示 |
| **设备状态** | Connected/Reachable/Offline/History 徽章 (TCP 端口探测) |
| **Camera 实时预览** | MJPEG 流解码 (端口 81) |
| **Settings 配置** | WiFi/音量/Camera Stream 开关 + 恢复出厂设置 |
| **音频录制** | 调用 8080 API 远程录制 MP3 到 SD 卡 |
| **音频播放** | 远程播放 SD 卡上已录制的 MP3 文件 |
| **ULog 视频查看** | 下载/解析 .ulg 文件，camera_frame JPEG 帧缩略图 + 幻灯片 + 保存 |
| **平台支持** | macOS, iOS, Linux, Android |

**核心文件** (`flutter_app/lib/`):
```
screens/
├── home_screen.dart         # 设备发现 + 连接入口
├── camera_screen.dart       # 摄像头实时画面 (全窗口)
├── settings_screen.dart     # 配置 + 录音/播放 + 文件管理 + ULog (手机比例)
└── ulog_viewer_screen.dart  # ULog 视频查看 (缩略图/幻灯片/保存)
services/
├── http_service.dart        # 8080/81 API 封装
├── device_discovery.dart    # mDNS + HTTP 发现
├── ulog_parser.dart         # ULog 二进制解析器 (camera_frame JPEG 提取)
└── connected_device_store.dart
providers/
└── app_state.dart           # 全局状态管理
widgets/
├── device_card.dart         # 设备卡片 (Camera + Settings 按钮)
└── image_viewer.dart        # MJPEG 帧显示
```

**API 端点调用** (端口 8080):
- `GET /api/status` — 获取设备状态 (连接验证)
- `POST /api/settings` — 保存 WiFi/音量
- `POST /api/factory_reset` — 恢复出厂设置
- `POST /api/camera_stream` — 开/关 Camera Stream
- `GET /api/audio/*` — 6 个音频端点 (record/stop/status/list/play/stop)

> **注意**: 音频功能在 ESP32 Camera Stream 运行时被阻断 (`__cam_running()` 检查)。
> 连接流程: Camera 按钮 → 端口 81 推流; Settings 按钮 → 端口 8080 配置。

## Internal SRAM 分配分析 (HP L2MEM 768 KB)

ESP32-P4 内置 768 KB HP L2MEM，由 SRAM 和 L2 Cache 共享：

| 用途 | 大小 | 占比 | 类型 |
|------|------|------|------|
| L2 Cache (CONFIG_CACHE_L2_CACHE_SIZE) | 256 KB | 33.3% | 不可用 |
| IRAM 代码 (flash操作/中断安全代码) | **92 KB** | **20.9%** | 静态 |
| ROM 保留 (ROM BSS/Data/Stack) | 81 KB | 10.5% | 不可用 |
| DRAM .data + .bss (全局变量) | 45 KB | 10.2% | 静态 |
| 内部堆 (启动后空闲) | **~297 KB** | **68.9%** | 动态 (运行时峰值占用 ~386KB，剩余 **~54 KB**，超 85% 告警阈值) |

> 精确拆分（来自 `build/esp32p4_monitor.map` + `idf.py size`）：DIRAM 总量 431 KB = IRAM 90.1 KB + DRAM 静态 44.2 KB (.data 17.9 + .bss 26.3) + 内部堆空闲 ~297 KB。L2 Cache 256 KB 为保留、不计入 DIRAM。ROM 保留 81 KB 属独立区域，不纳入 DIRAM 口径。
>
> **2026-07-07 优化**: 关闭 LVGL IRAM (`CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n`) → LVGL 代码移至 PSRAM XIP，IRAM 从 156 KB 降至 92 KB，释放 ~64 KB。

运行时内部堆（启动后空闲 **~297 KB**；CameraStream+Music 峰值仅 **~54 KB** 空闲 / 占用 ~87.7%）主要消耗者：
- WiFi/LWIP 缓冲区: ~20-30 KB (SPIRAM_MALLOC_ALWAYSINTERNAL=4096, TCP_SND/WND=32768, pbufs 走 PSRAM)
- FreeRTOS 任务栈: ~30-40 KB (detect 16KB PSRAM, ASP 8KB PSRAM, audio 12KB×2 PSRAM)
- 系统服务 (mDNS/NVS/esp_netif): ~10-20 KB
- DMA 描述符/USB: ~5-10 KB
- 剩余可用: **~180 KB** (优化后，原 ~90 KB)

**最近优化**:
- **2026-07-07**: 关闭 LVGL IRAM (`LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n`) → LVGL 代码移至 PSRAM XIP，释放 **~64 KB** IRAM
- **2026-07-07**: ASP 音频任务栈 8KB 移至 PSRAM (`task_stack_in_ext=true` — GMF 内部用 WithCaps 处理), 共省 **~8 KB**
- **2026-07-07**: LWIP TCP 缓冲区 65535→32768, httpd `max_open_sockets` 7→3/2/3, 省 pbuf 头部（TCP 窗口最终回退至 32KB 并稳定，见 S175）
- **2026-07-08**: LWIP_MAX_SOCKETS 22→28 (3 httpd 实例内部占用 17 个 socket, 22 太紧导致 accept() ENOTSOCK), httpd 启用 TCP keep-alive + WiFi 断连重启
- **2026-07-08**: Web Config (8080) `lru_purge_enable` false→true + `max_open_sockets` 3→12 + 新增 `web_config_self_probe()` 自检看门狗, 修复客户端断开 (recv 113) 后 WiFi 仍 UP 但无法重连的问题
- **2026-07-08**: SystemMonitor 内存告警阈值 80%→85% (减少 Internal SRAM 误报)
- **2026-07-09**: `max_uri_handlers` 29/30→42, 修复 ESP-Claw IM (WeChat+TG) + LLM API 添加后 handler 注册失败 (`no slots left`)
- **2026-07-06**: `SPIRAM_MALLOC_ALWAYSINTERNAL` 从 16384 降至 4096 → LWIP pbufs (~1.5KB) 走 PSRAM，释放 ~20-30KB
- **2026-07-06**: Audio PCM buffer (phone_app_audio + web_config_server, 共 ~6.5KB) 从 INTERNAL 移入 PSRAM
- **2026-07-06**: detect task 16KB 栈移入 PSRAM (`xTaskCreateStaticPinnedToCore` + `heap_caps_malloc(SPIRAM)`)
- **2026-07-06**: CameraStream model_load task 8KB 栈移入 PSRAM (`xTaskCreateStatic`)
- **2026-07-08**: 两个音频任务栈 12KB×2 从 Internal SRAM 移至 PSRAM（O2 优化）— `phone_app_audio.cpp` 的 `audio_echo` 与 `web_config_server.cpp` 的 `w_audio` 改用 `xTaskCreateStatic`/`xTaskCreateStaticPinnedToCore` + `heap_caps_malloc(SPIRAM|MALLOC_CAP_8BIT)` 分配栈（TCB 留 Internal），任务退出后释放静态缓冲区；运行时节省 **~24 KB** Internal SRAM。与 detect(16KB)/ASP(8KB) 同模式，仅栈在 PSRAM，TCB 仍在 Internal。
- **2026-07-08**: SRAM 优化 (S216): 禁用 EAP (`CONFIG_ESP_WIFI_REMOTE_EAP_ENABLED=n`, 省 ~1.1KB IRAM + 21KB PSRAM) + DVP (`CONFIG_ESP_VIDEO_ENABLE_DVP_VIDEO_DEVICE=n`, 此板仅用 MIPI CSI); TCP SND_BUF/WND 64KB→32KB (65536 超 Kconfig range 被钳制为 5760, 32768 无需 WND_SCALE)
- **2026-07-06**: `SPIRAM_TRY_ALLOCATE_DMA_BUFFER` 在 IDF v6.x 中已不存在，`SPIRAM_TRY_ALLOCATE_WIFI_LWIP` 已覆盖 DMA 分配

> **✅ 已解决**: Camera Stream + Music 播放时 internal SRAM >80% 导致音频卡顿。主要通过 LVGL IRAM→PSRAM XIP (-64KB) 解决，辅以 ASP 栈→PSRAM (-8KB) 和 LWIP TCP 缓冲区缩小 (-30KB)。

## 构建和烧录

```bash
# 设置环境
source ~/.espressif/v6.x/esp-idf/export.sh

# 构建
cd esp32p4_monitor
idf.py set-target esp32p4
idf.py build

# 烧录
idf.py -p /dev/ttyUSB0 flash monitor
```

## 需求与问题登记

已完成需求、已修复问题 (R/S/M 编号) 与变更记录统一维护在 **[PROJECT_REQUIREMENTS.md](PROJECT_REQUIREMENTS.md)**，本文档不再重复登记。

- **已完成需求 / 已修复问题**: 见 PROJECT_REQUIREMENTS.md §2 (R1–R22 核心功能, S1–S246 稳定性与性能, M1–M13 系统监控)
- **待完成需求 / 已知限制 / 风险**: 见 PROJECT_REQUIREMENTS.md §3–§4
- **变更记录**: 见 PROJECT_REQUIREMENTS.md §7

