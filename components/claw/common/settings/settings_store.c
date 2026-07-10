/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "settings_store.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "settings_store";

static claw_kv_backend_t *s_backend;

esp_err_t settings_store_init(const settings_store_config_t *config)
{
    if (!config || !config->namespace_name || config->namespace_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->backend) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = config->backend->init(&config->backend->ctx,
                                          config->namespace_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backend init(%s) failed: %s",
                 config->namespace_name, esp_err_to_name(err));
        return err;
    }

    s_backend = config->backend;
    return ESP_OK;
}

esp_err_t settings_store_get_string(const char *key,
                                    char *buf,
                                    size_t buf_size,
                                    const char *default_value)
{
    if (!s_backend) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_backend->get_str(s_backend->ctx, key, buf, buf_size,
                              default_value);
}

esp_err_t settings_store_has_key(const char *key, bool *exists)
{
    if (!s_backend) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_backend->has_key(s_backend->ctx, key, exists);
}

esp_err_t settings_store_set_string(const char *key, const char *value)
{
    if (!s_backend) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_backend->set_str(s_backend->ctx, key, value);
}

esp_err_t settings_store_erase_key(const char *key)
{
    if (!s_backend) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_backend->erase_key(s_backend->ctx, key);
}

esp_err_t settings_store_get_blob(const char *key,
                                  void **buf, size_t *len)
{
    if (!s_backend) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_backend->get_blob(s_backend->ctx, key, buf, len);
}

esp_err_t settings_store_set_blob(const char *key,
                                  const void *data, size_t len)
{
    if (!s_backend) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_backend->set_blob(s_backend->ctx, key, data, len);
}

esp_err_t settings_store_commit(void)
{
    if (!s_backend) {
        return ESP_ERR_INVALID_STATE;
    }
    return s_backend->commit(s_backend->ctx);
}
