/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Abstract key-value storage backend interface.
 *
 * Components that need persistent storage accept a claw_kv_backend_t*
 * and call through it, remaining decoupled from any concrete storage
 * implementation (NVS, filesystem, SPIFFS, etc.).
 */
typedef struct claw_kv_backend {
    /** Opaque context pointer set by the backend implementation. */
    void *ctx;

    /** Initialize the backend with a namespace. Called once. */
    esp_err_t (*init)(void **ctx, const char *namespace_name);

    /** Get a string value. If key not found and default_value is set,
     *  buf is filled with default_value and ESP_OK is returned. */
    esp_err_t (*get_str)(void *ctx, const char *key,
                         char *buf, size_t buf_size,
                         const char *default_value);

    /** Set a string value. */
    esp_err_t (*set_str)(void *ctx, const char *key, const char *value);

    /** Check if a key exists. */
    esp_err_t (*has_key)(void *ctx, const char *key, bool *exists);

    /** Erase a key. No-op if key does not exist. */
    esp_err_t (*erase_key)(void *ctx, const char *key);

    /** Get a blob value. *buf is allocated by the backend; caller must free(). */
    esp_err_t (*get_blob)(void *ctx, const char *key,
                          void **buf, size_t *len);

    /** Set a blob value. */
    esp_err_t (*set_blob)(void *ctx, const char *key,
                          const void *data, size_t len);

    /** Commit pending writes to storage. */
    esp_err_t (*commit)(void *ctx);

    /** Deinitialize and free backend resources. */
    esp_err_t (*deinit)(void *ctx);
} claw_kv_backend_t;

#ifdef __cplusplus
}
#endif
