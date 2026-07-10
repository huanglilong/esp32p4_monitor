#pragma once

#include "sdkconfig.h"

#if CONFIG_SOC_PPA_SUPPORTED

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "hal/cache_hal.h"
#include "hal/cache_ll.h"
#include <cstdint>

/**
 * PPAPreprocessor — Hardware-accelerated image preprocessing using ESP32-P4 PPA.
 *
 * Offloads resize + color conversion from CPU to PPA hardware:
 *   RGB565 (any resolution) → BGR888 (model input resolution, e.g. 320×320)
 *
 * PPA scale uses 4-bit fractional precision (step = 1/16 = 0.0625).
 * If the ideal scale is not exactly representable, the actual output size
 * will be smaller than requested (e.g., 800→320 requests 0.4 but PPA
 * quantizes to 0.375, producing 300×300 output with black border).
 *
 * This class tracks the actual output dimensions and provides them
 * for correct coordinate rescaling and letterbox handling.
 *
 * Note: PPA_SRM_COLOR_MODE_RGB888 outputs BGR24 in memory (Byte0=B,G,R).
 *
 * Usage:
 *   PPAPreprocessor ppa;
 *   ppa.init(800, 800, 320, 320);
 *   ppa.process(rgb565_buf);
 *   // Use actual_width()/actual_height() for img descriptor and rescale
 *   ppa.deinit();
 */
class PPAPreprocessor {
public:
    PPAPreprocessor() = default;
    ~PPAPreprocessor() { deinit(); }

    bool init(uint16_t src_w, uint16_t src_h, uint16_t dst_w, uint16_t dst_h);
    void deinit();
    bool process(const uint8_t *rgb565_buf);

    /** PPA output buffer (BGR888, allocated for dst_w*dst_h*3) */
    uint8_t *out_buf() const { return _out_buf; }
    size_t out_buf_size() const { return _out_buf_size; }

    /** Requested destination width (model input, e.g. 320) */
    uint16_t dst_width() const { return _dst_w; }
    uint16_t dst_height() const { return _dst_h; }

    /**
     * ACTUAL PPA output width. May be less than dst_width() due to
     * 4-bit scale quantization. Use for img descriptor and coordinate rescaling.
     * E.g., 800→320: scale=0.375, actual_width=300 (not 320).
     */
    uint16_t actual_width() const { return _actual_w; }
    uint16_t actual_height() const { return _actual_h; }

    float scale_x() const { return _scale_x; }
    float scale_y() const { return _scale_y; }
    bool is_initialized() const { return _ppa_handle != nullptr; }

private:
    ppa_client_handle_t _ppa_handle = nullptr;
    uint8_t *_out_buf = nullptr;
    size_t   _out_buf_size = 0;
    uint16_t _src_w = 0, _src_h = 0;
    uint16_t _dst_w = 0, _dst_h = 0;
    uint16_t _actual_w = 0, _actual_h = 0;
    float    _scale_x = 1.0f, _scale_y = 1.0f;
};

#endif // CONFIG_SOC_PPA_SUPPORTED
