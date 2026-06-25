# ESP32-P4 Monitor Project Setup

## 项目概述
基于 ESP32-P4 + Waveshare ESP32-P4-WiFi6-Touch-LCD-4B 开发板的综合监控项目,集成:
- **MIPI DSI** 显示 (720x720, ST7703, 通过 Waveshare BSP)
- **MIPI CSI** 摄像头 (OV5647, ISP 处理 RAW8→RGB565)
- **SDMMC** SD 卡 (4-bit 模式, FAT 文件系统)
- **音频输入/输出** (ES8311 DAC + ES7210 ADC, I2S)
- **UI** ESP-Brookesia Phone 桌面 (LVGL v9.2.2)

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
│   └── main.cpp                # 主程序 (C++)
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
| `espressif/esp_lvgl_port` | 2.8.0~1 | **本地补丁版** |
| `lvgl/lvgl` | 9.2.2 | ESP Registry |

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

### 音频 I2S (ES8311 + ES7210)
| 信号 | GPIO | 说明 |
|------|------|------|
| I2S_MCLK | 13 | 主时钟 |
| I2S_BCLK | 12 | 位时钟 |
| I2S_LRCK | 10 | 左右声道时钟 |
| I2S_SDIN | 9 | ES8311 DAC 输入 |
| I2S_SDOUT | 11 | ES7210 ADC 输出 |

### I2C 共享总线 (GT911触摸 + 音频Codec)
| 信号 | GPIO | 说明 |
|------|------|------|
| I2C_SDA | 7 | 数据 |
| I2C_SCL | 8 | 时钟 |

## 关键修改和问题解决

### 1. esp_lvgl_port 兼容性问题
**问题**: ESP Registry 的 `espressif/esp_lvgl_port: 2.8.0~1` 引用了 `LV_COLOR_FORMAT_RGB565_SWAPPED`,该符号在 `lvgl: 9.2.2` 中不存在。

**解决**: 从 `phone_p4_function_ev_board` 项目复制了本地补丁版 `espressif__esp_lvgl_port` 到 `components/` 目录,该版本已修复此兼容性问题。

### 2. C++ 指定初始化器顺序
**问题**: `main.cpp` 使用 C++ 编译,对 struct 设计化初始化器的字段顺序要求严格（必须与声明顺序一致）。多个 struct 初始化顺序错误:
- `esp_cam_ctlr_csi_config_t`: 添加缺失的 `clk_src`、`data_type` 字段,调整为声明顺序
- `es8311_codec_cfg_t`: 调整为声明顺序,添加缺失的 `digital_mic`、`invert_mclk`、`invert_sclk`、`no_dac_ref` 字段

### 3. gpio_num_t 类型转换
**问题**: Kconfig 生成的 `CONFIG_EXAMPLE_PIN_*` 和 `CONFIG_EXAMPLE_I2S_*` 值为 `int`,但 GPIO 函数期望 `gpio_num_t` (enum)。C++ 不允许隐式 int→enum 转换。

**解决**: 所有 GPIO 引脚赋值添加显式 `(gpio_num_t)` 类型转换。

### 4. i2s_mclk_multiple_t 类型
**问题**: `mclk_multiple` 字段类型为 `i2s_mclk_multiple_t` (enum),但宏定义为整数 384。

**解决**: 直接使用 `I2S_MCLK_MULTIPLE_384` 枚举值。

### 5. I2C 总线共享
**说明**: GT911 触摸控制器、ES8311、ES7210、OV5647 共享同一物理 I2C 总线 (GPIO7/8)。BSP 初始化 I2C_NUM_0 (`bsp_display_start` 内调用 `bsp_i2c_init`),音频和摄像头复用 BSP 的 I2C 句柄 (`bsp_i2c_get_handle()`),避免重复初始化冲突。

### 6. MIPI CSI 与 SDMMC 引脚冲突
**说明**: GPIO39/43/44 同时用于 MIPI CSI 和 SDMMC。两者不能同时使用。当前代码默认启用 SDMMC,摄像头初始化代码已保留但被注释 (`monitor_init_camera()`)。如需使用摄像头,需禁用 SDMMC。

### 7. ESP-Brookesia 样式表适配
**说明**: 720x720 分辨率没有对应的 ESP-Brookesia 预置样式表。当前使用 480x480 样式表作为回退方案。需要自定义 720x720 样式表以获得最佳显示效果。

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
- [ ] 自定义 720x720 ESP-Brookesia 样式表
- [ ] OV5647 传感器初始化 (需接入 I2C 总线的传感器驱动)
- [ ] ES7210 ADC 独立初始化配置
- [ ] WiFi/BLE 支持 (通过 ESP32-C6 SDIO)
- [ ] 摄像头与 SDMMC 动态切换
