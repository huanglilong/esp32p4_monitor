/*
 * AudioDriver — manages audio I2S + codec lifecycle.
 *
 * Extracted from PeripheralManager for independent module ownership.
 * Reference-counted init/deinit, thread-safe codec operations.
 * Publishes volume_state via uORB when volume changes.
 */

#include "audio_driver.hpp"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es7210_adc.h"
#include "bsp/esp-bsp.h"
#include "example_config.h"
#include "uorb.h"
#include "topics.h"

extern "C" i2c_master_bus_handle_t bsp_i2c_get_handle(void);

static const char *TAG = "AudioDriver";

/*============================================================================
 * Singleton
 *============================================================================*/
AudioDriver& AudioDriver::instance(void)
{
    static AudioDriver s;
    return s;
}

AudioDriver::AudioDriver() :
    _lifecycle_mutex(nullptr),
    _codec_mutex(nullptr),
    _has_lcd(false),
    _refcount(0),
    _volume(EXAMPLE_VOICE_VOLUME),
    _codec_handle(nullptr),
    _codec_mic_handle(nullptr),
    _rx_handle(nullptr),
    _tx_handle(nullptr),
    _vol_pub(ORB_ADVERT_INVALID)
{
    _lifecycle_mutex = xSemaphoreCreateMutex();
}

AudioDriver::~AudioDriver()
{
    if (_lifecycle_mutex) {
        vSemaphoreDelete(_lifecycle_mutex);
        _lifecycle_mutex = nullptr;
    }
    if (_codec_mutex) {
        vSemaphoreDelete(_codec_mutex);
        _codec_mutex = nullptr;
    }
}

/*============================================================================
 * Audio I2S + Codec lifecycle
 *============================================================================*/
