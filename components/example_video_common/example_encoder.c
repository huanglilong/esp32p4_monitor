/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_video_ioctl.h"
#include "esp_video_init.h"
#include "esp_jpeg_enc.h"
#include "example_video_common.h"

typedef struct example_encoder {
    jpeg_enc_handle_t jpeg_handle;
    uint32_t jpeg_out_buf_size;
} example_encoder_t;

static const char *TAG = "example_encoder";

esp_err_t example_encoder_init(example_encoder_config_t *config, example_encoder_handle_t *ret_handle)
{
    if (!config || !ret_handle) {
        ESP_LOGE(TAG, "invalid argument");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    example_encoder_t *encoder;
    uint32_t jpeg_enc_input_src_size;
    jpeg_enc_handle_t jpeg_handle = NULL;
    jpeg_enc_config_t jpeg_enc_config = {0};

    jpeg_enc_config.height = config->height;
    jpeg_enc_config.width = config->width;
    jpeg_enc_config.quality = config->quality;

    switch (config->pixel_format) {
    case V4L2_PIX_FMT_SBGGR8:
    case V4L2_PIX_FMT_GREY:
        jpeg_enc_config.src_type = JPEG_PIXEL_FORMAT_GRAY;
        jpeg_enc_config.subsampling = JPEG_SUBSAMPLE_GRAY;
        jpeg_enc_input_src_size = config->width * config->height;
        break;
    case V4L2_PIX_FMT_RGB565:
        jpeg_enc_config.src_type = JPEG_PIXEL_FORMAT_RGB565_LE;
        jpeg_enc_config.subsampling = JPEG_SUBSAMPLE_422;
        jpeg_enc_input_src_size = config->width * config->height * 2;
        break;
    case V4L2_PIX_FMT_RGB24:
        jpeg_enc_config.src_type = JPEG_PIXEL_FORMAT_RGB888;
        jpeg_enc_config.subsampling = JPEG_SUBSAMPLE_422;
        jpeg_enc_input_src_size = config->width * config->height * 3;
        break;
    case V4L2_PIX_FMT_YUV422P:
        jpeg_enc_config.src_type = JPEG_PIXEL_FORMAT_YCbYCr;
        jpeg_enc_config.subsampling = JPEG_SUBSAMPLE_422;
        jpeg_enc_input_src_size = config->width * config->height * 2;
        break;
    default:
        ESP_LOGE(TAG, "Unsupported format 0x%08" PRIx32, config->pixel_format);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_RETURN_ON_ERROR(jpeg_enc_open(&jpeg_enc_config, &jpeg_handle), TAG, "failed to open jpeg encoder");

    encoder = (example_encoder_t *)calloc(1, sizeof(example_encoder_t));
    ESP_GOTO_ON_FALSE(encoder, ESP_ERR_NO_MEM, fail0, TAG, "failed to alloc example encoder");

    encoder->jpeg_handle = jpeg_handle;
    encoder->jpeg_out_buf_size = jpeg_enc_input_src_size * 3 / 4;

    *ret_handle = encoder;
    return ESP_OK;

fail0:
    jpeg_enc_close(jpeg_handle);
    return ret;
}

esp_err_t example_encoder_alloc_output_buffer(example_encoder_handle_t handle, uint8_t **buf, uint32_t *size)
{
    uint8_t *jpeg_out_buf;
    example_encoder_t *encoder = (example_encoder_t *)handle;
    if (!encoder || !buf || !size) {
        ESP_LOGE(TAG, "invalid argument");
        return ESP_ERR_INVALID_ARG;
    }

    jpeg_out_buf = jpeg_calloc_align(encoder->jpeg_out_buf_size, 128);
    ESP_RETURN_ON_FALSE(jpeg_out_buf, ESP_ERR_NO_MEM, TAG, "failed to alloc jpeg output buf");

    *buf = jpeg_out_buf;
    *size = encoder->jpeg_out_buf_size;
    return ESP_OK;
}

esp_err_t example_encoder_free_output_buffer(example_encoder_handle_t handle, uint8_t *buf)
{
    if (!buf || !handle) {
        ESP_LOGE(TAG, "invalid argument");
        return ESP_ERR_INVALID_ARG;
    }
    jpeg_free_align(buf);
    return ESP_OK;
}

esp_err_t example_encoder_process(example_encoder_handle_t handle, uint8_t *src_buf, uint32_t src_size,
                                   uint8_t *dst_buf, uint32_t dst_size, uint32_t *dst_size_out)
{
    if (!handle || !src_buf || !src_size || !dst_buf || !dst_size || !dst_size_out) {
        return ESP_ERR_INVALID_ARG;
    }

    example_encoder_t *encoder = (example_encoder_t *)handle;
    return jpeg_enc_process(encoder->jpeg_handle, src_buf, src_size, dst_buf, dst_size, (int *)dst_size_out);
}

esp_err_t example_encoder_set_jpeg_quality(example_encoder_handle_t handle, uint8_t quality)
{
    example_encoder_t *encoder = (example_encoder_t *)handle;
    if (!encoder) {
        ESP_LOGE(TAG, "example encoder is not initialized");
        return ESP_ERR_INVALID_ARG;
    }
    return jpeg_enc_set_quality(encoder->jpeg_handle, quality);
}

esp_err_t example_encoder_deinit(example_encoder_handle_t handle)
{
    example_encoder_t *encoder = (example_encoder_t *)handle;
    if (!encoder) {
        ESP_LOGE(TAG, "example encoder is not initialized");
        return ESP_ERR_INVALID_ARG;
    }
    jpeg_enc_close(encoder->jpeg_handle);
    free(encoder);
    return ESP_OK;
}
