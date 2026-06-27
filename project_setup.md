# ESP32-P4 Monitor Project Setup

## 项目概述
基于 ESP32-P4 + Waveshare ESP32-P4-WiFi6-Touch-LCD-4B 开发板的综合监控项目,集成:
- **MIPI DSI** 显示 (720x720, ST7703, 通过 Waveshare BSP)
- **MIPI CSI** 摄像头 (OV5647, ISP 处理 RAW8→RGB565)
- **SDMMC** SD 卡 (4-bit 模式, FAT 文件系统)
- **音频输入/输出** (ES8311 DAC + ES7210 ADC, I2S)
- **UI** ESP-Brookesia Phone 桌面 (LVGL v9.2.2) + 自定义 App

### ESP-Brookesia App 列表

| App | 类名 | 功能 |
|-----|------|------|
| 📷 Camera | `PhoneAppCamera` | OV5647 实时预览, MIPI CSI + ISP, 800×800 → 720×720 显示 |
| 🎤 Audio | `PhoneAppAudio` | 双 Mic 实时电平监控 + **MP3 录音 (SD 卡)** |
| 🎨 Squareline | `PhoneAppSquareline` | ESP-Brookesia 内置 Squareline 示例 |
| 🎵 Music | `PhoneAppMusic` | MP3/WAV 播放器, SD 卡, ESP-GMF 音频管道 |
| ⚙️ **Settings** | `PhoneAppSettings` | **音量/亮度 滑条 + NVS 持久化 (WiFi 待启用)** |

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
│   ├── Kconfig.projbuild       # 项目 Kconfig 菜单
│   ├── example_config.h        # 引脚和参数宏定义
│   ├── main.cpp                # 主程序 (C++): BSP 外设初始化 + 5 个 App 安装
│   ├── phone_app_camera.hpp    # Camera App 头文件
│   ├── phone_app_camera.cpp    # Camera App (MIPI CSI + ISP + OV5647 sensor)
│   ├── phone_app_audio.hpp     # Audio App 头文件
│   ├── phone_app_audio.cpp     # Audio App (双 Mic 电平监控 + MP3 录音)
│   ├── phone_app_music.hpp     # Music App 头文件
│   ├── phone_app_music.cpp     # Music App (MP3/WAV 播放器)
│   ├── phone_app_settings.hpp  # Settings App 头文件
│   └── phone_app_settings.cpp  # Settings App (音量/亮度 + NVS, WiFi 条件编译)
├── components/
│   └── espressif__esp_lvgl_port/   # 本地补丁版 esp_lvgl_port
└── project_setup.md            # 本文档
```

## 关键依赖

| 组件 | 版本 | 来源 |
|------|------|------|
| `espressif/esp-brookesia` | 0.5.0 | ESP Registry |
| `waveshare/esp32_p4_wifi6_touch_lcd_4b` | 2.0.0 | ESP Registry |
| `espressif/esp_codec_dev` | 1.5.10 | ESP Registry |
| `espressif/esp_cam_sensor` | 1.7.0 | ESP Registry |
| `espressif/esp_sccb_intf` | 0.0.8 | ESP Registry |
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
| 1 | main | 默认(1) | 10 | ESP-IDF | 外设初始化 + 内存监控循环 (5s) |
| 2 | taskLVGL | 4 | 10 | `esp_lvgl_port` | LVGL 渲染 + 触摸输入 |
| 3 | audio_echo | 5 | 4 | `PhoneAppAudio::run()` | Mic 读取 + 电平计算 (运行时创建, 退出时销毁) |

## 引脚配置

### MIPI DSI (2-lane)
| 信号 | GPIO | 说明 |
|------|------|------|
| DSI_DATAP1 | 34 | FPC D1+ |
| DSI_DATAN1 | 35 | FPC D1- |
| DSI_CLKN | 36 | FPC CLK- |
| DSI_CLKP | 37 | FPC CLK+ |
| DSI_DATAP0 | 38 | FPC D0+ |
| DSI_DATAN0 | 39 | FPC D0- |

### MIPI CSI (2-lane, OV5647) — 与 SDMMC 共享 GPIO39/43/44
| 信号 | GPIO | 说明 |
|------|------|------|
| CSI_DATAP0 | 43 | DAT0+ |
| CSI_DATAN0 | 44 | DAT0- |
| CSI_CLKP | 45 | CLK+ |
| CSI_CLKN | 46 | CLK- |
| CSI_DATAP1 | 47 | DAT1+ |
| CSI_DATAN1 | 48 | DAT1- |

### SDMMC (4-bit) — 与 MIPI CSI 共享 GPIO39/43/44
| 信号 | GPIO | 说明 |
|------|------|------|
| SD_CLK | 43 | Clock |
| SD_CMD | 44 | Command |
| SD_D0 | 39 | Data 0 |
| SD_D1 | 40 | Data 1 |
| SD_D2 | 41 | Data 2 |
| SD_D3 | 42 | Data 3 |

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


## 外设初始化流程

```
app_main()
  ├─ 0. NVS Flash (nvs_flash_init + 亮度加载)
  ├─ 1. MIPI DSI Display (bsp_display_start_with_config)
  │      → ST7703 720×720 LCD + GT911 Touch
  │      → LVGL taskLVGL 创建
  │
  ├─ 2. ESP-Brookesia Phone UI (5 apps installed)
  │      → PhoneAppSquareline
  │      → PhoneAppCamera, PhoneAppAudio
  │
  ├─ 3. SDMMC SD Card (4-bit, FAT)
  │
  ├─ 4. Audio (手动 I2S + ES8311 + ES7210)
  │      → PA GPIO 53 HIGH
  │      → I2S 16000Hz Stereo Duplex
  │      → ES8311 codec (speaker output)
  │      → ES7210 codec (dual mic input, 30dB gain)
  │
  └─ 5. Memory Monitor Loop (每 5s)
