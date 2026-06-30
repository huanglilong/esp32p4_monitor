# Waveshare ESP32-P4-WIFI6 vs ESP32-P4-WIFI6-Touch-LCD-4B 外设接线对比

> 数据来源: 
> - [ESP32-P4-WIFI6 官方Wiki](https://www.waveshare.com/wiki/ESP32-P4-WIFI6) + 原理图 (`doc/ESP32-P4-WIFI6-datasheet.pdf`)
> - [ESP32-P4-WIFI6-Touch-LCD-4B 官方Wiki](https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-4B) + 原理图 (`doc/ESP32-P4-WIFI6-Touch-LCD-4B.pdf`)
> - 本项目 BSP 头文件: `managed_components/waveshare__esp32_p4_wifi6_touch_lcd_4b/include/bsp/esp32_p4_wifi6_touch_lcd_4b.h`

---

## 一、基本硬件对比

| 特性 | ESP32-P4-WIFI6 | ESP32-P4-WIFI6-Touch-LCD-4B |
|:---|:---|:---|
| 主芯片 | ESP32-P4NRW32 | ESP32-P4NRW32 |
| PSRAM | 32MB (叠封) | 32MB (叠封) |
| Nor Flash | 32MB (QSPI) | 32MB (QSPI) |
| WiFi/BT 协处理器 | ESP32-C6-MINI-1 (板载PCB天线) | ESP32-C6-MINI-1U-H8 (IPEX外接天线) |
| P4↔C6 接口 | SDIO (SDMMC1) 4-bit | SDIO (SDMMC1) 4-bit |
| 板载显示屏 | **无** (仅有MIPI-DSI FPC座) | **4寸 IPS 720×720** (ST7703驱动) |
| 触摸屏 | **无** | GT911 电容触摸 (5点) |
| USB-UART | Type-C | Type-C |
| USB OTG | Type-A (USB 2.0 HS) | Type-C (USB 2.0 HS) |
| GPIO扩展 | **40-Pin排针** (2.54mm, 兼容部分Pi Pico HAT) | **扩展排针** (2.0mm间距, 连接86面板底板) |
| 以太网 | **无** | RJ45 100M (IP101 PHY, 仅ETH-2RO版本) |
| RS485 + 继电器 | **无** | **有** (仅ESP32-P4-86-Panel-ETH-2RO版本) |
| RTC电池座 | **无** | **有** |

---

## 二、相同之外设接线 (GPIO完全一致)

### 2.1 MIPI DSI (2-lane) — 专用接口引脚

> **注意**: MIPI DSI 使用 ESP32-P4 专用接口引脚 (Dedicated Interface Pins, 电源域 VDD_MIPI_DPHY), 不是 GPIO。以下编号为芯片物理引脚号。

| 信号 | P4 Pin | 说明 |
|:---|:---|:---|
| DSI_DATAP1 | 35 | FPC D1+ |
| DSI_DATAN1 | 36 | FPC D1− |
| DSI_CLKN | 37 | FPC CLK− |
| DSI_CLKP | 38 | FPC CLK+ |
| DSI_DATAP0 | 39 | FPC D0+ |
| DSI_DATAN0 | 40 | FPC D0− |

> WIFI6: 通过FPC座子外接5/7/8/10.1寸DSI屏幕；LCD-4B: 板载4寸720x720 LCD。

### 2.2 MIPI CSI (2-lane) — 专用接口引脚

| 信号 | P4 Pin | 说明 |
|:---|:---|:---|
| CSI_DATAN0 | 42 | DAT0− |
| CSI_DATAP0 | 43 | DAT0+ |
| CSI_CLKN | 45 | CLK− |
| CSI_CLKP | 44 | CLK+ |
| CSI_DATAN1 | 46 | DAT1− |
| CSI_DATAP1 | 47 | DAT1+ |
| CSI_REXT | 48 | 4.02 kΩ 参考电阻 |

> 两块板子均支持 OV5647 等 MIPI 摄像头, 物理引脚完全相同。

### 2.3 ESP32-C6 ←SDIO→ ESP32-P4

| 信号 | P4 GPIO | C6 功能 |
|:---|:---|:---|
| SDIO_CLK | GPIO18 (SDMMC1_CLK) | CLK |
| SDIO_CMD | GPIO19 (SDMMC1_CMD) | CMD |
| SDIO_D0 | GPIO14 (SDMMC1_CDATA0) | DAT0 |
| SDIO_D1 | GPIO15 (SDMMC1_CDATA1) | DAT1 |
| SDIO_D2 | GPIO16 (SDMMC1_CDATA2) | DAT2 |
| SDIO_D3 | GPIO17 (SDMMC1_CDATA3) | DAT3 |

### 2.4 SD/TF 卡

| 信号 | GPIO | 物理引脚 | 说明 |
|:---|:---|:---|:---|
| SD_CLK | 43 | 84 | SPI SCLK / SDMMC CLK |
| SD_CMD | 44 | 86 | SPI MOSI / SDMMC CMD |
| SD_D0 | 39 | 80 | SPI MISO / SDMMC D0 |
| SD_D1 | 40 | 81 | SDMMC D1 (4-bit) |
| SD_D2 | 41 | 82 | SDMMC D2 (4-bit) |
| SD_D3 | 42 | 83 | SPI CS / SDMMC D3 |

> 两块板子TF卡座引脚完全一致。注意 SDMMC_HOST_SLOT_0 被 C6 SDIO 占用, 实际使用 SDSPI 1-bit 模式。

### 2.5 I2C 共享总线

| 信号 | GPIO | 说明 |
|:---|:---|:---|
| I2C_SDA | GPIO7 | 数据线 |
| I2C_SCL | GPIO8 | 时钟线 |

> 两块板子 I2C 引脚完全一致。但挂载的设备不同 (见下文)。

### 2.6 I2S 音频 + 功放

| 信号 | GPIO | 说明 |
|:---|:---|:---|
| I2S_MCLK | GPIO13 | 主时钟 |
| I2S_BCLK (SCLK) | GPIO12 | 位时钟 |
| I2S_LRCK (WS) | GPIO10 | 左右声道时钟 |
| I2S_DOUT (ASDOUT) | GPIO11 | ADC 数据输出 (Mic) |
| I2S_DIN (DSDIN) | GPIO9 | DAC 数据输入 (Speaker) |
| PA_CTRL | GPIO53 | 功放使能 (HIGH=ON) |

> 两块板子 I2S 和 PA_CTRL 的 GPIO 完全一致。

---

## 三、不同之处

### 3.1 音频系统

| 特性 | ESP32-P4-WIFI6 | ESP32-P4-WIFI6-Touch-LCD-4B |
|:---|:---|:---|
| 音频Codec | **ES8311** (单芯片, 兼任ADC+DAC) | **ES8311** (DAC) + **ES7210** (ADC, 回声消除) |
| 功放 | **NS4150B** | 板载功放 (PA_CTRL=GPIO53) |
| 麦克风 | **1个** SMD麦克风 (单声道) | **2个** SMD麦克风 (双麦回声消除) |
| I2C地址 | ES8311: 0x18 | ES8311: 0x30, ES7210: 0x80 |

> **关键差异**: LCD-4B 使用双芯片方案 (ES8311+ES7210), 支持双麦回声消除, 适合语音交互场景。WIFI6 使用 ES8311 单芯片方案, 单声道录音, 成本更低。

### 3.2 显示屏与触摸

| 特性 | ESP32-P4-WIFI6 | ESP32-P4-WIFI6-Touch-LCD-4B |
|:---|:---|:---|
| 板载LCD | **无** | 4寸 720×720 IPS |
| LCD驱动 | 外接 (ILI9881C/JD9365等) | ST7703 |
| LCD_RST | — | **GPIO27** |
| LCD_BACKLIGHT | — | **GPIO26** |
| 触摸控制器 | **无** | GT911 (I2C) |
| TOUCH_RST | — | **GPIO23** |
| TOUCH_INT | — | **GPIO_NC** (未连接) |

> LCD-4B 独有的GPIO: GPIO26(LCD背光), GPIO27(LCD复位), GPIO23(触摸复位)。

### 3.3 以太网 (仅LCD-4B ETH-2RO版本)

| 特性 | ESP32-P4-WIFI6 | ESP32-P4-WIFI6-Touch-LCD-4B |
|:---|:---|:---|
| 以太网PHY | **无** | IP101 (RMII, 100M) |
| RJ45接口 | **无** | **有** |
| RS485 | **无** | **有** (仅ETH-2RO版本) |
| 继电器 | **无** | **2路** (仅ETH-2RO版本, ≤10A 250VAC) |

### 3.4 GPIO 扩展接口

| 特性 | ESP32-P4-WIFI6 | ESP32-P4-WIFI6-Touch-LCD-4B |
|:---|:---|:---|
| 接口形式 | **2×20 排针** (2.54mm间距) | **扩展排针** (2.0mm间距) |
| 兼容性 | 兼容部分 Raspberry Pi Pico HAT | 连接 Waveshare 86面板底板 |
| 可用GPIO | **27个** 可编程GPIO | 少量GPIO (大部分被板载外设占用) |

### 3.5 I2C 总线挂载设备

| 设备 | WIFI6 | LCD-4B |
|:---|:---|:---|
| ES8311 Codec | 0x18 | 0x30 |
| ES7210 ADC | — | 0x80 |
| GT911 触摸 | — | 0x5D (BSP内部处理) |
| OV5647 Camera | SCCB (auto-detect) | SCCB (auto-detect) |

### 3.6 其他差异

| 特性 | ESP32-P4-WIFI6 | ESP32-P4-WIFI6-Touch-LCD-4B |
|:---|:---|:---|
| C6模块天线 | PCB板载天线 | IPEX外接天线 |
| C6固件烧录 | 测试点焊盘 | **2.54 4-Pin焊盘** |
| RTC电池 | 无 | **有** (支持可充电RTC电池) |
| 电源输入 | USB Type-C (5V) | USB Type-C (5V) / DC 6~30V宽压 (仅ETH-2RO版本) |
| BOOT按钮 | 有 | 有 |
| RESET按钮 | 有 | 有 |
| PWR LED | 有 | 有 |

---

## 四、总结

### 相同点
两块板子的**核心外设接线完全一致**, 包括:
- MIPI DSI (物理引脚35-40)
- MIPI CSI (物理引脚42-48)  
- SDIO WiFi (GPIO14-19, SDMMC1)
- TF卡 (GPIO39-44)
- I2C (GPIO7-8)
- I2S + PA_CTRL (GPIO9-13, GPIO53)

这意味着**为一块板子编写的底层驱动代码, 在另一块板子上可以直接复用**。

### 不同点
差异主要集中在**板载功能外设**:
1. **LCD-4B 独有的GPIO**: GPIO23(Touch RST), GPIO26(LCD背光), GPIO27(LCD RST)
2. **音频方案不同**: WIFI6 用 ES8311 单芯片, LCD-4B 用 ES8311+ES7210 双芯片(支持双麦回声消除)
3. **LCD-4B 独有**: 板载LCD+触摸、以太网、RS485、继电器(后三者仅ETH-2RO版本)
4. **WIFI6 独有**: 40-Pin GPIO排针(兼容Pi Pico HAT), 更多可编程GPIO

### 选型建议
- **需要板载LCD+触摸** → LCD-4B
- **需要大量GPIO扩展** → WIFI6 (27个可用GPIO)
- **需要以太网/RS485/继电器** → LCD-4B ETH-2RO版本
- **需要双麦语音交互** → LCD-4B
- **成本敏感 + 外接自己屏幕** → WIFI6
