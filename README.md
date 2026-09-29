### Hardware Info
- [ESP32-P4-WIFI6](https://docs.waveshare.net/ESP32-P4-WIFI6)
  - ESP32-P4NRW32 + 32MB Nor Flash + 32MB PSRAM
    - chip version v1.x and CPU frequency 360 MHz
  - ESP32-C6-MINI-1 with SDIO connected to ESP32-P4 for Wi-Fi 6 and Bluetooth 5 (LE) Zigbee and Thread
    |   Signal    |     P4      |    C6   |
    |:----:|:----:|:----:|
    |   SDIO_CLK	|   SDMMC1_CLK	    |   (GPIO18)    |	CLK     |
    |   SDIO_CMD	|   SDMMC1_CMD      |   (GPIO19)    |	CMD     |
    |   SDIO_D0	    |   SDMMC1_CDATA0	|   (GPIO14)    |	DAT0    |
    |   SDIO_D1	    |   SDMMC1_CDATA1	|   (GPIO15)    |	DAT1    |
    |   SDIO_D2	    |   SDMMC1_CDATA2   |   (GPIO16)    |	DAT2    |
    |   SDIO_D3	    |   SDMMC1_CDATA3	|   (GPIO17)    |	DAT3    |
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
  - Audio codec ES8311 (single chip, ADC + DAC) + NS4150B power amp:
    |   Signal  |   P4 GPIO |   Direction   |   ES8311  |
    |:----:|:----:|:----:|:----:|
    |   I2C_SDA         |   GPIO7    |  P4↔ES8311   |   I2C data (addr 0x18) |
    |   I2C_SCL         |   GPIO8    |  P4↔ES8311   |   I2C clk  |
    |   DAC_I2S_MCLK    |   GPIO13   |  P4→ES8311   |   MCLK    |
    |   DAC_I2S_SCLK	|   GPIO12   |  P4→ES8311	|   BCLK    |
    |   DAC_I2S_LRCK	|   GPIO10   |  P4→ES8311	|   LRCK    |
    |   DAC_I2S_SDIN	|   GPIO9    |  P4→ES8311	|   PCM out (speaker) |
    |   ADC_I2S_SDOUT	|   GPIO11   |  P4←ES8311	|   PCM in (mic)  |
    |   PA_CTRL         |   GPIO53   |  P4→NS4150B  |   Power Amp Enable (HIGH=ON) |
  - SDMMC/SDSPI:

    > **注意**: SD 卡使用真实的 GPIO 引脚 (物理引脚 80-86, 电源域 VDD_IO_5), 通过 IO MUX 可配置为 SDMMC 4-bit 或 SDSPI 模式。
    > **本项目实际使用 SDSPI 模式**: SDMMC_HOST_SLOT_0 被 ESP32-C6 WiFi (SDIO) 占用, 详见 `main/drivers/sdcard/sdcard_driver.cpp` (boot 挂载 SDSPI, LDO4 由 `sd_pwr_ctrl` API 管理)。

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
- ESP32P4:
  - [ESP32-P4 Datasheet](https://documentation.espressif.com/esp32-p4_datasheet_en.pdf)
  - [ESP32-P4 Technical Reference Manual](https://documentation.espressif.com/esp32-p4_technical_reference_manual_en.pdf)
- ESP-IDF Version: v6.x
  - Build, Flash and Monitor
    - MacOS:
      ```
      $ source ~/.espressif/v6.x/esp-idf/export.sh
      $ idf.py build && idf.py flash -b 1500000 -p $(ls /dev/cu.usbmodem*) monitor
      ```
    - Linux:
      ```
      $ source ~/.espressif/v6.x/esp-idf/export.sh
      $ idf.py build && idf.py flash -b 1500000 -p /dev/ttyACM0 monitor
      ```

> **历史说明**: 本项目曾同时支持 ESP32-P4-WIFI6-Touch-LCD-4B (4寸 720×720 LCD + GT911 触摸 +
> ES8311/ES7210 双芯片音频, ESP-Brookesia Phone UI)。LCD-4B 支持已于 2026-09-29 全部移除,
> 现仅支持无屏的 ESP32-P4-WIFI6 (headless, Web/Flutter App 交互)。

### Software Features
- **MIPI CSI** OV5647 camera (V4L2, ~5fps sensor via VTS=9840, HW JPEG, PPA-accelerated preprocessing; Camera Stream PPA resizes to 300×300 for efficient MJPEG encoding)
- **Camera Stream** MJPEG WiFi streaming (HTTP port 80/81, mDNS, PPA-accelerated 300×300 JPEG encoding, independent capture task, TCP keep-alive, image rotation 0°/90°/180°/270°; toggle via Web API)
- **Audio** ES8311 single-chip codec (mic input + speaker output), AAC recording (ESP AAC encoder, SD card) + playback (ESP-GMF simple player), all controlled via Web API
- **Web Config** HTTP :8080 (WiFi/volume/settings, audio record/play, file manager, ULog control, system stats/alerts, camera stream + auto frame recording + take-a-picture to SD, SD card format (FAT repair), WiFi recovery httpd restart, captive portal for headless config)
- **Flutter App** Cross-platform (macOS/iOS/Linux/Android) with device discovery, settings, ULog video+audio viewer (parse .ulg frames, slideshow, save)
- **uORB** PX4-style pub/sub message bus (FreeRTOS Queue, .msg auto-generation)
- **ULog** PX4-compatible binary log format (SD card, SNTP date naming, file rotation, pyulog compatible, auto-start on WiFi+SNTP, camera frame + audio frame recording, capacity-based cleanup at start + each rotation with critical free-space override)
- **System Monitor** Per-core CPU busy%, heap/PSRAM tracking, resource alerts (uORB + ULog + Web API)
- **Logger** Text log to SD card (ring buffer + writer task, file rotation, esp_log_set_vprintf interception)
- **Captive DNS** UDP 53 DNS hijack + DHCP DNS advertisement for headless WiFi provisioning
- **Driver Architecture** PeripheralManager facade → AudioDriver + SDCardDriver + CameraDriver + SystemMonitor
