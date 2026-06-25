### Hardware Info
- [ESP32-P4-WIFI6-Touch-LCD-4B](https://docs.waveshare.net/ESP32-P4-WIFI6-Touch-LCD-4B)
  - ESP32-P4NRW32 + 32MB Nor Flash
    - chip version v1.x and CPU frequency 360 MHz
  - ESP32-C6-MINI-1U-H8 with SDIO connected to ESP32-P4 for Wi-Fi 6 and Bluetooth 5 (LE) Zigbee and Thread
    |   Signal    |     P4      |    C6   |
    |:----:|:----:|:----:|
    |   SDIO_CLK	|   SDMMC1_CLK	    |   (GPIO18)    |	CLK     |
    |   SDIO_CMD	|   SDMMC1_CMD      |   (GPIO19)    |	CMD     |
    |   SDIO_D0	    |   SDMMC1_CDATA0	|   (GPIO14)    |	DAT0    |
    |   SDIO_D1	    |   SDMMC1_CDATA1	|   (GPIO15)    |	DAT1    |
    |   SDIO_D2	    |   SDMMC1_CDATA2   |   (GPIO16)    |	DAT2    |
    |   SDIO_D3	    |   SDMMC1_CDATA3	|   (GPIO17)    |	DAT3    |
  - MIPI DSI 2lane, LCD resolution 720 * 720, 4 inch, driver: waveshare/esp32_p4_wifi6_touch_lcd_4b
    |   Signal    |     P4      |    MIPI DSI   |
    |:----:|:----:|:----:|
    |   DSI_DATAP1	|   GPIO34  |	FPC D1+     |
    |   DSI_DATAN1	|   GPIO35  |	FPC D1−     |
    |   DSI_CLKN	|   GPIO36  |	FPC CLK−    |
    |   DSI_CLKP	|   GPIO37  |	FPC CLK+    |
    |   DSI_DATAP0	|   GPIO38  |	FPC D0+     |
    |   DSI_DATAN0	|   GPIO39  |	FPC D0−     |
  - Touch Panel: GT911 (I2C, shared with audio codec)
    |   Signal  |   P4 GPIO   |   Direction   |   GT911  |
    |:----:|:----:|:----:|:----:|
    |   I2C_SDA	|   GPIO7	|   P4↔GT911	|   I2C data  |
    |   I2C_SCL	|   GPIO8	|   P4↔GT911	|   I2C clk   |
    |   TP_RST	|   GPIO23	|   P4→GT911	|   Reset     |
    |   TP_INT	|   NC    	|   --    	|   Interrupt |
  - MIPI CSI 2lane, camera: OV5647
    |   Signal    |     P4      |    MIPI CSI    |
    |:----:|:----:|:----:|
    |   CSI_DATAP0	|   GPIO43	|   FPC     |   DAT0+   |
    |   CSI_DATAN0	|   GPIO44	|   FPC     |   DAT0−   |
    |   CSI_CLKP	|   GPIO45	|   FPC     |   CLK+    |
    |   CSI_CLKN	|   GPIO46	|   FPC     |   CLK−    |
    |   CSI_DATAP1	|   GPIO47	|   FPC     |   DAT1+   |
    |   CSI_DATAN1	|   GPIO48	|   FPC     |   DAT1−   |
    |   CSI_REXT	|   GPIO49	|   --      |   4.02 kΩ |
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
  - SDMMC:
    |   Signal	|   P4 GPIO	|   SD Card     |   Pin	Description |
    |:----:|:----:|:----:|:----:|
    |   SD_CLK	|   GPIO43	|   Pin 5	    |   Clock, 10k pull-up  |
    |   SD_CMD	|   GPIO44	|   Pin 2	    |   Command, 10k pull-up    |
    |   SD_D0	|   GPIO39	|   Pin 7	    |   Data 0, 10k pull-up |
    |   SD_D1	|   GPIO40	|   Pin 8	    |   Data 1, 10k pull-up (4-bit mode)    |
    |   SD_D2	|   GPIO41	|   Pin 9	    |   Data 2, 10k pull-up (4-bit mode)    |
    |   SD_D3	|   GPIO42	|   Pin 1	    |   Data 3, 10k pull-up (also serves as card detect)    |
    |   SD_VDD	|   LDO_VO4 |   Pin 4	    |   Card power supply   |
    |   SD_VSS	|   GND	    |   Pin 3/6	    |   Ground  |
- ESP32P4:
  - [ESP32-P4 Datasheet](https://documentation.espressif.com/esp32-p4_datasheet_en.pdf)
  - [ESP32-P4 Technical Reference Manual](https://documentation.espressif.com/esp32-p4_technical_reference_manual_en.pdf)
- ESP-IDF Version: V6.0.1
