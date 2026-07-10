/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_kv_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create a claw_kv_backend_t backed by ESP-IDF NVS.
 *
 * @return  Pointer to a fully initialized backend, or NULL on failure.
 *          Caller must eventually call backend->deinit(backend->ctx)
 *          and free() the backend struct.
 */
claw_kv_backend_t *claw_kv_nvs_create(void);

#ifdef __cplusplus
}
#endif
