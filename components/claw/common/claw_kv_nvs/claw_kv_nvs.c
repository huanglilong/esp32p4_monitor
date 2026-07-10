/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_kv_nvs.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "claw_kv_nvs";

typedef struct {
    char namespace_name[16];
    bool initialized;
} claw_kv_nvs_ctx_t;

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t open_nvs_readonly(claw_kv_nvs_ctx_t *ctx,
                                   nvs_handle_t *out_handle)
{
    if (!ctx || !ctx->initialized || !out_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    return nvs_open(ctx->namespace_name, NVS_READONLY, out_handle);
}

static esp_err_t open_nvs_readwrite(claw_kv_nvs_ctx_t *ctx,
                                    nvs_handle_t *out_handle)
{
    if (!ctx || !ctx->initialized || !out_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    return nvs_open(ctx->namespace_name, NVS_READWRITE, out_handle);
}



/* ------------------------------------------------------------------ */
/*  Interface implementation                                           */
/* ------------------------------------------------------------------ */

static esp_err_t nvs_init(void **ctx_out, const char *namespace_name)
{
    if (!ctx_out || !namespace_name || namespace_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    claw_kv_nvs_ctx_t *ctx = calloc(1, sizeof(claw_kv_nvs_ctx_t));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }

    if (strlcpy(ctx->namespace_name, namespace_name,
                sizeof(ctx->namespace_name)) >= sizeof(ctx->namespace_name)) {
        free(ctx);
        return ESP_ERR_INVALID_SIZE;
    }
    ctx->initialized = true;

    /* Probe: try to open read-only to see if namespace exists */
    nvs_handle_t probe_handle = 0;
    esp_err_t err = nvs_open(ctx->namespace_name, NVS_READONLY, &probe_handle);
    if (err == ESP_OK) {
        nvs_close(probe_handle);
        *ctx_out = ctx;
        return ESP_OK;
    }

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Create namespace by opening read-write */
        err = nvs_open(ctx->namespace_name, NVS_READWRITE, &probe_handle);
        if (err == ESP_OK) {
            nvs_close(probe_handle);
            *ctx_out = ctx;
            return ESP_OK;
        }
    }

    free(ctx);
    return err;
}

static esp_err_t backend_get_str(void *ctx, const char *key,
                             char *buf, size_t buf_size,
                             const char *default_value)
{
    claw_kv_nvs_ctx_t *c = (claw_kv_nvs_ctx_t *)ctx;
    if (!c || !key || !buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    buf[0] = '\0';

    nvs_handle_t handle = 0;
    esp_err_t err = open_nvs_readonly(c, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (default_value) {
            strlcpy(buf, default_value, buf_size);
        }
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = buf_size;
    err = nvs_get_str(handle, key, buf, &required_size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (default_value) {
            strlcpy(buf, default_value, buf_size);
        }
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_str(%s) ns=%s failed: %s",
                 key, c->namespace_name, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t backend_set_str(void *ctx, const char *key, const char *value)
{
    claw_kv_nvs_ctx_t *c = (claw_kv_nvs_ctx_t *)ctx;
    if (!c || !key) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = open_nvs_readwrite(c, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, key, value ? value : "");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s) ns=%s failed: %s",
                 key, c->namespace_name, esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit ns=%s failed: %s",
                 c->namespace_name, esp_err_to_name(err));
    }
    nvs_close(handle);
    return err;
}

static esp_err_t backend_has_key(void *ctx, const char *key, bool *exists)
{
    claw_kv_nvs_ctx_t *c = (claw_kv_nvs_ctx_t *)ctx;
    if (!c || !key || !exists) {
        return ESP_ERR_INVALID_ARG;
    }

    *exists = false;

    nvs_handle_t handle = 0;
    esp_err_t err = open_nvs_readonly(c, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = 0;
    err = nvs_get_str(handle, key, NULL, &required_size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err == ESP_OK) {
        *exists = true;
    }
    return err;
}

static esp_err_t backend_erase_key(void *ctx, const char *key)
{
    claw_kv_nvs_ctx_t *c = (claw_kv_nvs_ctx_t *)ctx;
    if (!c || !key) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = open_nvs_readwrite(c, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_erase_key(%s) ns=%s failed: %s",
                 key, c->namespace_name, esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit ns=%s failed: %s",
                 c->namespace_name, esp_err_to_name(err));
    }
    nvs_close(handle);
    return err;
}

static esp_err_t backend_get_blob(void *ctx, const char *key,
                              void **buf, size_t *len)
{
    claw_kv_nvs_ctx_t *c = (claw_kv_nvs_ctx_t *)ctx;
    if (!c || !key || !buf || !len) {
        return ESP_ERR_INVALID_ARG;
    }

    *buf = NULL;
    *len = 0;

    nvs_handle_t handle = 0;
    esp_err_t err = open_nvs_readonly(c, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_blob(handle, key, NULL, len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(size) ns=%s key=%s failed: %s",
                 c->namespace_name, key, esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    void *data = calloc(1, *len ? *len : 1);
    if (!data) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_blob(handle, key, data, len);
    nvs_close(handle);

    if (err != ESP_OK) {
        free(data);
        return err;
    }

    *buf = data;
    return ESP_OK;
}

static esp_err_t backend_set_blob(void *ctx, const char *key,
                              const void *data, size_t len)
{
    claw_kv_nvs_ctx_t *c = (claw_kv_nvs_ctx_t *)ctx;
    if (!c || !key || !data) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = open_nvs_readwrite(c, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, key, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob ns=%s key=%s failed: %s",
                 c->namespace_name, key, esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit ns=%s failed: %s",
                 c->namespace_name, esp_err_to_name(err));
    }
    nvs_close(handle);
    return err;
}

static esp_err_t backend_commit(void *ctx)
{
    (void)ctx;
    return ESP_OK;
}

static esp_err_t nvs_deinit(void *ctx)
{
    claw_kv_nvs_ctx_t *c = (claw_kv_nvs_ctx_t *)ctx;
    if (c) {
        memset(c->namespace_name, 0, sizeof(c->namespace_name));
        c->initialized = false;
        free(c);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Public factory                                                     */
/* ------------------------------------------------------------------ */

claw_kv_backend_t *claw_kv_nvs_create(void)
{
    claw_kv_backend_t *backend = calloc(1, sizeof(claw_kv_backend_t));
    if (!backend) {
        return NULL;
    }

    backend->ctx       = NULL;
    backend->init      = nvs_init;
    backend->get_str   = backend_get_str;
    backend->set_str   = backend_set_str;
    backend->has_key   = backend_has_key;
    backend->erase_key = backend_erase_key;
    backend->get_blob  = backend_get_blob;
    backend->set_blob  = backend_set_blob;
    backend->commit    = backend_commit;
    backend->deinit    = nvs_deinit;

    return backend;
}
