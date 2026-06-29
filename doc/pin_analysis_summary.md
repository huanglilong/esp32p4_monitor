# ESP32-P4 Waveshare Board Pin Analysis Summary

## 核心发现：Pin Number vs GPIO Number 区分

**ESP32-P4 芯片的物理引脚（Pin Number）与 GPIO Number 是两个不同的概念。** 参考数据手册 Table 2-1 (Pin Overview) 和 Table 2-3 (IO MUX Pin Functions)。

### 实际对应关系

| 引脚号 | Pin Name | 类型 | 电源域 | 说明 |
|--------|----------|------|--------|------|
| 34 | DSI_REXT | Dedicated IO | VDD_MIPI_DPHY | MIPI DSI 4.02kΩ 外部电阻 |
| 35 | DSI_DATAP1 | Dedicated IO | VDD_MIPI_DPHY | MIPI DSI PHY DATAP1 |
| 36 | DSI_DATAN1 | Dedicated IO | VDD_MIPI_DPHY | MIPI DSI PHY DATAN1 |
| 37 | DSI_CLKN | Dedicated IO | VDD_MIPI_DPHY | MIPI DSI PHY CLKN |
| 38 | DSI_CLKP | Dedicated IO | VDD_MIPI_DPHY | MIPI DSI PHY CLKP |
| 39 | DSI_DATAP0 | Dedicated IO | VDD_MIPI_DPHY | MIPI DSI PHY DATAP0 |
| 40 | DSI_DATAN0 | Dedicated IO | VDD_MIPI_DPHY | MIPI DSI PHY DATAN0 |
| 41 | VDD_MIPI_DPHY | Power | - | MIPI DPHY 电源 |
| 42 | CSI_DATAN0 | Dedicated IO | VDD_MIPI_DPHY | MIPI CSI PHY DATAN0 |
| 43 | CSI_DATAP0 | Dedicated IO | VDD_MIPI_DPHY | MIPI CSI PHY DATAP0 |
| 44 | CSI_CLKP | Dedicated IO | VDD_MIPI_DPHY | MIPI CSI PHY CLKP |
| 45 | CSI_CLKN | Dedicated IO | VDD_MIPI_DPHY | MIPI CSI PHY CLKN |
| 46 | CSI_DATAN1 | Dedicated IO | VDD_MIPI_DPHY | MIPI CSI PHY DATAN1 |
| 47 | CSI_DATAP1 | Dedicated IO | VDD_MIPI_DPHY | MIPI CSI PHY DATAP1 |
| 48 | CSI_REXT | Dedicated IO | VDD_MIPI_DPHY | MIPI CSI 4.02kΩ 外部电阻 |
| 80 | GPIO39 | IO | VDD_IO_5 | 实际 GPIO39 (SDMMC D0 / SDSPI MISO) |
| 81 | GPIO40 | IO | VDD_IO_5 | 实际 GPIO40 (SDMMC D1) |
| 82 | GPIO41 | IO | VDD_IO_5 | 实际 GPIO41 (SDMMC D2) |
| 83 | GPIO42 | IO | VDD_IO_5 | 实际 GPIO42 (SDMMC D3 / SDSPI CS) |
| 84 | GPIO43 | IO | VDD_IO_5 | 实际 GPIO43 (SDMMC CLK / SDSPI SCLK) |
| 86 | GPIO44 | IO | VDD_IO_5 | 实际 GPIO44 (SDMMC CMD / SDSPI MOSI) |

### 关键结论

**MIPI DSI 引脚 34-40 和 MIPI CSI 引脚 42-48 是专用接口引脚（Dedicated Interface Pins），不是 GPIO。**

它们属于 **VDD_MIPI_DPHY** 电源域，功能固定为 MIPI DPHY 信号，不能作为通用 GPIO 使用。

**GPIO39-44 是物理引脚 80-86**，属于 **VDD_IO_5** 电源域，通过 IO MUX 可配置为 SDMMC/SDSPI/GPIO 功能。

**两者是完全不同的物理引脚，不存在引脚冲突！**

---

## MIPI CSI 与 SD Card 引脚冲突分析

### 纠正：SD卡实际使用配置

本项目实际使用的是 **SDSPI 模式**（非 SDMMC 4-bit），因为 SDMMC_HOST_SLOT_0 被 ESP32-C6 WiFi (esp_hosted) 通过 SDIO 占用。

固件代码 (`main/main.cpp:163-211`):
```cpp
// SD Card (SPI mode — SDMMC slot 0 blocked by esp_hosted C6 WiFi on slot 1)
// SPI pins: CS=GPIO42(D3), MOSI=GPIO44(CMD), SCLK=GPIO43(CLK), MISO=GPIO39(D0)
```

| SPI 信号 | GPIO | 物理引脚 | 电源域 |
|----------|------|----------|--------|
| MISO | GPIO39 | 80 | VDD_IO_5 |
| CS | GPIO42 | 83 | VDD_IO_5 |
| SCLK | GPIO43 | 84 | VDD_IO_5 |
| MOSI | GPIO44 | 86 | VDD_IO_5 |

