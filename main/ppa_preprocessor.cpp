#include "ppa_preprocessor.hpp"

#if CONFIG_SOC_PPA_SUPPORTED

#include "esp_log.h"
#include <cmath>

static const char *TAG = "PPAPreprocessor";

static inline size_t align_up(size_t val, size_t align)
{
    return (val + align - 1) & ~(align - 1);
}

/** Compute PPA-compatible scale factor (8-bit int + 4-bit frac, step=1/16). */
static float get_ppa_scale(uint16_t src, uint16_t dst)
{
    if (src == dst) return 1.0f;
    float scale = (float)dst / (float)src;
    if (!(scale >= 0.0625f && scale < 256.0f)) {
        ESP_LOGE(TAG, "PPA scale out of range: %d→%d (scale=%.2f)", src, dst, scale);
        return -1.0f;
    }
    float scale_int;
    float scale_frac = modff(scale, &scale_int);
    scale_frac = floorf(scale_frac / 0.0625f) * 0.0625f;
    return scale_int + scale_frac;
}

/** Compute actual output dimension from PPA scale (matches PPA hardware calculation). */
static inline uint16_t ppa_actual_dim(uint16_t src, float scale)
{
    /* PPA hardware: new_block = scale_int * in_block + scale_frag * in_block / 16 */
    uint32_t scale_int = (uint32_t)scale;
    uint32_t scale_frag = ((uint32_t)(scale * 16)) & 0xF;
    return (uint16_t)(scale_int * src + scale_frag * src / 16);
}

bool PPAPreprocessor::init(uint16_t src_w, uint16_t src_h, uint16_t dst_w, uint16_t dst_h)
{
    if (_ppa_handle) {
        ESP_LOGW(TAG, "Already initialized, deinit first");
        deinit();
    }

    _src_w = src_w;
    _src_h = src_h;
    _dst_w = dst_w;
    _dst_h = dst_h;

    /* Compute PPA scale factors (4-bit fractional precision) */
    _scale_x = get_ppa_scale(src_w, dst_w);
    _scale_y = get_ppa_scale(src_h, dst_h);
    if (_scale_x < 0 || _scale_y < 0) {
        ESP_LOGE(TAG, "Invalid PPA scale: %.4f x %.4f", _scale_x, _scale_y);
        return false;
    }

    /* Compute actual PPA output dimensions.
     * PPA may produce fewer pixels than requested due to scale quantization.
     * E.g., 800→320: ideal_scale=0.4, ppa_scale=0.375, actual=300 (not 320). */
    _actual_w = ppa_actual_dim(src_w, _scale_x);
    _actual_h = ppa_actual_dim(src_h, _scale_y);
    ESP_LOGI(TAG, "PPA scale: %d×%d → %d×%d (requested %d×%d, scale=%.4f,%.4f)",
             src_w, src_h, _actual_w, _actual_h, dst_w, dst_h, _scale_x, _scale_y);

    /* Allocate PPA output buffer for the REQUESTED size (model input shape).
     * PPA writes actual_w×actual_h valid pixels; the remainder (right/bottom
     * border) stays zero from calloc. Passing actual_w×actual_h as
     * img dimensions ensures correct stride for downstream consumers.
     * RGB565 output: 2 bytes per pixel. */
    size_t align = cache_hal_get_cache_line_size(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_DATA);
    _out_buf_size = align_up((size_t)dst_w * dst_h * 2, align);
    _out_buf = (uint8_t *)heap_caps_aligned_calloc(align, 1, _out_buf_size,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!_out_buf) {
        ESP_LOGE(TAG, "Failed to alloc PPA output buffer (%zu bytes)", _out_buf_size);
        return false;
    }
    esp_cache_msync(_out_buf, _out_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /* Register PPA SRM client */
    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    esp_err_t ret = ppa_register_client(&ppa_cfg, &_ppa_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPA client: %s", esp_err_to_name(ret));
        heap_caps_free(_out_buf);
        _out_buf = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "Initialized: %d×%d RGB565 → %d×%d RGB565 (contiguous, model=%d×%d with letterbox)",
             src_w, src_h, _actual_w, _actual_h, dst_w, dst_h);
    return true;
}

void PPAPreprocessor::deinit()
{
    if (_ppa_handle) {
        ppa_unregister_client(_ppa_handle);
        _ppa_handle = nullptr;
    }
    if (_out_buf) {
        heap_caps_free(_out_buf);
        _out_buf = nullptr;
    }
    _out_buf_size = 0;
    _src_w = _src_h = _dst_w = _dst_h = _actual_w = _actual_h = 0;
    _scale_x = _scale_y = 1.0f;
}

bool PPAPreprocessor::process(const uint8_t *rgb565_buf)
{
    if (!_ppa_handle || !_out_buf) {
        ESP_LOGE(TAG, "Not initialized");
        return false;
    }

    /* Cache sync: source written by CPU, make visible to PPA DMA */
    esp_cache_msync((void *)rgb565_buf, (size_t)_src_w * _src_h * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    ppa_srm_oper_config_t srm_cfg = {};

    /* Input: RGB565LE */
    srm_cfg.in.buffer = rgb565_buf;
    srm_cfg.in.pic_w = _src_w;
    srm_cfg.in.pic_h = _src_h;
    srm_cfg.in.block_w = _src_w;
    srm_cfg.in.block_h = _src_h;
    srm_cfg.in.block_offset_x = 0;
    srm_cfg.in.block_offset_y = 0;
    srm_cfg.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    /* Output: RGB565 (same format as input, resize only).
     * Use actual_w/actual_h as the output picture dimensions so PPA writes
     * contiguous data with the correct row stride.
     * E.g., 800→400: scale=0.5, actual=400 (exact). */
    srm_cfg.out.buffer = _out_buf;
    srm_cfg.out.buffer_size = _out_buf_size;
    srm_cfg.out.pic_w = _actual_w;
    srm_cfg.out.pic_h = _actual_h;
    srm_cfg.out.block_offset_x = 0;
    srm_cfg.out.block_offset_y = 0;
    srm_cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    /* Scale + no rotation/mirror */
    srm_cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    srm_cfg.scale_x = _scale_x;
    srm_cfg.scale_y = _scale_y;
    srm_cfg.mirror_x = false;
    srm_cfg.mirror_y = false;

    /* No byte swap for RGB565LE input.
     * No RGB swap on output — same RGB565 format as input. */
    srm_cfg.rgb_swap = false;
    srm_cfg.byte_swap = false;

    /* Blocking mode — waits for PPA hardware to finish */
    srm_cfg.mode = PPA_TRANS_MODE_BLOCKING;

    esp_err_t ret = ppa_do_scale_rotate_mirror(_ppa_handle, &srm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA SRM failed: %s", esp_err_to_name(ret));
        return false;
    }

    /* Cache sync: PPA wrote to _out_buf, make visible to CPU */
    esp_cache_msync(_out_buf, _out_buf_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    return true;
}

#endif // CONFIG_SOC_PPA_SUPPORTED