void AudioDriver::init(void)
{
    if (!_lifecycle_mutex) return;
    xSemaphoreTake(_lifecycle_mutex, portMAX_DELAY);

    if (_refcount > 0) {
        _refcount++;
        ESP_LOGI(TAG, "Audio already initialized (refcount=%d)", _refcount);
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }

    ESP_LOGI(TAG, "Initializing audio (%s)...", _has_lcd ? "ES8311 + ES7210" : "ES8311 single-chip");

    /* Enable PA GPIO 53 (critical for speaker output) */
    gpio_config_t pa_conf = {
        .pin_bit_mask = (1ULL << 53),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_conf);
    gpio_set_level((gpio_num_t)53, 1);
    ESP_LOGI(TAG, "PA GPIO 53 enabled");

    /* I2S channel init (duplex, STD, 48kHz stereo) */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &_tx_handle, &_rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)13,
            .bclk = (gpio_num_t)12,
            .ws   = (gpio_num_t)10,
            .dout = (gpio_num_t)9,
            .din  = (gpio_num_t)11,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = EXAMPLE_AUDIO_MCLK_MULTIPLE;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(_tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(_rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(_tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(_rx_handle));

    i2c_master_bus_handle_t i2c_handle = bsp_i2c_get_handle();
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    assert(gpio_if);

    if (_has_lcd) {
        /* ===== LCD-4B: ES8311 DAC (Speaker, 0x30) + ES7210 ADC (Mic, 0x80) ===== */
        audio_codec_i2s_cfg_t i2s_out_cfg = {
            .port = 0,
            .rx_handle = NULL,
            .tx_handle = _tx_handle,
        };
        const audio_codec_data_if_t *data_out = audio_codec_new_i2s_data(&i2s_out_cfg);
        assert(data_out);

        audio_codec_i2s_cfg_t i2s_in_cfg = {
            .port = 0,
            .rx_handle = _rx_handle,
            .tx_handle = NULL,
        };
        const audio_codec_data_if_t *data_in = audio_codec_new_i2s_data(&i2s_in_cfg);
        assert(data_in);

        audio_codec_i2c_cfg_t i2c_dac = { .port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_handle };
        const audio_codec_ctrl_if_t *ctrl_dac = audio_codec_new_i2c_ctrl(&i2c_dac);
        es8311_codec_cfg_t es8311_cfg = {
            .ctrl_if = ctrl_dac, .gpio_if = gpio_if,
            .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
            .pa_pin = 53, .master_mode = false, .use_mclk = true,
            .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
            .mclk_div = I2S_MCLK_MULTIPLE_256,
        };
        esp_codec_dev_cfg_t dev_dac = {
            .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es8311_codec_new(&es8311_cfg), .data_if = data_out
        };
        _codec_handle = esp_codec_dev_new(&dev_dac);
        assert(_codec_handle);

        audio_codec_i2c_cfg_t i2c_adc = { .port = 0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = i2c_handle };
        const audio_codec_ctrl_if_t *ctrl_adc = audio_codec_new_i2c_ctrl(&i2c_adc);
        es7210_codec_cfg_t es7210_cfg = {
            .ctrl_if = ctrl_adc, .master_mode = false,
            .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
            .mclk_src = ES7210_MCLK_FROM_PAD, .mclk_div = I2S_MCLK_MULTIPLE_256,
        };
        esp_codec_dev_cfg_t dev_adc = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = es7210_codec_new(&es7210_cfg), .data_if = data_in
        };
        _codec_mic_handle = esp_codec_dev_new(&dev_adc);
        assert(_codec_mic_handle);
    } else {
        /* ===== WIFI6: ES8311 single-chip (0x30, ADC + DAC) + NS4150B amp ===== */
        audio_codec_i2s_cfg_t i2s_data_cfg = {
            .port = 0,
            .rx_handle = _rx_handle,
            .tx_handle = _tx_handle,
        };
        const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
        assert(data_if);

        audio_codec_i2c_cfg_t i2c_es8311 = { .port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_handle };
        const audio_codec_ctrl_if_t *ctrl_es8311 = audio_codec_new_i2c_ctrl(&i2c_es8311);
        es8311_codec_cfg_t es8311_cfg = {
            .ctrl_if = ctrl_es8311, .gpio_if = gpio_if,
            .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
            .pa_pin = 53, .master_mode = false, .use_mclk = true,
            .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
            .mclk_div = I2S_MCLK_MULTIPLE_256,
        };
        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
            .codec_if = es8311_codec_new(&es8311_cfg),
            .data_if = data_if,
        };
        _codec_handle = esp_codec_dev_new(&dev_cfg);
        assert(_codec_handle);
        /* _codec_mic_handle stays NULL */
    }

    /* Create mutex BEFORE opening codecs */
    _codec_mutex = xSemaphoreCreateMutex();

    /* Open codecs */
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 2, .channel_mask = 0x03, .sample_rate = EXAMPLE_AUDIO_SAMPLE_RATE };
    if (_has_lcd) {
        ESP_ERROR_CHECK(esp_codec_dev_open(_codec_handle, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
        esp_codec_dev_set_out_vol(_codec_handle, _volume);
        ESP_ERROR_CHECK(esp_codec_dev_open(_codec_mic_handle, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
        esp_codec_dev_set_in_gain(_codec_mic_handle, 42);
    } else {
        ESP_ERROR_CHECK(esp_codec_dev_open(_codec_handle, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL);
        esp_codec_dev_set_out_vol(_codec_handle, _volume);
        esp_codec_dev_set_in_gain(_codec_handle, 24);
    }

    ESP_LOGI(TAG, "Audio initialized: %s, vol=%d", _has_lcd ? "ES8311 + ES7210" : "ES8311 (single-chip)", _volume);
    _refcount = 1;
    xSemaphoreGive(_lifecycle_mutex);
}

void AudioDriver::deinit(void)
{
    if (!_lifecycle_mutex) return;
    xSemaphoreTake(_lifecycle_mutex, portMAX_DELAY);

    if (_refcount <= 0) {
        ESP_LOGW(TAG, "Audio refcount already 0, skipping deinit");
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }
    _refcount--;
    if (_refcount > 0) {
        ESP_LOGI(TAG, "Audio still in use (refcount=%d), skipping deinit", _refcount);
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }

    ESP_LOGI(TAG, "Deinitializing audio...");

    if (_codec_handle) {
        esp_codec_dev_close(_codec_handle);
        esp_codec_dev_delete(_codec_handle);
        _codec_handle = nullptr;
    }
    if (_codec_mic_handle) {
        esp_codec_dev_close(_codec_mic_handle);
        esp_codec_dev_delete(_codec_mic_handle);
        _codec_mic_handle = nullptr;
    }

    if (_tx_handle) {
        i2s_del_channel(_tx_handle);
        _tx_handle = nullptr;
    }
    if (_rx_handle) {
        i2s_del_channel(_rx_handle);
        _rx_handle = nullptr;
    }

    gpio_set_level((gpio_num_t)53, 0);

    /* Nullify codec handles first so any in-flight codec operation
     * (set_volume / set_mic_gain / codec_write) will skip the
     * xSemaphoreTake(_codec_mutex, ...) path and exit quickly.
     * Then yield to let any such operation finish before we delete
     * the mutex — deleting a semaphore with waiters is UB. */
    if (_codec_mutex) {
        xSemaphoreGive(_lifecycle_mutex);   /* let other tasks run */
        vTaskDelay(pdMS_TO_TICKS(10));      /* wait for in-flight ops to exit */
        xSemaphoreTake(_lifecycle_mutex, portMAX_DELAY);
        vSemaphoreDelete(_codec_mutex);
        _codec_mutex = nullptr;
    }

    _vol_pub = ORB_ADVERT_INVALID;

    ESP_LOGI(TAG, "Audio deinitialized");
    xSemaphoreGive(_lifecycle_mutex);
}

/*============================================================================
 * Thread-safe codec operations
 *============================================================================*/
void AudioDriver::set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    if (_codec_handle && _codec_mutex &&
        xSemaphoreTake(_codec_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _volume = volume;  /* Store inside mutex for thread safety */
        esp_codec_dev_set_out_vol(_codec_handle, volume);
        xSemaphoreGive(_codec_mutex);
    }

    /* Publish volume_state via uORB for cross-module notification */
    if (_vol_pub < 0) {
        _vol_pub = orb_advertise(ORB_ID(volume_state));
    }
    if (_vol_pub >= 0) {
        struct volume_state_s vs = {};
        vs.timestamp = esp_timer_get_time();
        vs.volume = volume;
        orb_publish(ORB_ID(volume_state), _vol_pub, &vs);
    }
}

void AudioDriver::set_mic_gain(int gain_db)
{
    if (_codec_mic_handle && _codec_mutex &&
        xSemaphoreTake(_codec_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        esp_codec_dev_set_in_gain(_codec_mic_handle, gain_db);
        xSemaphoreGive(_codec_mutex);
    }
}

int AudioDriver::codec_write(const uint8_t *data, int size)
{
    if (!_codec_handle || !_codec_mutex || size <= 0) return -1;
    if (xSemaphoreTake(_codec_mutex, pdMS_TO_TICKS(50)) != pdPASS) return -1;
    int ret = esp_codec_dev_write(_codec_handle, (void *)data, size);
    xSemaphoreGive(_codec_mutex);
    return (ret == ESP_CODEC_DEV_OK) ? size : -1;
}
