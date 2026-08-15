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
    _codec_mutex(SemaphoreHandle_t(nullptr)),
    _has_lcd(false),
    _refcount(0),
    _volume(EXAMPLE_VOICE_VOLUME),
    _codec_handle(nullptr),
    _codec_mic_handle(nullptr),
    _rx_handle(nullptr),
    _tx_handle(nullptr),
    _vol_pub(ORB_ADVERT_INVALID),
    _codec_ops_in_flight(0)
{
    _lifecycle_mutex = xSemaphoreCreateMutex();
}

AudioDriver::~AudioDriver()
{
    if (_lifecycle_mutex) {
        vSemaphoreDelete(_lifecycle_mutex);
        _lifecycle_mutex = nullptr;
    }
    SemaphoreHandle_t cm = _codec_mutex.exchange(nullptr, std::memory_order_acq_rel);
    if (cm) {
        vSemaphoreDelete(cm);
    }
}

/*============================================================================
 * Audio I2S + Codec lifecycle
 *============================================================================*/
void AudioDriver::init(void)
{
    if (!_lifecycle_mutex) return;
    xSemaphoreTake(_lifecycle_mutex, portMAX_DELAY);

    if (_refcount.load(std::memory_order_relaxed) > 0) {
        _refcount.fetch_add(1, std::memory_order_relaxed);
        ESP_LOGI(TAG, "Audio already initialized (refcount=%d)", _refcount.load(std::memory_order_relaxed));
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }

    bool has_lcd = _has_lcd.load(std::memory_order_relaxed);
    ESP_LOGI(TAG, "Initializing audio (%s)...", has_lcd ? "ES8311 + ES7210" : "ES8311 single-chip");

    /* Enable PA GPIO 53 (critical for speaker output) */
    gpio_config_t pa_conf = {
        .pin_bit_mask = (1ULL << AUDIO_PA_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&pa_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to config PA GPIO %d: %s — speaker will be silent", AUDIO_PA_GPIO, esp_err_to_name(ret));
        /* Continue init — audio codec/I2S may still work for mic input,
         * but speaker output will be non-functional without PA enabled. */
    } else {
        gpio_set_level((gpio_num_t)AUDIO_PA_GPIO, 1);
        ESP_LOGI(TAG, "PA GPIO %d enabled", AUDIO_PA_GPIO);
    }

    /* I2S channel init (duplex, STD, 16kHz stereo) */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    i2s_chan_handle_t tx_h = nullptr, rx_h = nullptr;
    ret = i2s_new_channel(&chan_cfg, &tx_h, &rx_h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        gpio_set_level((gpio_num_t)AUDIO_PA_GPIO, 0);
        ESP_LOGE(TAG, "Audio initialization failed — audio unavailable");
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }
    _tx_handle.store(tx_h, std::memory_order_release);
    _rx_handle.store(rx_h, std::memory_order_release);

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)AUDIO_I2S_MCK_IO,
            .bclk = (gpio_num_t)AUDIO_I2S_BCK_IO,
            .ws   = (gpio_num_t)AUDIO_I2S_WS_IO,
            .dout = (gpio_num_t)AUDIO_I2S_DO_IO,
            .din  = (gpio_num_t)AUDIO_I2S_DI_IO,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = EXAMPLE_AUDIO_MCLK_MULTIPLE;
    ret = i2s_channel_init_std_mode(tx_h, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S TX std mode: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_h); i2s_del_channel(rx_h);
        _tx_handle.store(nullptr, std::memory_order_release);
        _rx_handle.store(nullptr, std::memory_order_release);
        gpio_set_level((gpio_num_t)AUDIO_PA_GPIO, 0);
        ESP_LOGE(TAG, "Audio initialization failed — audio unavailable");
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }
    ret = i2s_channel_init_std_mode(rx_h, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S RX std mode: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_h); i2s_del_channel(rx_h);
        _tx_handle.store(nullptr, std::memory_order_release);
        _rx_handle.store(nullptr, std::memory_order_release);
        gpio_set_level((gpio_num_t)AUDIO_PA_GPIO, 0);
        ESP_LOGE(TAG, "Audio initialization failed — audio unavailable");
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }
    ret = i2s_channel_enable(tx_h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S TX: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_h); i2s_del_channel(rx_h);
        _tx_handle.store(nullptr, std::memory_order_release);
        _rx_handle.store(nullptr, std::memory_order_release);
        gpio_set_level((gpio_num_t)AUDIO_PA_GPIO, 0);
        ESP_LOGE(TAG, "Audio initialization failed — audio unavailable");
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }
    ret = i2s_channel_enable(rx_h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S RX: %s", esp_err_to_name(ret));
        i2s_del_channel(tx_h); i2s_del_channel(rx_h);
        _tx_handle.store(nullptr, std::memory_order_release);
        _rx_handle.store(nullptr, std::memory_order_release);
        gpio_set_level((gpio_num_t)AUDIO_PA_GPIO, 0);
        ESP_LOGE(TAG, "Audio initialization failed — audio unavailable");
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }

    i2c_master_bus_handle_t i2c_handle = bsp_i2c_get_handle();
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    bool init_ok = true;

    if (!gpio_if) {
        ESP_LOGE(TAG, "Failed to create codec GPIO interface");
        init_ok = false;
    }

    if (init_ok && has_lcd) {
        /* ===== LCD-4B: ES8311 DAC (Speaker, 0x30) + ES7210 ADC (Mic, 0x80) ===== */
        audio_codec_i2s_cfg_t i2s_out_cfg = {
            .port = 0,
            .rx_handle = NULL,
            .tx_handle = _tx_handle.load(std::memory_order_relaxed),
        };
        const audio_codec_data_if_t *data_out = audio_codec_new_i2s_data(&i2s_out_cfg);
        if (!data_out) {
            ESP_LOGE(TAG, "Failed to create I2S data output interface");
            init_ok = false;
        }

        const audio_codec_data_if_t *data_in = nullptr;
        if (init_ok) {
            audio_codec_i2s_cfg_t i2s_in_cfg = {
                .port = 0,
                .rx_handle = _rx_handle.load(std::memory_order_relaxed),
                .tx_handle = NULL,
            };
            data_in = audio_codec_new_i2s_data(&i2s_in_cfg);
            if (!data_in) {
                ESP_LOGE(TAG, "Failed to create I2S data input interface");
                init_ok = false;
            }
        }

        if (init_ok) {
            audio_codec_i2c_cfg_t i2c_dac = { .port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_handle };
            const audio_codec_ctrl_if_t *ctrl_dac = audio_codec_new_i2c_ctrl(&i2c_dac);
            if (!ctrl_dac) {
                ESP_LOGE(TAG, "Failed to create I2C control interface for DAC");
                init_ok = false;
            }
            if (init_ok) {
                es8311_codec_cfg_t es8311_cfg = {
                    .ctrl_if = ctrl_dac, .gpio_if = gpio_if,
                    .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
                    .pa_pin = AUDIO_PA_GPIO, .pa_reverted = false, .master_mode = false, .use_mclk = true,
                    .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
                    .mclk_div = I2S_MCLK_MULTIPLE_256,
                };
                esp_codec_dev_cfg_t dev_dac = {
                    .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es8311_codec_new(&es8311_cfg), .data_if = data_out
                };
                _codec_handle.store(esp_codec_dev_new(&dev_dac), std::memory_order_relaxed);
                if (!_codec_handle.load(std::memory_order_relaxed)) {
                    ESP_LOGE(TAG, "Failed to create ES8311 DAC codec device");
                    init_ok = false;
                }
            }
        }

        if (init_ok) {
            audio_codec_i2c_cfg_t i2c_adc = { .port = 0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = i2c_handle };
            const audio_codec_ctrl_if_t *ctrl_adc = audio_codec_new_i2c_ctrl(&i2c_adc);
            if (!ctrl_adc) {
                ESP_LOGE(TAG, "Failed to create I2C control interface for ADC");
                init_ok = false;
            }
            if (init_ok) {
            es7210_codec_cfg_t es7210_cfg = {
                .ctrl_if = ctrl_adc, .master_mode = false,
                .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
                .mclk_src = ES7210_MCLK_FROM_PAD, .mclk_div = I2S_MCLK_MULTIPLE_256,
            };
            esp_codec_dev_cfg_t dev_adc = {
                .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = es7210_codec_new(&es7210_cfg), .data_if = data_in
            };
            _codec_mic_handle.store(esp_codec_dev_new(&dev_adc), std::memory_order_relaxed);
            if (!_codec_mic_handle.load(std::memory_order_relaxed)) {
                ESP_LOGE(TAG, "Failed to create ES7210 ADC codec device");
                /* DAC was created successfully, delete it before rollback */
                esp_codec_dev_handle_t dac_handle = _codec_handle.load(std::memory_order_relaxed);
                if (dac_handle) {
                    esp_codec_dev_delete(dac_handle);
                    _codec_handle.store(nullptr, std::memory_order_relaxed);
                }
                init_ok = false;
            }
        }
        }
    } else if (init_ok) {
        /* ===== WIFI6: ES8311 single-chip (0x30, ADC + DAC) + NS4150B amp ===== */
        audio_codec_i2s_cfg_t i2s_data_cfg = {
            .port = 0,
            .rx_handle = _rx_handle.load(std::memory_order_relaxed),
            .tx_handle = _tx_handle.load(std::memory_order_relaxed),
        };
        const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
        if (!data_if) {
            ESP_LOGE(TAG, "Failed to create I2S data interface");
            init_ok = false;
        }

        if (init_ok) {
            audio_codec_i2c_cfg_t i2c_es8311 = { .port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = i2c_handle };
            const audio_codec_ctrl_if_t *ctrl_es8311 = audio_codec_new_i2c_ctrl(&i2c_es8311);
            if (!ctrl_es8311) {
                ESP_LOGE(TAG, "Failed to create I2C control interface for ES8311");
                init_ok = false;
            }
            if (init_ok) {
            es8311_codec_cfg_t es8311_cfg = {
                .ctrl_if = ctrl_es8311, .gpio_if = gpio_if,
                .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
                .pa_pin = AUDIO_PA_GPIO, .pa_reverted = false, .master_mode = false, .use_mclk = true,
                .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
                .mclk_div = I2S_MCLK_MULTIPLE_256,
            };
            esp_codec_dev_cfg_t dev_cfg = {
                .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
                .codec_if = es8311_codec_new(&es8311_cfg),
                .data_if = data_if,
            };
            _codec_handle.store(esp_codec_dev_new(&dev_cfg), std::memory_order_relaxed);
            if (!_codec_handle.load(std::memory_order_relaxed)) {
                ESP_LOGE(TAG, "Failed to create ES8311 codec device");
                init_ok = false;
            }
        }
        }
        /* _codec_mic_handle stays NULL */
    }

    if (!init_ok) {
        /* Rollback: clean up any partially-created resources */
        esp_codec_dev_handle_t mic_h = _codec_mic_handle.load(std::memory_order_relaxed);
        if (mic_h) {
            esp_codec_dev_delete(mic_h);
            _codec_mic_handle.store(nullptr, std::memory_order_relaxed);
        }
        esp_codec_dev_handle_t codec_h = _codec_handle.load(std::memory_order_relaxed);
        if (codec_h) {
            esp_codec_dev_delete(codec_h);
            _codec_handle.store(nullptr, std::memory_order_relaxed);
        }
        i2s_chan_handle_t tx_h = _tx_handle.load(std::memory_order_relaxed);
        if (tx_h) {
            i2s_del_channel(tx_h);
            _tx_handle.store(nullptr, std::memory_order_release);
        }
        i2s_chan_handle_t rx_h = _rx_handle.load(std::memory_order_relaxed);
        if (rx_h) {
            i2s_del_channel(rx_h);
            _rx_handle.store(nullptr, std::memory_order_release);
        }
        gpio_set_level((gpio_num_t)AUDIO_PA_GPIO, 0);
        ESP_LOGE(TAG, "Audio initialization failed — audio unavailable");
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }

    /* Open codecs — track which ones were successfully opened so rollback
     * only calls esp_codec_dev_close() on opened handles.
     * Note: _codec_mutex is created AFTER successful codec open to prevent
     * concurrent codec ops from seeing a valid mutex during init rollback. */
    bool codec_dac_opened = false;
    bool codec_mic_opened = false;
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 2, .channel_mask = 0x03, .sample_rate = EXAMPLE_AUDIO_SAMPLE_RATE };
    esp_codec_dev_handle_t codec_h = _codec_handle.load(std::memory_order_relaxed);
    esp_codec_dev_handle_t mic_h = _codec_mic_handle.load(std::memory_order_relaxed);
    if (has_lcd) {
        if (esp_codec_dev_open(codec_h, &fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Failed to open ES8311 DAC codec");
            init_ok = false;
        } else {
            codec_dac_opened = true;
            esp_codec_dev_set_out_vol(codec_h, _volume.load(std::memory_order_relaxed));
        }
        if (init_ok && esp_codec_dev_open(mic_h, &fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Failed to open ES7210 ADC codec");
            init_ok = false;
        } else if (init_ok) {
            codec_mic_opened = true;
            esp_codec_dev_set_in_gain(mic_h, 42);
        }
    } else {
        if (esp_codec_dev_open(codec_h, &fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Failed to open ES8311 codec");
            init_ok = false;
        } else {
            codec_dac_opened = true;
            esp_codec_dev_set_out_vol(codec_h, _volume.load(std::memory_order_relaxed));
            esp_codec_dev_set_in_gain(codec_h, 24);
        }
    }

    if (!init_ok) {
        /* Rollback codec open failure — only close handles that were opened */
        mic_h = _codec_mic_handle.load(std::memory_order_relaxed);
        if (mic_h) {
            if (codec_mic_opened) esp_codec_dev_close(mic_h);
            esp_codec_dev_delete(mic_h);
            _codec_mic_handle.store(nullptr, std::memory_order_relaxed);
        }
        codec_h = _codec_handle.load(std::memory_order_relaxed);
        if (codec_h) {
            if (codec_dac_opened) esp_codec_dev_close(codec_h);
            esp_codec_dev_delete(codec_h);
            _codec_handle.store(nullptr, std::memory_order_relaxed);
        }
        i2s_chan_handle_t tx_h2 = _tx_handle.load(std::memory_order_relaxed);
        if (tx_h2) {
            i2s_del_channel(tx_h2);
            _tx_handle.store(nullptr, std::memory_order_release);
        }
        i2s_chan_handle_t rx_h2 = _rx_handle.load(std::memory_order_relaxed);
        if (rx_h2) {
            i2s_del_channel(rx_h2);
            _rx_handle.store(nullptr, std::memory_order_release);
        }
        gpio_set_level((gpio_num_t)AUDIO_PA_GPIO, 0);
        ESP_LOGE(TAG, "Audio codec open failed — audio unavailable");
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }

    ESP_LOGI(TAG, "Audio initialized: %s, vol=%d", has_lcd ? "ES8311 + ES7210" : "ES8311 (single-chip)", _volume.load(std::memory_order_relaxed));
    /* Create codec mutex AFTER successful init — prevents concurrent codec ops
     * from seeing a valid mutex during init/rollback. */
    SemaphoreHandle_t codec_mutex = xSemaphoreCreateMutex();
    if (!codec_mutex) {
        ESP_LOGE(TAG, "Failed to create codec mutex — audio will be non-functional");
        /* Rollback: close codec devices, disable I2S, release PA GPIO */
        if (_codec_handle.load(std::memory_order_relaxed)) {
            esp_codec_dev_close(_codec_handle.load(std::memory_order_relaxed));
            esp_codec_dev_delete(_codec_handle.load(std::memory_order_relaxed));
            _codec_handle.store(nullptr, std::memory_order_relaxed);
        }
        if (_codec_mic_handle.load(std::memory_order_relaxed)) {
            esp_codec_dev_close(_codec_mic_handle.load(std::memory_order_relaxed));
            esp_codec_dev_delete(_codec_mic_handle.load(std::memory_order_relaxed));
            _codec_mic_handle.store(nullptr, std::memory_order_relaxed);
        }
        i2s_chan_handle_t rx_h3 = _rx_handle.load(std::memory_order_relaxed);
        if (rx_h3) { i2s_channel_disable(rx_h3); i2s_del_channel(rx_h3); _rx_handle.store(nullptr, std::memory_order_release); }
        i2s_chan_handle_t tx_h3 = _tx_handle.load(std::memory_order_relaxed);
        if (tx_h3) { i2s_channel_disable(tx_h3); i2s_del_channel(tx_h3); _tx_handle.store(nullptr, std::memory_order_release); }
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }
    _codec_mutex.store(codec_mutex, std::memory_order_release);
    _refcount.store(1, std::memory_order_relaxed);
    xSemaphoreGive(_lifecycle_mutex);
}

void AudioDriver::deinit(void)
{
    if (!_lifecycle_mutex) return;
    xSemaphoreTake(_lifecycle_mutex, portMAX_DELAY);

    if (_refcount.load(std::memory_order_relaxed) <= 0) {
        ESP_LOGW(TAG, "Audio refcount already 0, skipping deinit");
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }
    int remaining = _refcount.fetch_sub(1, std::memory_order_relaxed) - 1;
    if (remaining > 0) {
        ESP_LOGI(TAG, "Audio still in use (refcount=%d), skipping deinit", remaining);
        xSemaphoreGive(_lifecycle_mutex);
        return;
    }

    ESP_LOGI(TAG, "Deinitializing audio...");

    /* Step 1: Atomically nullify _codec_mutex to prevent NEW codec operations.
     * Any in-flight op that already loaded the old mutex pointer can still
     * proceed — we wait for them below. */
    SemaphoreHandle_t old_codec_mutex = _codec_mutex.exchange(nullptr, std::memory_order_acq_rel);
    if (old_codec_mutex) {
        /* Wait for in-flight codec ops to finish (they decrement _codec_ops_in_flight
         * on exit). Spin with yields to avoid busy-wait burning CPU. */
        for (int i = 0; i < 200 && _codec_ops_in_flight.load(std::memory_order_acquire) > 0; i++) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (_codec_ops_in_flight.load(std::memory_order_acquire) > 0) {
            ESP_LOGW(TAG, "Deinit: %d codec ops still in-flight after 1s, proceeding anyway",
                     _codec_ops_in_flight.load());
        }
    }

    /* Step 2: Now safe to close/delete codec handles — no one can
     * acquire the mutex (it's null) to perform codec operations. */
    esp_codec_dev_handle_t c_handle = _codec_handle.exchange(nullptr, std::memory_order_acq_rel);
    if (c_handle) {
        esp_codec_dev_close(c_handle);
        esp_codec_dev_delete(c_handle);
    }
    esp_codec_dev_handle_t c_mic_handle = _codec_mic_handle.exchange(nullptr, std::memory_order_acq_rel);
    if (c_mic_handle) {
        esp_codec_dev_close(c_mic_handle);
        esp_codec_dev_delete(c_mic_handle);
    }

    i2s_chan_handle_t tx_h4 = _tx_handle.exchange(nullptr, std::memory_order_acq_rel);
    if (tx_h4) {
        i2s_del_channel(tx_h4);
    }
    i2s_chan_handle_t rx_h4 = _rx_handle.exchange(nullptr, std::memory_order_acq_rel);
    if (rx_h4) {
        i2s_del_channel(rx_h4);
    }

    gpio_set_level((gpio_num_t)AUDIO_PA_GPIO, 0);

    /* Step 3: Delete the old mutex — no one references it anymore. */
    if (old_codec_mutex) {
        vSemaphoreDelete(old_codec_mutex);
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

    SemaphoreHandle_t cm = _codec_mutex.load(std::memory_order_acquire);
    esp_codec_dev_handle_t ch = _codec_handle.load(std::memory_order_acquire);
    if (!ch || !cm) {
        ESP_LOGW(TAG, "set_volume(%d) skipped: codec/mutex unavailable", volume);
        return;
    }

    _codec_ops_in_flight.fetch_add(1, std::memory_order_acq_rel);
    /* Re-read cm after incrementing counter — deinit() waits for counter=0
     * before deleting the mutex, so our cm is guaranteed valid. */
    cm = _codec_mutex.load(std::memory_order_acquire);
    if (!cm) {
        _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        ESP_LOGW(TAG, "set_volume(%d) skipped: mutex nullified during entry", volume);
        return;
    }

    if (xSemaphoreTake(cm, pdMS_TO_TICKS(100)) == pdTRUE) {
        /* Re-read codec handle under mutex in case it changed */
        esp_codec_dev_handle_t ch2 = _codec_handle.load(std::memory_order_relaxed);
        if (ch2) {
            _volume = volume;  /* Store inside mutex for thread safety */
            esp_codec_dev_set_out_vol(ch2, volume);
            xSemaphoreGive(cm);
        } else {
            xSemaphoreGive(cm);
            _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);
            ESP_LOGW(TAG, "set_volume(%d) skipped: codec handle nullified", volume);
            return;
        }
    } else {
        _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        ESP_LOGW(TAG, "set_volume(%d) skipped: mutex timeout", volume);
        return;
    }
    _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);

    /* Publish volume_state via uORB for cross-module notification */
    if (_vol_pub.load(std::memory_order_relaxed) < 0) {
        orb_advert_t new_pub = orb_advertise(ORB_ID(volume_state));
        orb_advert_t expected_vp = ORB_ADVERT_INVALID;
        _vol_pub.compare_exchange_strong(expected_vp, new_pub,
                std::memory_order_acq_rel, std::memory_order_acquire);
        /* If CAS fails, another thread already set up the publisher handle —
         * our unused handle has an extra refcount on the topic but is harmless. */
    }
    if (_vol_pub.load(std::memory_order_relaxed) >= 0) {
        struct volume_state_s vs = {};
        vs.timestamp = esp_timer_get_time();
        vs.volume = volume;
        orb_publish(ORB_ID(volume_state), _vol_pub, &vs);
    }
}

void AudioDriver::set_mic_gain(int gain_db)
{
    SemaphoreHandle_t cm = _codec_mutex.load(std::memory_order_acquire);
    if (!cm) return;

    /* WIFI6 boards use _codec_handle for both ADC and DAC;
     * _codec_mic_handle is NULL on WIFI6. */
    esp_codec_dev_handle_t mic_h = _codec_mic_handle.load(std::memory_order_acquire);
    esp_codec_dev_handle_t out_h = _codec_handle.load(std::memory_order_acquire);
    esp_codec_dev_handle_t h = mic_h ? mic_h : out_h;
    if (!h) return;

    _codec_ops_in_flight.fetch_add(1, std::memory_order_acq_rel);
    cm = _codec_mutex.load(std::memory_order_acquire);
    if (!cm) {
        _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    if (xSemaphoreTake(cm, pdMS_TO_TICKS(100)) == pdTRUE) {
        /* Re-read under mutex */
        mic_h = _codec_mic_handle.load(std::memory_order_relaxed);
        out_h = _codec_handle.load(std::memory_order_relaxed);
        h = mic_h ? mic_h : out_h;
        if (h) esp_codec_dev_set_in_gain(h, gain_db);
        xSemaphoreGive(cm);
    }
    _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);
}

int AudioDriver::codec_write(const uint8_t *data, int size)
{
    if (size <= 0) return -1;
    SemaphoreHandle_t cm = _codec_mutex.load(std::memory_order_acquire);
    esp_codec_dev_handle_t ch = _codec_handle.load(std::memory_order_acquire);
    if (!ch || !cm) return -1;

    _codec_ops_in_flight.fetch_add(1, std::memory_order_acq_rel);
    cm = _codec_mutex.load(std::memory_order_acquire);
    if (!cm) {
        _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        return -1;
    }

    if (xSemaphoreTake(cm, pdMS_TO_TICKS(50)) != pdPASS) {
        _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        return -1;
    }
    /* Re-read under mutex */
    esp_codec_dev_handle_t ch2 = _codec_handle.load(std::memory_order_relaxed);
    if (!ch2) { xSemaphoreGive(cm); _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel); return -1; }
    int ret = esp_codec_dev_write(ch2, (void *)data, size);
    xSemaphoreGive(cm);
    _codec_ops_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    return (ret == ESP_CODEC_DEV_OK) ? size : -1;
}