```


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

### 5. MIPI CSI 与 SDMMC 引脚冲突
GPIO39/43/44 同时用于 MIPI CSI 和 SDMMC，两者不能同时使用:
- 默认启用 SDMMC (SD 卡已挂载)
- 打开 Camera App 时自动卸载 SD 卡并初始化摄像头
- 关闭 Camera App 后释放 CSI/ISP 资源

### 6. Camera App 实现

`PhoneAppCamera` 继承 `ESP_Brookesia_PhoneApp`，主要技术细节:

| 项目 | 配置 |
|------|------|
| 传感器 | OV5647, I2C auto-detect, 格式 `MIPI_2lane_24Minput_RAW8_800x800_50fps` |
| CSI | 2-lane, 200Mbps, RAW8 input |
| ISP | RAW8→RGB565, 80MHz clock |
| 帧缓冲 | PSRAM, `heap_caps_aligned_alloc(128, ...)`, 128字节对齐 (cache line) |
| 显示 | LVGL `lv_canvas` + buffer 零拷贝, 30fps 定时器刷新 |
| 传感器初始化 | `esp_cam_sensor` auto-detect → `set_format` → `ioctl(S_STREAM)` |
| 清理顺序 | sensor stop → del_dev → sccb_del → CSI stop→disable→del → ISP disable→del → free buf |

**已解决的问题**:
- Buffer 溢出: OV5647 输出 800×800, 帧缓冲需匹配 800×800 (1.28MB)
- 显示适配: LVGL v9 不支持 `lv_image_set_src` 动态 buffer, 改用 `lv_canvas`
- CSI 控制器泄漏: 必须按 stop→disable→del 顺序, 否则第二次打开失败
- SCCB 链接: ESL 头文件需 `extern "C"` 包裹
- 音量振荡: 移除 mic→speaker 回声功能

### 7. Audio App 实现

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

### 13. Settings App (NVS 持久化)

`PhoneAppSettings` 继承 `ESP_Brookesia_PhoneApp`，提供系统设置界面:

| 设置项 | 控件 | NVS Key | 范围 | 默认值 |
|--------|------|---------|------|--------|
| 🔉 音量 | LVGL Slider | `volume` | 0-100 | 60 |
| ☀️ 屏幕亮度 | LVGL Slider | `brightness` | 20-100 | 80 |
| 📶 Wi-Fi | LVGL Switch + 扫描列表 + 密码输入 | `wifi_en`, `ssid`, `pass` | - | - |

**NVS 命名空间**: `"settings"`

**WiFi 说明**: WiFi 代码通过 `#ifdef SETTINGS_WIFI_ENABLED` 条件编译。ESP32-P4 使用 `CONFIG_ESP_HOST_WIFI_ENABLED=y`（而非 `CONFIG_ESP_WIFI_ENABLED`），因为 P4 本身没有内置 WiFi，需要 ESP32-C6 通过 SDIO 提供远程 WiFi 传输。

**全局共享**:
- 音量通过 `s_codec_handle` → `esp_codec_dev_set_out_vol()` 设置
- 亮度通过 `bsp_display_brightness_set()` 设置
- Music App 启动时从 NVS 读取音量
- 主程序启动后从 NVS 读取并应用亮度
- 所有更改立即保存到 NVS

## 构建和烧录

```bash
# 设置环境
source ~/.espressif/v6.0.1/esp-idf/export.sh

# 构建
cd esp32p4_monitor
idf.py set-target esp32p4
idf.py build

# 烧录
idf.py -p /dev/ttyUSB0 flash monitor
```

## 待完成事项
- [x] Camera App 框架 (PhoneAppCamera 类 + CSI/ISP + OV5647 Sensor Init)
- [x] Audio App 框架 (PhoneAppAudio 类 + 双 Mic 电平监控)
- [x] ES7210 ADC 初始化 + Mic Gain 配置
- [x] Camera App 打开/关闭/重新打开 生命周期
- [x] CSI/ISP 正确释放 (stop→disable→del 顺序)
- [ ] Camera 在 LCD 显示有问题, 红色显示成绿色
- [x] Camera App 关闭后重新挂载 SD 卡
- [ ] 自定义 720x720 ESP-Brookesia 样式表
- [ ] WiFi/BLE 支持 (通过 ESP32-C6 SDIO)
- [ ] Camera App 回放/录制功能
- [x] Audio App Speaker 输出功能 (需解决回声消除)
- [x] Audio App MP3 录音 (Shine encoder, SD 卡)
- [x] Music App LVGL 线程安全 (GMF 回调加 lvgl_port_lock)
- [x] Settings App LVGL 线程安全 (wifiConnectTaskHandler 加 bsp_display_lock)
