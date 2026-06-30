# ESP32-P4 Monitor Project Setup

> 📋 架构分析 & 潜在问题: 参见 [project_design.md](project_design.md)

## 项目概述
基于 ESP32-P4 + Waveshare ESP32-P4-WiFi6-Touch-LCD-4B 开发板的综合监控项目,集成:
- **MIPI DSI** 显示 (720x720, ST7703, 通过 Waveshare BSP)
- **MIPI CSI** 摄像头 (OV5647, ISP 处理 RAW8→RGB565)
- **SDMMC** SD 卡 (SDSPI 1-bit 模式, SPI 20MHz, FAT 文件系统)
- **音频输入/输出** (ES8311 DAC + ES7210 ADC, I2S)
- **UI** ESP-Brookesia Phone 桌面 (LVGL v9.2.2) + 自定义 App
- **多板支持** `CONFIG_BOARD_WIFI6_TOUCH_LCD_4B` 切换 LCD-4B / WIFI6 基板
- **Web 配置** (端口 8080) WiFi/音量网页设置, WiFi 连接验证后写 NVS

### ESP-Brookesia App 列表

| App | 类名 | 功能 |
|-----|------|------|
| 📷 Camera | `PhoneAppCamera` | OV5647 实时预览, V4L2 (esp_video) 驱动, 800×800 → 720×720 显示 (**V4L2 重构**) |
| 🎤 Audio | `PhoneAppAudio` | 双 Mic 实时电平监控 + **MP3 录音 (SD 卡)** |
| 🎨 Squareline | `PhoneAppSquareline` | ESP-Brookesia 内置 Squareline 示例 |
| 🎵 Music | `PhoneAppMusic` | MP3/WAV 播放器, SD 卡, ESP-GMF 音频管道 |
| 🌐 **Camera Stream** | `PhoneAppCameraStream` | WiFi 启动后通过浏览器实时查看 MJPEG 摄像头流, mDNS 发现, 带 CPU/PSRAM 监控 |
| ⚙️ **Settings** | `PhoneAppSettings` | **音量/亮度 滑条 + WiFi** (WiFi 后台运行, 退出 App 保持连接) |

## 开发环境
- **芯片**: ESP32-P4NRW32
- **ESP-IDF 版本**: v6.0.1
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
│   ├── Kconfig.projbuild           # 项目 Kconfig 菜单 (含 BOARD_TYPE)
│   ├── example_config.h        # 引脚和参数宏定义
│   ├── main.cpp                    # 主程序 (C++): 多板支持, 按需初始化
│   ├── web_config_server.hpp       # Web 配置服务器头文件
│   ├── web_config_server.cpp       # Web 配置服务器 (HTTP :8080, WiFi/音量设置)
│   ├── phone_app_camera.hpp        # Camera App 头文件
│   ├── phone_app_camera.cpp    # Camera App (V4L2 + OV5647 sensor + ESP-DL 人体检测)
│   ├── phone_app_audio.hpp     # Audio App 头文件
│   ├── phone_app_audio.cpp     # Audio App (双 Mic 电平监控 + MP3 录音)
│   ├── phone_app_music.hpp     # Music App 头文件
│   ├── phone_app_music.cpp     # Music App (MP3/WAV 播放器)
│   ├── phone_app_settings.hpp     # Settings App 头文件
│   ├── phone_app_settings.cpp     # Settings App (音量/亮度 + WiFi)
│   ├── phone_app_camera_stream.hpp # Camera Stream App 头文件 (NEW)
│   ├── phone_app_camera_stream.cpp # Camera Stream App (WiFi状态 + MJPEG切换 + 系统监控)
│   ├── camera_stream.hpp          # Camera Stream 核心头文件
│   └── camera_stream.cpp          # Camera Stream 核心 (V4L2 + JPEG → HTTP MJPEG + mDNS)
├── doc/
│   ├── waveshare_esp32p4_wifi_vs_lcd_4b.md  # 两板外设接线对比
│   ├── ESP32-P4-WIFI6-datasheet.pdf          # WIFI6 基板原理图
│   └── ESP32-P4-WIFI6-Touch-LCD-4B.pdf       # LCD-4B 原理图
├── components/
│   ├── espressif__esp_lvgl_port/   # 本地补丁版 esp_lvgl_port
│   └── example_video_common/       # V4L2 视频初始化 + JPEG 编码 (NEW)
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

