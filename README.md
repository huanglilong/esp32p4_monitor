### Hardware Info
- [ESP32-P4-WIFI6-Touch-LCD-4B](https://docs.waveshare.net/ESP32-P4-WIFI6-Touch-LCD-4B)
  - ESP32-P4NRW32 + 32MB Nor Flash
    - chip version v1.x and CPU frequency 360 MHz
  - ESP32-C6-MINI-1U-H8 with SDIO connected to ESP32-P4 for Wi-Fi 6 and Bluetooth 5 (LE) Zigbee and Thread
  - Software: waveshare/esp32_p4_wifi6_touch_lcd_4b
    |   Signal    |     P4      |    C6   |
    |:----:|:----:|:----:|
    |   SDIO_CLK	|   SDMMC1_CLK	    |   (GPIO18)    |	CLK     |
    |   SDIO_CMD	|   SDMMC1_CMD      |   (GPIO19)    |	CMD     |
    |   SDIO_D0	    |   SDMMC1_CDATA0	|   (GPIO14)    |	DAT0    |
    |   SDIO_D1	    |   SDMMC1_CDATA1	|   (GPIO15)    |	DAT1    |
    |   SDIO_D2	    |   SDMMC1_CDATA2   |   (GPIO16)    |	DAT2    |
    |   SDIO_D3	    |   SDMMC1_CDATA3	|   (GPIO17)    |	DAT3    |
  - MIPI DSI 2lane, LCD resolution 720 * 720, 4 inch

    > **注意**: MIPI DSI 使用 ESP32-P4 专用接口引脚 (Dedicated Interface Pins, 电源域 VDD_MIPI_DPHY), 不是 GPIO。以下编号为芯片物理引脚号。

    |   Signal    |   P4 Pin    |    MIPI DSI   |
    |:----:|:----:|:----:|
    |   DSI_DATAP1	|   35   |	FPC D1+     |
    |   DSI_DATAN1	|   36   |	FPC D1−     |
    |   DSI_CLKN	|   37   |	FPC CLK−    |
    |   DSI_CLKP	|   38   |	FPC CLK+    |
    |   DSI_DATAP0	|   39   |	FPC D0+     |
    |   DSI_DATAN0	|   40   |	FPC D0−     |
  - Touch Panel: GT911 (I2C, shared with audio codec)
    |   Signal  |   P4 GPIO   |   Direction   |   GT911  |
    |:----:|:----:|:----:|:----:|
    |   I2C_SDA	|   GPIO7	|   P4↔GT911	|   I2C data  |
    |   I2C_SCL	|   GPIO8	|   P4↔GT911	|   I2C clk   |
    |   TP_RST	|   GPIO23	|   P4→GT911	|   Reset     |
    |   TP_INT	|   NC    	|   --    	|   Interrupt |
  - MIPI CSI 2lane, camera: OV5647

    > **注意**: MIPI CSI 使用 ESP32-P4 专用接口引脚 (Dedicated Interface Pins, 电源域 VDD_MIPI_DPHY), 不是 GPIO。以下编号为芯片物理引脚号。

    |   Signal    |   P4 Pin    |    MIPI CSI    |
    |:----:|:----:|:----:|
    |   CSI_DATAP0	|   43   |	FPC DAT0+   |
    |   CSI_DATAN0	|   42   |	FPC DAT0−   |
    |   CSI_CLKP	|   44   |	FPC CLK+    |
    |   CSI_CLKN	|   45   |	FPC CLK−    |
    |   CSI_DATAP1	|   47   |	FPC DAT1+   |
    |   CSI_DATAN1	|   46   |	FPC DAT1−   |
    |   CSI_REXT	|   48   |	4.02 kΩ    |
  - Two mic connected ES7210, and ES7210 attached to ESP32P4 I2S
    |   Signal  |   P4 GPIO   |   Direction   |   ES7210  |
    |:----:|:----:|:----:|:----:|
    |   I2C_SDA	        |   GPIO7	|   P4↔ES7210	|   codec       |
    |   I2C_SCL	        |   GPIO8	|   P4↔ES7210	|   I2C clk     |
    |   ADC_I2S_MCLK	|   GPIO13	|   P4→ES7210	|   ES7210 MCLK |
    |   ADC_I2S_SCLK	|   GPIO12	|   P4↔ES7210   |   BCLK        |
    |   ADC_I2S_LRCK	|   GPIO10	|   P4↔ES7210   |   LRCK/WS     |
    |   ADC_I2S_SDOUT	|   GPIO11	|   P4←ES7210	|   DOUT mic PCM|
  - Speaker with ES8311:
    |   Signal  |   P4 GPIO |   Direction   |   ES8311  |
    |:----:|:----:|:----:|:----:|
    |   DAC_I2S_MCLK    |   GPIO13   |  P4→ES8311   |   MCLK    |
    |   DAC_I2S_SCLK	|   GPIO12   |  P4→ES8311	|   BCLK    |
    |   DAC_I2S_LRCK	|   GPIO10   |  P4→ES8311	|   LRCK    |
    |   DAC_I2S_SDIN	|   GPIO9    |  P4→ES8311	|   PCM     |
    |   PA_CTRL         |   GPIO53   |  P4→NS4150B  |   Power Amp Enable (HIGH=ON) |
  - SDMMC/SDSPI:

    > **注意**: SD 卡使用真实的 GPIO 引脚 (物理引脚 80-86, 电源域 VDD_IO_5), 通过 IO MUX 可配置为 SDMMC 4-bit 或 SDSPI 模式。
    > **本项目实际使用 SDSPI 模式**: SDMMC_HOST_SLOT_0 被 ESP32-C6 WiFi (SDIO) 占用, 详见 `main/main.cpp:163`。

    |   Signal	|   P4 GPIO   |   Phys Pin     |   SD Card     |   Description |
    |:----:|:----:|:----:|:----:|:----:|
    |   SD_CLK	|   GPIO43    |   84	    |   Pin 5	    |   Clock / SPI SCLK, 10k pull-up  |
    |   SD_CMD	|   GPIO44    |   86	    |   Pin 2	    |   Command / SPI MOSI, 10k pull-up |
    |   SD_D0	|   GPIO39    |   80	    |   Pin 7	    |   Data 0 / SPI MISO, 10k pull-up |
    |   SD_D1	|   GPIO40    |   81	    |   Pin 8	    |   Data 1, 10k pull-up (4-bit mode) |
    |   SD_D2	|   GPIO41    |   82	    |   Pin 9	    |   Data 2, 10k pull-up (4-bit mode) |
    |   SD_D3	|   GPIO42    |   83	    |   Pin 1	    |   Data 3 / SPI CS, 10k pull-up (also card detect) |
    |   SD_VDD	|   LDO_VO4  |   --	    |   Pin 4	    |   Card power supply   |
    |   SD_VSS	|   GND	    |   --	    |   Pin 3/6	    |   Ground  |
- [ESP32-P4-WIFI6](https://docs.waveshare.net/ESP32-P4-WIFI6)
  - ESP32-P4NRW32 + 32MB Nor Flash
  - chip version v1.x and CPU frequency 360 MHz
  - ESP32-C6-MINI-1U-H8 with SDIO connected to ESP32-P4 for Wi-Fi 6 and Bluetooth 5 (LE) Zigbee and Thread
  - Software: https://gitee.com/waveshare/esp32-p4-platform
- ESP32P4:
  - [ESP32-P4 Datasheet](https://documentation.espressif.com/esp32-p4_datasheet_en.pdf)
  - [ESP32-P4 Technical Reference Manual](https://documentation.espressif.com/esp32-p4_technical_reference_manual_en.pdf)
- ESP-IDF Version: v6.x

### Software Features
- **MIPI DSI** 720×720 LCD + GT911 Touch (ESP-Brookesia Phone UI, LVGL v9.2.2)
- **MIPI CSI** OV5647 camera (V4L2, ~5fps, HW JPEG, ESP-DL human detection)
- **Camera Stream** MJPEG WiFi streaming (HTTP port 80/81, mDNS, inline detection, JPEG snapshot)
- **Audio** Dual mic monitoring + MP3 recording (Shine encoder, SD card) + Music playback (ESP-GMF)
- **Web Config** HTTP :8080 (WiFi/volume/settings, audio record/play, file manager, ULog control)
- **Flutter App** Cross-platform (macOS/iOS/Linux/Android) with device discovery and settings
- **uORB** PX4-style pub/sub message bus (FreeRTOS Queue, .msg auto-generation)
- **ULog** PX4-compatible binary log format (SD card, SNTP date naming, file rotation)
- **Multi-board** Auto-detect LCD-4B / WIFI6 via GT911 I2C probe, single firmware
- **Driver Architecture** PeripheralManager facade → AudioDriver + SDCardDriver + CameraDriver