### MIPI CSI 引脚

| CSI 信号 | 物理引脚 | 物理 Pin Name | 电源域 |
|----------|----------|---------------|--------|
| CSI_DATAN0 | 42 | CSI_DATAN0 | VDD_MIPI_DPHY |
| CSI_DATAP0 | 43 | CSI_DATAP0 | VDD_MIPI_DPHY |
| CSI_CLKP | 44 | CSI_CLKP | VDD_MIPI_DPHY |
| CSI_CLKN | 45 | CSI_CLKN | VDD_MIPI_DPHY |
| CSI_DATAN1 | 46 | CSI_DATAN1 | VDD_MIPI_DPHY |
| CSI_DATAP1 | 47 | CSI_DATAP1 | VDD_MIPI_DPHY |
| CSI_REXT | 48 | CSI_REXT | VDD_MIPI_DPHY |

### 结论

> **MIPI CSI 与 SD Card 不存在引脚冲突。**
>
> - CSI 使用专用 MIPI DPHY 引脚（物理引脚 42-48），电源域 VDD_MIPI_DPHY
> - SD Card (SDSPI) 使用 GPIO39/42/43/44（物理引脚 80/83/84/86），电源域 VDD_IO_5
> - 两者是不同的物理引脚，且属于不同的电源域
>
> 误判原因：以前错误地将原理图中的物理引脚号（43、44、39）理解为 GPIO 号。

### 关于 Camera App 中卸载 SD 卡的原因

`phone_app_camera.cpp` 中卸载 SD 卡的操作可能是**遗留代码**，基于之前的误判。现在确认 `MIPI CSI` 和 `SD Card` 使用完全独立的物理引脚，理论上不需要卸载。但保留该操作不会造成问题（仅暂时不可用）。

---

## MIPI DSI 与 SD Card 引脚冲突分析

### MIPI DSI 引脚

| DSI 信号 | 物理引脚 | 物理 Pin Name | 电源域 |
|----------|----------|---------------|--------|
| DSI_REXT | 34 | DSI_REXT | VDD_MIPI_DPHY |
| DSI_DATAP1 | 35 | DSI_DATAP1 | VDD_MIPI_DPHY |
| DSI_DATAN1 | 36 | DSI_DATAN1 | VDD_MIPI_DPHY |
| DSI_CLKN | 37 | DSI_CLKN | VDD_MIPI_DPHY |
| DSI_CLKP | 38 | DSI_CLKP | VDD_MIPI_DPHY |
| DSI_DATAP0 | 39 | DSI_DATAP0 | VDD_MIPI_DPHY |
| DSI_DATAN0 | 40 | DSI_DATAN0 | VDD_MIPI_DPHY |

### 结论

> **MIPI DSI 与 SD Card 也不存在引脚冲突。**
>
> - DSI 使用专用 MIPI DPHY 引脚（物理引脚 34-40），电源域 VDD_MIPI_DPHY
> - SD Card 使用 GPIO（物理引脚 80-86），电源域 VDD_IO_5
> - 两者是完全不同的物理引脚

---

## README.md / PROJECT.md 需要修正的内容

### 1. MIPI DSI 表格
- 列标题应改为 `Pin` 而非 `GPIO`
- 引脚号应为物理引脚号 35-40（DSI_REXT 为引脚 34）
- 当前 README 表格中的编号已修正为 GPIO36-41，但需要改为无 `GPIO` 前缀的纯引脚号

### 2. MIPI CSI 表格
- 列标题应改为 `Pin` 而非 `GPIO`
- 引脚号应为物理引脚号 42-48
- 当前表中编号为 GPIO43-48 恰好与 CSI 物理引脚号巧合相同（43-48），但因标记为 GPIO 导致误解

### 3. SDMMC 表格
- SD 卡使用真正的 GPIO39-44（物理引脚 80-86）
- 应保留 `GPIO` 标记

### 4. 冲突描述
- 删除 "MIPI CSI 与 SDMMC 引脚冲突" 的错误描述
- Camera App 中卸载 SD 卡的理由需要更新

---

## 数据来源

1. **ESP32-P4 Series Datasheet v0.6** - Table 2-1 Pin Overview, Table 2-3 IO MUX Pin Functions, Table 2-9 Dedicated Interface Pins
2. **Waveshare ESP32-P4-WiFi6-Touch-LCD-4B 原理图** - `doc/ESP32-P4-WIFI6-Touch-LCD-4B.pdf`
3. **Waveshare BSP 代码** - `managed_components/waveshare__esp32_p4_wifi6_touch_lcd_4b/include/bsp/esp32_p4_wifi6_touch_lcd_4b.h`
4. **项目固件代码** - `main/main.cpp` (SDSPI 配置)