## FreeRTOS 任务列表

| # | 任务名 | 优先级 | 栈(KB) | 创建者 | 职责 |
|---|--------|--------|--------|--------|------|
| 1 | main | 默认(1) | 10 | ESP-IDF | 外设初始化 + 空闲循环 |
| 2 | taskLVGL | 4 | 10 | `esp_lvgl_port` | LVGL 渲染 + 触摸输入 |
| 3 | audio_echo | 5 | 4 | `PhoneAppAudio::run()` | Mic 读取 + 电平计算 (运行时创建, 退出时销毁) |
| 4 | httpd | 默认 | 6 | `CameraStream` | HTTP 服务器 (端口 80: Web UI, 端口 81: MJPEG) |
| 5 | wifi_scan | 1 | 6 | `PhoneAppSettings::run()` | WiFi 扫描 + 后台连接维护 (Settings 退出后保持运行) |

## 引脚配置

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

> **注意**: SD 卡使用真实的 GPIO 引脚 (物理引脚 80-86, 电源域 VDD_IO_5)。SDMMC_HOST_SLOT_0 被 ESP32-C6 WiFi (SDIO) 占用, 本项目实际使用 SDSPI 模式 (详见 `main/main.cpp:163`)。
> **重要**: MIPI CSI (物理引脚 42-48) 与 SD 卡 (物理引脚 80-86) 是完全不同的物理引脚, 不存在引脚冲突!
>
> **SDSPI 1-bit 模式原因**:
> - ESP32-P4 有 2 个 SDMMC Slot: Slot 0 和 Slot 1
> - **Slot 1**: 被 C6 WiFi 占用 (SDIO 4-bit, 40MHz)
> - **Slot 0 → SD 卡**: 只能用 `SDSPI_HOST_DEFAULT()` → GP-SPI2 (`SPI2_HOST`) → **1-bit 模式, 20MHz** (`SDMMC_FREQ_DEFAULT`)
> - SPI 协议本身只有 1 根数据线, 无法使用 4-bit 模式
> - 如果改为 SDMMC 4-bit 原生模式, 需要释放 Slot 0 或 Slot 1, 但这与 C6 WiFi SDIO 冲突

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


## 外设初始化流程 (按需初始化, App 退出时释放)

```
app_main()
  ├─ 0. NVS Flash (nvs_flash_init + 亮度加载)
  ├─ 1. MIPI DSI Display (bsp_display_start_with_config)
  │      → ST7703 720×720 LCD + GT911 Touch
  │      → LVGL taskLVGL 创建
  │
  ├─ 2. ESP-Brookesia Phone UI (6 apps installed)
  │      → PhoneAppSquareline
  │      → PhoneAppCamera        (CSI camera: run→init, close→deinit)
  │      → PhoneAppAudio         (I2S+ES8311+ES7210 + SD卡: run→init, close→deinit)
  │      → PhoneAppMusic         (I2S+ES8311 + SD卡: run→init, close→deinit)
  │      → PhoneAppSettings      (WiFi: run→init)
  │      → PhoneAppCameraStream  (CSI camera + mDNS + HTTP: run→init, close→deinit)
  │
  ├─ 3. (SD/Audio deferred to Audio & Music apps)
  │
  └─ 4. Memory Monitor Loop (每 5s)
```

> **注意**: SD 卡和音频 I2S 不再在 `app_main()` 中初始化, 而是由 Audio/Music App 在 `run()` 中按需初始化,
>   在 `close()` 中释放。这减少了空闲时的 DMA 对 PSRAM 的占用, 降低因负载导致 crash 的风险。
>   **引用计数**: `monitor_init_sdcard()` / `monitor_init_audio()` 使用引用计数保证多次调用安全。
>   每次打开 App 引用计数 +1, 退出 -1, 只有计数归零才真正释放硬件资源。


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

`PhoneAppCamera` 继承 `ESP_Brookesia_PhoneApp`，主要技术细节:

| 项目 | 配置 |
|------|------|
| 传感器 | OV5647, I2C auto-detect, 格式 `MIPI_2lane_24Minput_RAW8_800x800_50fps` (**VTS 运行时降为 4920 → ~10fps**) |
| CSI | 2-lane, 200Mbps, RAW8 input |
| ISP | RAW8→RGB565, 80MHz clock |
| ISP DMA | **~6.4 MB/s** (800×800×1×~10fps, VTS=4920) |
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

### 7. Camera App 人体检测 (ESP-DL + YOLO11n)

`PhoneAppCamera` 新增人体检测功能，使用 ESP-DL 框架 + COCODetect (YOLO11n 320×320)：

| 项目 | 配置 |
|------|------|
| 输入源 | Camera buffer (`_cam_width`×`_cam_height` RGB565LE) |
| 坐标处理 | COCODetect::run() 内部通过 ImagePreprocessor 自动缩放 → 无需手动 scale |

**已知问题 (已修复)**:
- ~~检测框不缩放 → 太小~~ (COCODetect 内部已缩放，手动 scale 会双重缩放导致过大)
- RGB565LE → PPA 不支持，改 CPU resize

**工作流程**:
1. Camera 30fps 正常预览 (ISP DMA → PSRAM → LVGL canvas)
2. detect task 每 600ms: 快照 RGB565 buffer → memcpy → COCODetect::run() → filter person
3. LVGL timer 检测到结果后: lv_canvas_init_layer → 画绿色矩形 + 置信度 % 标签 → lv_canvas_finish_layer

**TODO/优化**:
- 置信度标签带绿色背景框提升可读性
- 检测框平滑 (EMA 或 Kalman filter 减少抖动)
- ROI 区域检测 (只检测画面中心区域减少误报)

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
- mDNS: `esp-web.local`
- 启动条件: Toggle ON + WiFi 已连接 + Settings App 运行中
- 自动停止: Toggle OFF / WiFi 断开 / 退出 Settings App
- **不持久化**: 重启后默认 OFF

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
| 传感器 | OV5647, VTS=4920 (~10fps) |
| 编码 | **HW JPEG** (`CONFIG_EXAMPLE_SELECT_JPEG_HW_DRIVER=y`, esp_driver_jpeg), CPU 几乎无负载 |
| 编码质量 | 30 (降低 JPEG 体积 → 减少 WiFi/SDIO 负载, ~14KB/帧) |
| 帧率实测 | **6.9fps** @ 10fps sensor, CPU 7% |
| HTTP 架构 | 端口 80: Web UI + API (`/api/get_camera_info`, `/api/set_quality`), 端口 81: MJPEG `/stream` |
| Web UI | `<img>` 标签 + JavaScript 动态设置 `src` 至 `:81/stream`, AJAX 统计面板 (分辨率/帧率/帧数/画质滑块) |

**调试过程中发现并修复的关键问题**:
- **编码器信号量死锁**: `xSemaphoreGive` 在流处理器成功路径被误删, 第一帧后信号量永不归还, 后续帧全部超时 (0fps)。
- **`<iframe>` 不兼容 MJPEG**: Chrome 只能在 `<img>` 标签中渲染 MJPEG 流, `<iframe>` 无法显示。
- **JPEG 缓存对齐**: HW 编码器输出缓冲区需 `esp_cache_msync` 且大小向上取整到 128B 缓存行边界。
- **客户端断连处理**: `httpd_resp_send_chunk` 失败时 `break` 优雅退出, 避免 `httpd_sock_err` 刷屏。

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
- 帧率降至 ~10fps (VTS=4920) 降低 DMA 压力: ISP 带宽 ~6.4 MB/s

> **已知风险**: 高带宽入站 TCP (>200KB/s) 在 v2.12.7 仍可能触发死锁 (#197)。Camera Stream 为出站 (MJPEG ~98KB/s @ 7fps), 验证稳定。

### 16. 多板支持 (CONFIG_BOARD_WIFI6_TOUCH_LCD_4B)

通过 Kconfig `BOARD_TYPE` 选择目标开发板:

| 配置 | 板子 | 显示/LVGL | 音频 Codec | Web 配置 |
|------|------|:---:|------|:---:|
| `BOARD_WIFI6_TOUCH_LCD_4B` (默认) | LCD-4B | ✅ Phone UI | ES8311(0x30)+ES7210(0x80) | ✅ 8080 |
| `BOARD_WIFI6` | WIFI6 基板 | ❌ 无屏幕 | ES8311(0x18) 单芯片 | ✅ 8080 |

差异点详见 `doc/waveshare_esp32p4_wifi_vs_lcd_4b.md`。

### 17. Web 配置服务器 (web_config_server)

端口 **8080**，提供网页设置界面:
- **WiFi**: SSID / 密码 / 开关
- **音量**: 滑条 0-100
- **连接验证**: 点 Save 后先尝试连接，成功才写 NVS，失败回连旧 WiFi
- **WiFi 门控**: 任务等待 `IP_EVENT_STA_GOT_IP` 后才启动 HTTP，WiFi 未连时不占用 socket

> **已知限制**: 首次启动 NVS 为空时无法配网 (AP 模式因 esp-hosted SDIO 限制不稳定)。需预先通过 UART 烧录或 Settings App (LCD-4B) 写入 WiFi 凭据。

## 构建和烧录

```bash
# 设置环境
source ~/.espressif/v6.0.1/esp-idf/export.sh

# 构建
cd esp32p4_monitor
idf.py set-target esp32p4
idf.py build

# 切换板子 (WIFI6 无屏)
# idf.py menuconfig → Monitor Example Configuration → Select Board Type → ESP32-P4-WIFI6
# 或修改 sdkconfig.defaults: # CONFIG_BOARD_WIFI6_TOUCH_LCD_4B is not set

# 烧录
idf.py -p /dev/ttyUSB0 flash monitor
```

## 待完成事项
- [x] Camera App 框架 (PhoneAppCamera 类 + CSI/ISP + OV5647 Sensor Init)
- [x] Audio App 框架 (PhoneAppAudio 类 + 双 Mic 电平监控)
- [x] ES7210 ADC 初始化 + Mic Gain 配置
- [x] Camera App 打开/关闭/重新打开 生命周期
- [x] CSI/ISP 正确释放 (stop→disable→del 顺序)
- [x] Camera 在 LCD 显示有问题, 红色显示成绿色 — 修复: ISP `byte_swap_en=1`
- [x] Camera App 关闭后重新挂载 SD 卡
- [x] **Camera App 人体检测 (ESP-DL + YOLO11n 320x320 ≈ 1.8fps)**
- [x] **Camera App 迁移到 V4L2 (esp_video) 统一接口**
- [x] **Camera Stream WiFi 推流 (HTTP MJPEG + mDNS)**
- [x] **Camera Stream App 独立分离 + Web UI 优化 + HW JPEG 编码**
- [x] **Settings App Camera Stream toggle 移除 (迁移到独立 App)**
- [x] **esp_hosted 降级至 v2.12.7 + SDIO 稳定性配置**
- [x] **Settings App WiFi 静态成员修复 (跨实例持久化)**
- [x] **CPU/PSRAM 实时监控日志 (1s 间隔, FreeRTOS 运行时统计)**
- [x] **SD/音频延迟初始化 + 引用计数**
- [x] **esp_cam_sensor 升级 1.7.0 → 2.2.0**
- [x] **多板支持: CONFIG_BOARD_WIFI6_TOUCH_LCD_4B + WIFI6 无屏模式**
- [x] **Web 配置服务器 (端口 8080, WiFi/音量网页设置, connect-before-save)**
- [x] **WiFi 门控启动: web_config_task 等待 STA_GOT_IP 才启 HTTP**
- [ ] 自定义 720x720 ESP-Brookesia 样式表
- [ ] Camera App 回放/录制功能
- [ ] **WIFI6 无屏配网**: 首次启动 NVS 为空时需要 WiFi AP 模式配网。当前 esp-hosted SDIO 不支持稳定 SoftAP (客户端连接时 SDIO 缓冲区溢出 → C6 崩溃)。需修复 C6 SDIO 驱动或通过其他方式配网（UART CLI / BLE provisioning）。**当前假定 NVS 已有 WiFi SSID/密码**。
- [x] Audio App Speaker 输出功能 (需解决回声消除)
- [x] Audio App MP3 录音 (Shine encoder, SD 卡)
- [x] Music App LVGL 线程安全 (GMF 回调加 lvgl_port_lock)
- [x] Settings App LVGL 线程安全 (wifiConnectTaskHandler 加 bsp_display_lock)
